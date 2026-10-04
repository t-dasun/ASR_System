#!/usr/bin/env python3
"""Build an evidence-bounded M10 report from explicit local M9 runs."""
import argparse
from datetime import datetime, timezone
import hashlib
from html import escape
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.evaluation.compare_m9_pairs import compare, read_jsonl
from tools.reports.capacity import analyze_screens

VERSION = "m10-evidence-v1"
TARGETS = (50, 100, 200, 500, 1000)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source(path, *, sealed=False):
    path = path.resolve()
    if not path.is_file():
        raise FileNotFoundError(path)
    return {"path": str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path),
            "sha256": digest(path), "sealed_raw": sealed}


def verified_seal(directory):
    if json.loads((directory / "status.json").read_text())["status"] != "COMPLETE":
        raise ValueError("measured experiment is not complete")
    checksums = json.loads((directory / "checksums.json").read_text())
    if not checksums:
        raise ValueError("empty seal")
    for name, expected in checksums.items():
        path = (directory / name).resolve()
        if not path.is_relative_to(directory.resolve()) or not path.is_file() or digest(path) != expected:
            raise ValueError(f"raw seal mismatch: {name}")
    return {"file_count": len(checksums), "manifest": source(directory / "checksums.json", sealed=True)}


def build_report(holdout, load, stress, threads, capacity_screens=()):
    measured = holdout / "measured"
    seal = verified_seal(measured)
    manifest = read_jsonl(holdout / "combined.jsonl")
    raw = read_jsonl(measured / "accuracy.jsonl")
    experiment = json.loads((measured / "experiment.json").read_text())
    if digest(holdout / "combined.jsonl") != experiment["manifest_sha256"]:
        raise ValueError("holdout manifest differs from measured experiment")
    paired = compare(manifest, raw)
    if len(paired["pairs"]) != 15:
        raise ValueError("expected 15 complete holdout pairs")
    summary = json.loads((measured / "summary.json").read_text())
    if not summary.get("checks_passed") or summary.get("completed_calls") != 30:
        raise ValueError("holdout quality gate failed")
    load_status = json.loads((load / "status.json").read_text())
    load_summary = json.loads((load / "summary.json").read_text())
    if load_status.get("status") != "COMPLETE" or load_summary.get("is_mock"):
        raise ValueError("load screen is incomplete or mock")
    stress_audit = json.loads(stress.read_text())
    if not stress_audit.get("checks_passed"):
        raise ValueError("stress audit failed")
    thread_compare = json.loads(threads.read_text())
    if thread_compare.get("factor") != "model.threads":
        raise ValueError("unexpected configuration comparison")
    load_configs = sorted(load.glob("*/config.json"))
    if len(load_configs) != load_summary["metrics"]["offered_calls"]:
        raise ValueError("load call configurations missing")
    stress_config = Path(stress_audit["run"]) / "config.json"
    if not stress_config.is_file():
        raise ValueError("stress run configuration missing")
    thread_experiments = [Path(thread_compare[side]) / "experiment.json" for side in ("left", "right")]
    if any(not path.is_file() for path in thread_experiments):
        raise ValueError("thread comparison experiment missing")
    capacity = analyze_screens(capacity_screens) if capacity_screens else None
    capacity_sources = [{"run_id": directory.name,
                          "plan": source(directory / "plan.json"),
                          "summary": source(directory / "summary.json"),
                          "phase_resources": [source(path) for path in sorted(directory.glob("phase_*_system_metrics.jsonl"))],
                          "call_configs": [source(path) for path in sorted(directory.glob("*/config.json"))]}
                         for directory in capacity_screens] if capacity else []
    accuracy = {}
    for language, row in paired["by_language"].items():
        accuracy[language] = {"metric": row["metric"], "paired_clips": row["pairs"],
                              "reference_units": row["reference_units"],
                              "clean_rate": row["clean_failure_inclusive_rate"],
                              "simulated_telephone_rate": row["telephone_failure_inclusive_rate"],
                              "clean_failures": row["clean_failures"],
                              "simulated_telephone_failures": row["telephone_failures"],
                              "clean_p50_first_ms": row["clean_p50_first_ms"],
                              "simulated_telephone_p50_first_ms": row["telephone_p50_first_ms"]}
    metrics = load_summary["metrics"]
    sizing = [{"target_legs": legs, "classification": "INSUFFICIENT_EVIDENCE",
               "safe_legs_per_node": None, "node_count": None, "node_specification": None,
               "vcpu_definition": "one logical CPU hardware thread; not portable across CPU models",
               "vcpu_per_node": None,
               "ram_gib_per_node": None, "target_rtf": None, "target_p95_ms": None,
               "headroom": None, "assumptions": ["No identical-node extrapolation is made."],
               "basis_run_ids": [row["run_id"] for row in capacity["rows"]] if capacity else [load.name],
               "confidence": "none",
            "reason": "No measured saturation knee, sustained concurrent run, declared production SLO, or validated headroom."}
              for legs in TARGETS]
    return {
        "schema_version": 1, "generator_version": VERSION,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "status": "EVIDENCE_REPORT_PARTIAL", "qualification": "NOT_PRODUCTION_SIZED",
        "sources": {
            "holdout_run_id": holdout.name, "holdout_path": str(measured.relative_to(ROOT)),
            "holdout_seal": seal, "holdout_manifest": source(holdout / "combined.jsonl"),
            "holdout_accuracy": source(measured / "accuracy.jsonl", sealed=True),
            "holdout_experiment": source(measured / "experiment.json", sealed=True),
            "load_run_id": load.name, "load_summary": source(load / "summary.json"),
            "load_plan": source(load / "plan.json"),
            "load_call_configs": [source(path) for path in load_configs],
            "stress_run_id": Path(stress_audit["run"]).name, "stress_audit": source(stress),
            "stress_config": source(stress_config),
            "threads_comparison": source(threads),
            "thread_experiments": [source(path) for path in thread_experiments],
            "capacity_screens": capacity_sources},
        "configuration": {"model": "Qwen/Qwen3-ASR-0.6B CPU",
                          "holdout_config_sha256": experiment["config_sha256"],
                          "load_call_config_sha256": [digest(path) for path in load_configs],
                          "stress_config_sha256": digest(stress_config),
                          "thread_experiment_config_sha256": [json.loads(path.read_text())["config_sha256"] for path in thread_experiments],
                          "thread_factor": thread_compare["factor"],
                          "left_threads": thread_compare["left_threads"],
                          "right_threads": thread_compare["right_threads"]},
        "holdout": {"calls": 30, "clean_telephone_pairs": 15,
                    "by_language": accuracy,
                    "overall_first_usable_p50_ms": summary["latency"]["first_usable_transcript_ns"]["p50"] / 1e6,
                    "overall_first_usable_p95_ms": summary["latency"]["first_usable_transcript_ns"]["p95"] / 1e6,
                    "scope": "FLEURS validation: five pairs per language; telephone is simulated, not real calls. WER and CER are separate."},
        "concurrency_screen": {"offered_calls": metrics["offered_calls"],
                               "completed_calls": metrics["completed_calls"],
                               "peak_sampled_tree_rss_bytes": metrics["sampled_peak_tree_rss_bytes"],
                               "first_usable_p95_ms": metrics["first_usable_ms"]["p95"],
                               "final_p95_ms": metrics["final_result_ms"]["p95"],
                               "qualified": load_summary["slo"]["qualified"],
                               "scope": "Three 1.2-second fixture calls, two workers, staggered arrival; screening only. Suite has no raw checksum seal."},
        "stress_screen": {"duration_seconds": stress_audit["duration_seconds"],
                          "first_usable_ms": stress_audit["first_usable_ms"],
                          "final_result_ms": stress_audit["final_result_ms"],
                          "scope": stress_audit["scope"]},
        "threads_screen": {"by_language": thread_compare["by_language"], "scope": thread_compare["scope"]},
        "capacity_screen": capacity,
        "feasibility": {"status": "NOT_EVALUABLE", "reason": "No chosen product latency, accuracy, error, and memory limits; holdout is small and no qualified capacity curve exists."},
        "strict_subsecond": {"status": "NOT_DEMONSTRATED", "observed_holdout_p50_ms": summary["latency"]["first_usable_transcript_ns"]["p50"] / 1e6,
                             "target_ms": 1000, "scope": "First usable transcript from paced stream start, not per-200-ms decoder invocation."},
        "sizing": sizing,
        "next_evidence": ["Declare product SLOs and explicit vCPU/node/RAM target.",
                          "Run sealed, repeated native sweeps to a measured saturation knee with staggered calls and realistic language/duration mix.",
                          "Add sustained multi-call/endurance and recovery runs with queue, errors, CPU, RSS, swap, and thermal telemetry.",
                          "Validate a 50-per-language held-out cohort and real telephony/Common Voice when access exists."]}


