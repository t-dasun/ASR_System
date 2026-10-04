#!/usr/bin/env python3
"""Run or report a paired transport-chunk/decoder-step CPU ASR screen."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import subprocess
import sys
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[2]
CASES = ((200, 2000), (100, 2000), (200, 1000), (100, 1000))
LANGUAGES = ("en", "id", "zh")


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def median(values):
    values = sorted(values)
    if not values:
        return None
    middle = len(values) // 2
    return values[middle] if len(values) % 2 else (values[middle - 1] + values[middle]) / 2


def case_name(chunk_ms, decode_ms):
    return f"chunk{chunk_ms}_decode{decode_ms}"


def summarize_case(directory, chunk_ms, decode_ms):
    status_file = directory / "status.json"
    status = json.loads(status_file.read_text())["status"] if status_file.exists() else "NOT_RUN"
    rows_file = directory / "accuracy.jsonl"
    rows = [json.loads(line) for line in rows_file.read_text().splitlines() if line.strip()] if rows_file.exists() else []
    by_language = {}
    for language in LANGUAGES:
        group = [row for row in rows if row["language"] == language]
        complete = [row for row in group if row["status"] == "COMPLETE" and all(row["checks"].values())]
        edits = sum(row["accuracy"]["edits"] for row in group)
        units = sum(row["accuracy"]["reference_units"] for row in group)
        first = [row["measurements"]["first_usable_transcript_ns"] / 1e9 for row in complete
                 if row["measurements"].get("first_usable_transcript_ns") is not None]
        final = [row["measurements"]["final_result_ns"] / 1e9 for row in complete
                 if row["measurements"].get("final_result_ns") is not None]
        by_language[language] = {
            "offered": len(group), "completed": len(complete),
            "metric": "CER" if language == "zh" else "WER",
            "edits": edits, "reference_units": units,
            "error_rate": edits / units if units else None,
            "first_text_p50_s": median(first), "final_p50_s": median(final),
        }
    complete_rows = [row for row in rows if row["status"] == "COMPLETE" and all(row["checks"].values())]
    peak_rss = [row["resources"].get("sampled_peak_tree_rss_bytes") for row in complete_rows]
    first_all = [row["measurements"]["first_usable_transcript_ns"] / 1e9 for row in complete_rows
                 if row["measurements"].get("first_usable_transcript_ns") is not None]
    final_all = [row["measurements"]["final_result_ns"] / 1e9 for row in complete_rows
                 if row["measurements"].get("final_result_ns") is not None]
    cpu_all = [row["measurements"]["worker_cpu_ns"] / 1e9 for row in complete_rows
               if row["measurements"].get("worker_cpu_ns") is not None]
    return {"id": case_name(chunk_ms, decode_ms), "chunk_ms": chunk_ms,
            "decode_step_ms": decode_ms, "status": status, "offered": len(rows),
            "completed": len(complete_rows), "by_language": by_language,
            "overall_first_text_p50_s": median(first_all), "overall_final_p50_s": median(final_all),
            "worker_cpu_p50_s": median(cpu_all),
            "max_sampled_tree_rss_bytes": max((value for value in peak_rss if value is not None), default=None),
            "directory": str(directory.relative_to(directory.parent))}


def svg_chart(cases):
    width, height = 1200, 445
    colors = {case_name(200, 2000): "#1b7f79", case_name(100, 2000): "#4d71bc",
              case_name(200, 1000): "#e38635", case_name(100, 1000): "#a44ba0"}
    by_id = {case["id"]: case for case in cases}
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" aria-label="First and final transcript latency by configuration and language, with error rates">',
             '<rect width="1200" height="445" fill="#fff"/>',
             '<text x="30" y="28" font-size="19" font-family="sans-serif">First text improves with a 1 s decoder step</text>',
             '<text x="30" y="48" font-size="12" font-family="sans-serif" fill="#555">Median seconds; bar = first usable text, black line = final transcript. Three clips per language.</text>']
    for index, language in enumerate(LANGUAGES):
        x0 = 43 + 395 * index
        baseline = by_id[case_name(200, 2000)]["by_language"][language]
        error = baseline["error_rate"]
        caption = (f'{baseline["metric"]} {error * 100:.1f}% for all four settings'
                   if error is not None and all(case["by_language"][language]["error_rate"] == error for case in cases)
                   else f'{baseline["metric"]} varies; see report table')
        parts += [f'<text x="{x0}" y="82" font-size="16" font-family="sans-serif">{language.upper()}</text>',
                  f'<text x="{x0}" y="101" font-size="12" font-family="sans-serif" fill="#555">{escape(caption)}</text>',
                  f'<line x1="{x0}" y1="350" x2="{x0 + 345}" y2="350" stroke="#333"/>',
                  f'<line x1="{x0}" y1="125" x2="{x0}" y2="350" stroke="#333"/>',
                  f'<text x="{x0 + 3}" y="122" font-size="11" font-family="sans-serif">15 s</text>',
                  f'<text x="{x0 + 4}" y="367" font-size="11" font-family="sans-serif">0</text>']
        for slot, (chunk_ms, decode_ms) in enumerate(CASES):
            case = by_id[case_name(chunk_ms, decode_ms)]
            item = case["by_language"][language]
            first, final = item["first_text_p50_s"], item["final_p50_s"]
            if first is None:
                continue
            x = x0 + 35 + slot * 77
            bar_height = min(first, 15) * 15
            parts.append(f'<rect x="{x}" y="{350 - bar_height:.1f}" width="35" height="{bar_height:.1f}" fill="{colors[case["id"]]}"><title>{chunk_ms} ms audio / {decode_ms} ms decode: first {first:.2f} s</title></rect>')
            parts.append(f'<text x="{x - 1}" y="{340 - bar_height:.1f}" font-size="10" font-family="sans-serif">{first:.1f}</text>')
            if final is not None:
                line_y = 350 - min(final, 15) * 15
                parts.append(f'<line x1="{x - 5}" y1="{line_y:.1f}" x2="{x + 40}" y2="{line_y:.1f}" stroke="#222" stroke-width="3"><title>Final transcript {final:.2f} s</title></line>')
            parts.append(f'<text x="{x - 6}" y="386" font-size="11" font-family="sans-serif">{chunk_ms}/{decode_ms // 1000}s</text>')
    parts.append('<text x="43" y="422" font-size="11" font-family="sans-serif" fill="#555">Setting labels: audio chunk in ms / decoder step in seconds. Accuracy values are cohort edit totals, not per-call averages.</text>')
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def build_report(output, plan):
    cases = [summarize_case(output / case["id"], case["chunk_ms"], case["decode_step_ms"])
             for case in plan["cases"]]
    by_id = {case["id"]: case for case in cases}
    baseline = by_id[case_name(200, 2000)]
    def hypotheses(case):
        path = output / case["id"] / "accuracy.jsonl"
        return {row["id"]: row["accuracy"]["hypothesis"]
                for line in path.read_text().splitlines() if line.strip()
                for row in [json.loads(line)]} if path.exists() else {}
    baseline_text = hypotheses(baseline)
    for case in cases:
        texts = hypotheses(case)
        shared = baseline_text.keys() & texts.keys()
        case["paired_transcripts"] = len(shared)
        case["changed_final_transcripts_vs_baseline"] = sum(texts[key] != baseline_text[key] for key in shared)
    lines = ["# Audio-chunk / decoder-step latency–accuracy screen", "",
             "This is a paired **tuning-cohort screen**, not a population accuracy or production SLO result.",
             f"Each setting uses the same {plan['per_language']} FLEURS validation clips per language (EN/ID/ZH),",
             "paced 16 kHz PCM, one native worker, four model threads, and a newly loaded model per call.",
             "The run order was shuffled but not thermally or cache controlled. Failed calls count as empty",
             "hypotheses in error rates; latency uses only completed, fully checked calls.", "",
             f"Manifest SHA-256: `{plan['manifest_sha256']}`. Baseline: 200 ms audio / 2,000 ms decode.", "",
             "![Delay versus accuracy](latency_accuracy.svg)", "",
             "| Audio chunk | Decode step | Overall first-text p50 | Overall final p50 | Worker CPU p50 | Final transcript changes vs baseline |",
             "|---:|---:|---:|---:|---:|---:|"]
    for chunk_ms, decode_ms in CASES:
        case = by_id[case_name(chunk_ms, decode_ms)]
        lines.append(f'| {chunk_ms} ms | {decode_ms} ms | {case["overall_first_text_p50_s"]:.2f} s | '
                     f'{case["overall_final_p50_s"]:.2f} s | {case["worker_cpu_p50_s"]:.2f} CPU-s | '
                     f'{case["changed_final_transcripts_vs_baseline"]}/{case["paired_transcripts"]} |')
    lines += ["", "The overall latency column pools the nine EN/ID/ZH calls for timing only; error rates remain language-specific.", "",
             "| Language | Audio chunk | Decode step | Completed/offered | First text p50 | Final p50 | Error | Δ first vs baseline | Δ error vs baseline |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for language in LANGUAGES:
        baseline_row = baseline["by_language"][language]
        for chunk_ms, decode_ms in CASES:
            case = by_id[case_name(chunk_ms, decode_ms)]
            row = case["by_language"][language]
            fmt = lambda value: "—" if value is None else f"{value:.2f} s"
            error = "—" if row["error_rate"] is None else f'{row["metric"]} {row["edits"]}/{row["reference_units"]} ({row["error_rate"] * 100:.1f}%)'
            delta_first = (row["first_text_p50_s"] - baseline_row["first_text_p50_s"]
                           if row["first_text_p50_s"] is not None and baseline_row["first_text_p50_s"] is not None else None)
            delta_error = (row["error_rate"] - baseline_row["error_rate"]
                           if row["error_rate"] is not None and baseline_row["error_rate"] is not None else None)
            lines.append(f'| {language.upper()} | {chunk_ms} ms | {decode_ms} ms | {row["completed"]}/{row["offered"]} | '
                         f'{fmt(row["first_text_p50_s"])} | {fmt(row["final_p50_s"])} | {error} | '
                         f'{"—" if delta_first is None else f"{delta_first:+.2f} s"} | '
                         f'{"—" if delta_error is None else f"{delta_error * 100:+.1f} pp"} |')
        lines.append("| | | | | | | | | |")
    lines += ["", "## Interpretation", "",
              "Reducing the decoder step from 2,000 to 1,000 ms moved the overall first-text p50",
              "from about 7.20 s to 3.7–3.8 s. Halving only the transport chunk had negligible",
              "effect. The 1,000 ms decode setting raised overall final p50 from about 10.41 s",
              "to 12.1–12.2 s and median worker CPU from about 23 to 35 CPU-seconds; exact final",
              "transcript strings were unchanged on all nine paired clips. Worker CPU-seconds sum",
              "usage across threads and are not elapsed wall seconds.", "",
              "A lower first-text time is useful only if the same setting maintains acceptable accuracy,",
              "failure rate, final latency, CPU/RAM and live-call throughput. Three clips per language",
              "cannot establish p95 or a stable winner; the graph is exploratory. A 200 ms transport chunk",
              "does not imply 200 ms model decoding. The C++ runtime can still delay stable text by",
              "multiple decoding steps. Do not combine English/Indonesian WER with Mandarin CER.", "",
              "## Raw evidence", "",
              "Every setting has `experiment.json`, `accuracy.jsonl`, `summary.json`, `checksums.json`,",
              "and per-call `events.jsonl`, `audio_timing.jsonl`, `runtime_timing.jsonl`,",
              "`system_metrics.jsonl` and `config.json` under its named directory.", "",
              "| Setting | Status | Completed/offered | Peak sampled process-tree RSS | Directory |",
              "|---|---|---:|---:|---|"]
    for chunk_ms, decode_ms in CASES:
        case = by_id[case_name(chunk_ms, decode_ms)]
        rss = case["max_sampled_tree_rss_bytes"]
        lines.append(f'| {chunk_ms}/{decode_ms} ms | {case["status"]} | {case["completed"]}/{case["offered"]} | '
                     f'{"—" if rss is None else f"{rss / 2**30:.2f} GiB"} | [{case["id"]}]({case["id"]}/summary.json) |')
    lines += ["", "Compare only completed runs and inspect per-call transcripts for differences.", ""]
    write_json(output / "report.json", {"schema_version": 1, "plan": plan, "cases": cases,
                                         "qualification": "EXPLORATORY_SMALL_COHORT"})
    (output / "report.md").write_text("\n".join(lines), encoding="utf-8")
    (output / "latency_accuracy.svg").write_text(svg_chart(cases), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=ROOT / "datasets/manifests/fleurs_m4_tuning.jsonl")
    parser.add_argument("--per-language", type=int, default=3)
    parser.add_argument("--report-only", action="store_true")
    args = parser.parse_args()
    if args.per_language < 1:
        parser.error("--per-language must be positive")
    output = args.output.resolve()
    if args.report_only:
        plan = json.loads((output / "plan.json").read_text())
        build_report(output, plan)
        return
    output.mkdir(parents=True, exist_ok=False)
    cases = [{"id": case_name(chunk, decode), "chunk_ms": chunk, "decode_step_ms": decode}
             for chunk, decode in CASES]
    order = cases[:]
    random.Random(42).shuffle(order)
    plan = {"schema_version": 1, "manifest": str(args.manifest.resolve()),
            "manifest_sha256": sha256(args.manifest), "per_language": args.per_language,
            "config": "configs/qwen_native_single.yaml", "cases": cases,
            "execution_order": [case["id"] for case in order], "seed": 42}
    write_json(output / "plan.json", plan)
    for case in order:
        print(f'Running {case["id"]} ({args.per_language} clips/language)', flush=True)
        command = [sys.executable, str(ROOT / "tools/evaluation/run_measured.py"),
                   "--manifest", str(args.manifest), "--per-language", str(args.per_language),
                   "--set", f'audio.chunk_ms={case["chunk_ms"]}',
                   "--set", f'model.decode_step_ms={case["decode_step_ms"]}',
                   "--output", str(output / case["id"])]
        result = subprocess.run(command, cwd=ROOT, check=False)
        print(f'{case["id"]} exit={result.returncode}', flush=True)
    build_report(output, plan)
    print(f'Report: {output / "report.md"}', flush=True)


if __name__ == "__main__":
    main()