def html_report(report):
    esc = lambda value: escape(str(value))
    rows = []
    for language, item in report["holdout"]["by_language"].items():
        rows.append(f"<tr><td>{esc(language.upper())}</td><td>{esc(item['metric'].upper())}</td><td>{item['paired_clips']}</td><td>{item['clean_rate']:.1%}</td><td>{item['simulated_telephone_rate']:.1%}</td><td>{item['clean_p50_first_ms']/1000:.2f} s</td></tr>")
    sizes = "".join(f"<tr><td>{row['target_legs']}</td><td>Insufficient evidence</td>"
                    "<td>Unknown</td><td>Unknown</td><td>Unknown</td><td>Unknown</td></tr>"
                    for row in report["sizing"])
    capacity = report.get("capacity_screen")
    capacity_html = ""
    if capacity:
        screen_rows = "".join(f"<tr><td>{row['concurrency']}</td><td>{row['calls']}/{row['completed_calls']}</td>"
                              f"<td>{row['audio_seconds_per_wall_second']:.2f}</td>"
                              f"<td>{row['first_usable_p95_ms']/1000:.2f} s</td>"
                              f"<td>{row['sampled_peak_tree_rss_bytes']/2**30:.2f} GiB</td></tr>"
                              for row in capacity["rows"])
        capacity_html = ("<h2>Bounded native load comparison</h2><p>Twenty repeats of one 1.2-second English WAV "
                         "at each point; diagnostic zero-failure and 1-second send-lag thresholds only. "
                         "These short screens do not establish a safe calls-per-node value.</p>"
                         "<table><tr><th>Workers</th><th>Offered/completed</th><th>Audio s / wall s</th>"
                         "<th>P95 first text</th><th>Peak sampled RSS</th></tr>" + screen_rows + "</table>"
                         f"<p>Two versus one: {capacity['throughput_ratio_2_vs_1']:.2f}x audio throughput; "
                         f"P95 first text +{capacity['first_p95_delta_ms_2_vs_1']:.0f} ms. "
                         "No saturation knee observed; test duration was under one minute per point.</p>")
    refs = "".join(f"<li><code>{esc(name)}</code>: {esc(value['path'])} <small>SHA-256 {esc(value['sha256'][:16])}...</small></li>"
                   for name, value in report["sources"].items() if isinstance(value, dict) and "path" in value)
    return f"""<!doctype html><html lang="en"><meta charset="utf-8"><title>M10 evidence and sizing</title>
<style>body{{font:16px/1.5 system-ui,sans-serif;max-width:950px;margin:36px auto;padding:0 20px;color:#182232}}h1,h2{{color:#103568}}.tag{{background:#fff0cf;padding:10px;border-left:4px solid #b87900}}table{{border-collapse:collapse;width:100%;margin:14px 0 22px}}th,td{{border:1px solid #c7d0dc;padding:8px;text-align:left}}th{{background:#e9eff7}}code{{overflow-wrap:anywhere}}small{{color:#4c5a69}}</style>
<h1>M10 - evidence and capacity sizing</h1><p class="tag">PARTIAL EVIDENCE - no qualified CPU-only production capacity or node count yet.</p>
<p>Generated {esc(report['generated_utc'])}. Native Qwen3-ASR-0.6B; holdout config SHA-256 <code>{esc(report['configuration']['holdout_config_sha256'])}</code>.</p>
<h2>Held-out FLEURS screen</h2><p>30 calls, 15 clean/simulated-telephone pairs; five pairs per language. All completed. WER and CER are not pooled. This sample is not a production accuracy qualification.</p>
<table><tr><th>Language</th><th>Metric</th><th>Pairs</th><th>Clean</th><th>Sim. telephone</th><th>Clean P50 first text</th></tr>{''.join(rows)}</table>
<p>Across all 30 calls, first usable text P50 {report['holdout']['overall_first_usable_p50_ms']/1000:.2f} s, P95 {report['holdout']['overall_first_usable_p95_ms']/1000:.2f} s. These pooled latencies are descriptive; the conditions differ.</p>
<h2>Concurrency and long-form screens</h2><p>Two-worker staggered screen: {report['concurrency_screen']['completed_calls']}/{report['concurrency_screen']['offered_calls']} completed; P95 first text {report['concurrency_screen']['first_usable_p95_ms']/1000:.2f} s; sampled peak process-tree RSS {report['concurrency_screen']['peak_sampled_tree_rss_bytes']/2**30:.2f} GiB. Only three short calls: tail and capacity are unqualified.</p>
<p>Single-call synthetic replay: {report['stress_screen']['duration_seconds']:.2f} s audio with silence and interruptions; first text {report['stress_screen']['first_usable_ms']/1000:.2f} s. This is not concurrent endurance or conversational accuracy.</p>
{capacity_html}
<h2>Strict sub-second partials</h2><p>Not demonstrated: observed held-out median first usable text is {report['strict_subsecond']['observed_holdout_p50_ms']/1000:.2f} s from stream start. A 200 ms transport chunk is not a 200 ms transcription latency guarantee.</p>
<h2>CPU-only sizing guide</h2><p>No safe calls per node, measured saturation knee, sustained multi-call run, declared product SLO, or justified headroom. Node counts, vCPU and RAM therefore remain unknown; these rows are neither measured capacity nor extrapolated estimates.</p>
<table><tr><th>Concurrent legs</th><th>Evidence class</th><th>Nodes</th><th>vCPU / RAM</th><th>RTF / P95</th><th>Headroom</th></tr>{sizes}</table>
<p>vCPU means one logical CPU hardware thread and is not portable between processor models. Node specification, safe legs/node, target RTF/P95, memory, failure reserve, and headroom cannot be selected without product criteria and a sustained safe envelope. All rows cite the screening run IDs in <code>report.json</code>; confidence is none.</p>
<h2>Evidence needed next</h2><ol>{''.join('<li>'+esc(item)+'</li>' for item in report['next_evidence'])}</ol>
<h2>Source artifacts</h2><p>Run IDs: {esc(report['sources']['load_run_id'])}; {esc(report['sources']['stress_run_id'])}. Holdout raw seal verified ({report['sources']['holdout_seal']['file_count']} files). Derived load/audit artifacts are hash-recorded here but are not sealed raw runs.</p><ul>{refs}</ul>
</html>"""


def write_outputs(report, output):
    output.mkdir(parents=True, exist_ok=False)
    for name, obj in (("report.json", report), ("report_metadata.json", {"schema_version": 1,
            "generator_version": VERSION, "generated_utc": report["generated_utc"],
            "sources": report["sources"], "report_json_sha256": None})):
        (output / name).write_text(json.dumps(obj, indent=2, ensure_ascii=False, allow_nan=False) + "\n")
    metadata = json.loads((output / "report_metadata.json").read_text())
    metadata["report_json_sha256"] = digest(output / "report.json")
    (output / "report_metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (output / "report.html").write_text(html_report(report), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--holdout", type=Path, required=True)
    parser.add_argument("--load", type=Path, required=True)
    parser.add_argument("--stress-audit", type=Path, required=True)
    parser.add_argument("--threads-comparison", type=Path, required=True)
    parser.add_argument("--capacity-screen", type=Path, nargs=2,
                        help="comparable 20-call native load suites at concurrency 1 and 2")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = build_report(args.holdout.resolve(), args.load.resolve(),
                          args.stress_audit.resolve(), args.threads_comparison.resolve(),
                          [path.resolve() for path in args.capacity_screen] if args.capacity_screen else ())
    write_outputs(report, args.output.resolve())
    print(args.output.resolve())


if __name__ == "__main__":
    main()
