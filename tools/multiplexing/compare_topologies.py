#!/usr/bin/env python3
"""Paired topology screen. Inference, pacing, and timing use asr-cli load in C++."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import random
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from evaluation.scoring import AccuracyEvaluator
from tools.evaluation.run_measured import quantiles

IDS = ("fleurs_en_us_validation_1605_16", "fleurs_id_id_validation_1520_4",
       "fleurs_cmn_hans_cn_validation_1559_6")
CASES = {
    "native_1w_1s": ("qwen_native_single.yaml", 1, 1, 1),
    "shared_1w_1s_control": ("qwen_prefix_shared.yaml", 1, 1, 1),
    "shared_1w_2s": ("qwen_prefix_shared.yaml", 1, 2, 2),
    "native_2w_1s": ("qwen_native_single.yaml", 2, 1, 2),
}
TIMINGS = ("startup_ns", "first_partial_ns", "first_usable_transcript_ns", "final_result_ns",
           "finalization_ns", "scheduled_final_lag_ns", "model_load_ns", "shared_model_load_ns",
           "live_invocation_wall_ns", "prefix_decode_wall_ns", "eof_refinement_wall_ns",
           "prefix_decode_queue_wait_ns", "eof_decode_queue_wait_ns", "runtime_queue_wait_ns",
           "offline_decode_wall_ns", "worker_cpu_ns", "first_stable_transcript_ns",
           "first_inference_compute_ns", "partial_service_lag_ns", "inference_compute_rtf")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n")


def distribution(values, unit="ms", population="completed calls"):
    result = quantiles(values, unit)
    result["mean"] = statistics.mean(values) if values else None
    result["population"] = population
    return result


def score_calls(suite, source):
    scorer = AccuracyEvaluator()
    rows = []
    for phase in suite["phases"]:
        for item in phase["calls"]:
            summary = item.get("summary", {})
            events_path = Path(item["directory"]) / "events.jsonl"
            events = [json.loads(line) for line in events_path.read_text().splitlines()] if events_path.exists() else []
            completed = item["status"] == "COMPLETE"
            text = summary.get("transcript", "") if completed else ""
            checks = {"complete": completed, "native": summary.get("is_mock") is False,
                      "samples": summary.get("audio_samples") == source["num_samples"],
                      "one_final": sum(e["kind"] == "final" for e in events) == 1,
                      "call_isolation": all(e["call_id"] == item["run_id"] + "_call_0" for e in events),
                      "no_overflow": summary.get("audio_overflows") == 0,
                      "publication_order": all(e["published_ns"] >= e["produced_ns"] for e in events)}
            audio_path = Path(item["directory"]) / "audio_timing.jsonl"
            chunks = [json.loads(line) for line in audio_path.read_text().splitlines()] if audio_path.exists() else []
            raw_delays = {"send_lag": [c["lag_ns"] for c in chunks],
                          "controller_queue_wait": [c["sent_ns"] - c["enqueued_ns"] for c in chunks],
                          "submit_wall": [c["submit_returned_ns"] - c["submit_started_ns"] for c in chunks],
                          "publication_delay": [e["published_ns"] - e["produced_ns"] for e in events]}
            rows.append({"recording_id": source["id"], "language": source["language"],
                         "run_id": item["run_id"], "status": item["status"],
                         "worker_id": summary.get("worker_id"), "transcript": text,
                         "before_eof_text": any(e["before_eof"] and e["text"].strip() for e in events),
                         "measurements": summary.get("measurements", {}),
                         "max_send_lag_ms": summary.get("max_send_lag_ns", 0) / 1e6,
                         "accuracy": scorer.score(source["reference"], text, source["language"]),
                         "checks": checks, "events_sha256": digest(events_path) if events_path.exists() else None})
            rows[-1]["raw_delays_ns"] = raw_delays
    return rows


def summarize(plan, jobs, output):
    groups = {}
    for name, (_, workers, sessions, concurrency) in CASES.items():
        selected = [job for job in jobs if job["case"] == name]
        calls = [call for job in selected for call in job.get("calls", [])]
        timing = {key: distribution([call["measurements"][key] / 1e6 for call in calls
                                     if call["status"] == "COMPLETE" and call["measurements"].get(key) is not None]) for key in TIMINGS if key.endswith("_ns")}
        delays = {key: distribution([value / 1e6 for c in calls for value in c["raw_delays_ns"][key]],
                                    population="recognition events" if key == "publication_delay" else "delivered chunks")
                  for key in ("send_lag", "controller_queue_wait", "submit_wall", "publication_delay")}
        for key in ("effective_rtf", "offline_decode_wall_rtf", "inference_compute_rtf"):
            timing[key] = distribution([c["measurements"][key] for c in calls
                                       if c["status"] == "COMPLETE" and c["measurements"].get(key) is not None], "ratio")
        timing["shared_model_load_ns"] = distribution(
            [j["calls"][0]["measurements"]["shared_model_load_ns"] / 1e6 for j in selected
             if j.get("calls") and j["calls"][0]["measurements"].get("shared_model_load_ns") is not None],
            population="unique shared contexts, loaded once before suite")
        accuracy = {}
        for lang in ("en", "id", "zh"):
            subset = [c for c in calls if c["language"] == lang]
            units = sum(c["accuracy"]["reference_units"] for c in subset)
            edits = sum(c["accuracy"]["edits"] for c in subset)
            accuracy[lang] = {"metric": "cer" if lang == "zh" else "wer", "edits": edits,
                              "reference_units": units, "rate": edits / units if units else None,
                              "unique_recordings": len({c["recording_id"] for c in subset})}
        wall = sum(job.get("suite", {}).get("metrics", {}).get("measurement_wall_seconds", 0) for job in selected)
        audio = sum(job.get("suite", {}).get("metrics", {}).get("completed_audio_seconds", 0) for job in selected)
        external_wall = sum(job.get("external_wall_ns", 0) for job in selected) / 1e9
        groups[name] = {"workers": workers, "sessions_per_worker": sessions, "concurrency": concurrency,
                        "configured_compute_threads_total": workers * 4,
                        "offered": len(plan["recordings"]) * plan["calls_per_wav"], "recorded": len(calls),
                        "completed": sum(c["status"] == "COMPLETE" for c in calls),
                        "all_checks": len(calls) == len(plan["recordings"]) * plan["calls_per_wav"] and
                                      all(all(c["checks"].values()) for c in calls) and
                                      all(j.get("suite", {}).get("status") == "COMPLETE" for j in selected),
                        "pre_eof_text": sum(c["before_eof_text"] for c in calls), "timings": timing,
                        "delay_distributions": delays,
                        "peak_sampled_cpu_cores": max((phase.get("resources", {}).get("max_tree_cpu_core_equivalents", 0) or 0
                                                        for job in selected for phase in job.get("suite", {}).get("phases", [])), default=0),
                        "sampled_mean_cpu_cores": sum(j.get("sampled_cpu_core_seconds", 0) for j in selected) /
                                                   sum(j.get("cpu_sampled_wall_seconds", 0) for j in selected)
                                                   if sum(j.get("cpu_sampled_wall_seconds", 0) for j in selected) else None,
                        "audio_seconds_per_wall_second": audio / wall if wall else None,
                        "external_cli_wall_seconds": external_wall,
                        "audio_seconds_per_external_cli_second": audio / external_wall if external_wall else None,
                        "sampled_peak_rss_bytes": max((j.get("suite", {}).get("metrics", {}).get("sampled_peak_tree_rss_bytes", 0) or 0 for j in selected), default=0),
                        "sampled_peak_pss_bytes": max((phase.get("resources", {}).get("sampled_peak_tree_pss_bytes", 0) or 0
                                                       for j in selected for phase in j.get("suite", {}).get("phases", [])), default=0),
                        "accuracy": accuracy, "calls": calls}
    baseline = {c["recording_id"]: c["transcript"] for c in groups["native_1w_1s"]["calls"] if c["status"] == "COMPLETE"}
    for group in groups.values():
        group["final_matches_native_single"] = sum(c["transcript"] == baseline.get(c["recording_id"])
                                                    for c in group["calls"] if c["status"] == "COMPLETE")
    result = {"schema": "matched_cpp_topology_comparison_v1", "plan": plan, "groups": groups, "jobs": jobs,
              "scope": f"3 unique held-out recordings, {plan['calls_per_wav']} repetitions each per layout; paired same-utterance loads, not sustained mixed-input capacity or robust tail estimates"}
    result["status"] = "COMPLETE" if all(g["all_checks"] for g in groups.values()) else "INCOMPLETE"
    write(output / "comparison.json", result)
    lines = ["# Matched C++ topology comparison", "", result["scope"], "",
             "All inference, paced PCM, event artifacts and timings are produced by the main C++ `asr-cli load` runner. Python only invokes commands, checks hashes, scores text and formats the report.", "",
             "| Layout | Complete | Pre-EOF text | Startup mean | First text mean / p95 | Final mean / p95 | EOF delay mean | Peak RSS | Audio s / wall s |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    def fmt(value):
        return "unavailable" if value is None else f"{value / 1000:.3f} s"
    for name, g in groups.items():
        t = g["timings"]
        startup = "unavailable" if t['startup_ns']['mean'] is None else f"{t['startup_ns']['mean']:.3f} ms"
        lines.append(f"| {name} | {g['completed']}/{g['offered']} | {g['pre_eof_text']}/{g['offered']} | {startup} | "
                     f"{fmt(t['first_usable_transcript_ns']['mean'])} / {fmt(t['first_usable_transcript_ns']['p95'])} | "
                     f"{fmt(t['final_result_ns']['mean'])} / {fmt(t['final_result_ns']['p95'])} | {fmt(t['finalization_ns']['mean'])} | "
                     f"{g['sampled_peak_rss_bytes'] / 2**30:.2f} GiB | {g['audio_seconds_per_wall_second'] or 0:.3f} |")
    lines += ["", f"Each recording is offered {plan['calls_per_wav']} times per layout. Single-session cases run sequentially; concurrency-two cases overlap calls. Shared models are reused within a WAV suite; native contexts reload per call. First-text timing starts after readiness, so startup and shared context load are reported separately. Four threads are configured per worker; the two-native-worker layout therefore has eight configured compute threads in total.", "",
              "| Layout | EN WER | ID WER | ZH CER | Exact final match to native single |", "|---|---:|---:|---:|---:|"]
    for name, g in groups.items():
        rates = ["unavailable" if g['accuracy'][lang]['rate'] is None else f"{100*g['accuracy'][lang]['rate']:.2f}%" for lang in ("en", "id", "zh")]
        lines.append(f"| {name} | {' | '.join(rates)} | {g['final_matches_native_single']}/{g['completed']} |")
    lines += ["", "Accuracy has one unique utterance per language; repeating it does not increase independent accuracy coverage. WER and CER are never pooled.", "",
              "Full per-call startup, first partial, final, EOF delay, scheduled final lag, queue/submit/publication distributions, CPU, RSS, transcript and accuracy matrices are in `comparison.json` and the original C++ suite artifacts. Shared prefix/EOF queue waits and offline invocation wall time are measured explicitly. Native internal queue/active compute and stable-word timing remain unavailable. Shared model-load timing is one reused-context load, not a load repeated for each call. Native live invocation wall time includes pacing and is not comparable to offline decode wall time.", ""]
    lines += ["| Layout | Configured threads across workers | Sampled mean CPU cores | Sampled peak CPU cores | Peak PSS |",
              "|---|---:|---:|---:|---:|"]
    for name, g in groups.items():
        lines.append(f"| {name} | {g['configured_compute_threads_total']} | {g['sampled_mean_cpu_cores'] or 0:.2f} | {g['peak_sampled_cpu_cores']:.2f} | {g['sampled_peak_pss_bytes']/2**30:.2f} GiB |")
    lines += ["", "CPU means integrate available process-tree samples. Short-lived-process usage and between-sample peaks may be missed; per-call native worker CPU is recorded separately. Sample timing/granularity and controller overhead can put short peaks above the configured model budget.", "",
              "| Timing mean, ms | " + " | ".join(groups) + " |",
              "|---|" + "---:|" * len(groups)]
    for key in TIMINGS:
        if not key.endswith("_ns"):
            continue
        values = ["unavailable" if g['timings'][key]['mean'] is None else f"{g['timings'][key]['mean']:.3f}" for g in groups.values()]
        lines.append(f"| {key} | {' | '.join(values)} |")
    lines += ["", "| Chunk/event delay p95, ms | " + " | ".join(groups) + " |", "|---|" + "---:|" * len(groups)]
    for key in ("send_lag", "controller_queue_wait", "submit_wall", "publication_delay"):
        values = ["unavailable" if g['delay_distributions'][key]['p95'] is None else f"{g['delay_distributions'][key]['p95']:.3f}" for g in groups.values()]
        lines.append(f"| {key} | {' | '.join(values)} |")
    lines += ["", "Chunk/event distributions pool raw durations, never average per-call percentiles. Publication delay uses controller receipt before artifact append and includes IPC for native workers. The native path emits more revisions than the one-preview shared policy, so event counts and live-invocation wall time have different populations/semantics.", ""]
    (output / "comparison.md").write_text("\n".join(lines))
    with (output / "metrics.csv").open("w", newline="") as target:
        writer = csv.writer(target)
        writer.writerow(("layout", "metric", "unit", "count", "mean", "p50", "p95", "p99", "max"))
        for name, group in groups.items():
            for key, dist in {**group["timings"], **group["delay_distributions"]}.items():
                writer.writerow((name, key, dist["unit"], *(dist.get(k) for k in ("count", "mean", "p50", "p95", "p99", "max"))))
    with (output / "calls.csv").open("w", newline="") as target:
        writer = csv.writer(target)
        writer.writerow(("layout", "recording", "language", "run_id", "worker_id", "status", "pre_eof_text",
                         "startup_ms", "first_text_ms", "final_ms", "eof_delay_ms", "effective_rtf", "error_metric", "error_rate", "transcript"))
        for name, group in groups.items():
            for call in group["calls"]:
                m = call["measurements"]
                writer.writerow((name, call["recording_id"], call["language"], call["run_id"], call["worker_id"], call["status"],
                    call["before_eof_text"], *(m.get(k) / 1e6 if m.get(k) is not None else None for k in
                    ("startup_ns", "first_usable_transcript_ns", "final_result_ns", "finalization_ns")),
                    m.get("effective_rtf"), call["accuracy"]["metric"], call["accuracy"]["rate"], call["transcript"]))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--calls-per-wav", type=int, default=2)
    parser.add_argument("--report-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    if args.report_only:
        result = summarize(json.loads((output / "plan.json").read_text()), json.loads((output / "jobs.json").read_text()), output)
        write(output / "status.json", {"status": result["status"]})
        return
    if not 2 <= args.calls_per_wav <= 20:
        parser.error("calls per WAV must be 2..20")
    manifest = ROOT / "datasets/manifests/fleurs_m4_heldout_validation.jsonl"
    index = {row["id"]: row for line in manifest.read_text().splitlines() for row in [json.loads(line)]}
    recordings = [index[key] for key in IDS]
    for row in recordings:
        assert digest(ROOT / row["file"]) == row["sha256"], row["id"]
    output.mkdir(parents=True, exist_ok=False)
    tasks = [(case, row) for row in recordings for case in CASES]
    random.Random(42).shuffle(tasks)
    plan = {"recordings": recordings, "manifest_sha256": digest(manifest), "calls_per_wav": args.calls_per_wav,
            "seed": 42, "execution_order": [[case, row["id"]] for case, row in tasks],
            "cli_sha256": digest(ROOT / "build/release-cpu/asr-cli"),
            "worker_sha256": digest(ROOT / "build/release-cpu/asr-native-worker"),
            "model_acquisition": json.loads((ROOT / "models/qwen3-asr-0.6b/acquisition.json").read_text()),
            "fixed": {"mode": "direct", "chunk_ms": 200, "sample_rate_hz": 16000, "per_worker_threads": 4,
                      "bf16_cache_mb": 0, "realtime_pacing": True, "native_decode_step_ms": 2000,
                      "prefix_preview_ms": 4000, "sampling_interval_ms": 200, "warmups": 0}}
    write(output / "plan.json", plan)
    write(output / "status.json", {"status": "RUNNING"})
    jobs = []
    for number, (case, row) in enumerate(tasks, 1):
        config, workers, sessions, concurrency = CASES[case]
        destination = output / case / row["id"]
        command = [str(ROOT / "build/release-cpu/asr-cli"), "load", "--config", str(ROOT / "configs" / config),
                   "--set", f"workers.processes={workers}", "--set", f"workers.max_sessions_per_process={sessions}",
                   "--set", "audio.chunk_ms=200", "--set", "model.threads=4", "--set", "metrics.sample_interval_ms=200",
                   "--set", f"audio.path={ROOT / row['file']}", "--set", f"output.directory={destination}",
                   "--calls", str(args.calls_per_wav), "--concurrency", str(concurrency),
                   "--languages", row["language"], "--mode", "direct"]
        print(f"{number}/{len(tasks)}: {case} {row['id']}", flush=True)
        started = time.monotonic_ns()
        watchdog_seconds = max(180, math.ceil(args.calls_per_wav / concurrency) * (row["duration_s"] + 60))
        process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=watchdog_seconds)
        try:
            suite = json.loads(process.stdout)
        except json.JSONDecodeError:
            suite = {"status": "PREFLIGHT_REJECTED" if "preflight" in process.stderr else "FAILED"}
        job = {"case": case, "recording_id": row["id"], "command": command,
               "exit_code": process.returncode, "external_wall_ns": time.monotonic_ns() - started,
               "stderr": process.stderr, "suite": suite,
               "calls": score_calls(suite, row) if "phases" in suite else []}
        cpu_seconds = cpu_wall = 0.0
        for phase in suite.get("phases", []):
            samples = [json.loads(line) for line in (Path(suite["directory"]) / phase["resource_samples_path"]).read_text().splitlines()]
            for before, after in zip(samples, samples[1:]):
                interval = (after["timestamp_ns"] - before["timestamp_ns"]) / 1e9
                cpu_wall += interval
                cpu_seconds += interval * sum(p["cpu_core_equivalents"] or 0 for p in after["processes"])
        job["sampled_cpu_core_seconds"] = cpu_seconds
        job["cpu_sampled_wall_seconds"] = cpu_wall
        if len(job["calls"]) >= 2 and all(c["status"] == "COMPLETE" for c in job["calls"]):
            a, b = job["calls"][:2]
            overlap = max(a["measurements"]["stream_start_ns"], b["measurements"]["stream_start_ns"]) < min(
                a["measurements"]["stream_start_ns"] + a["measurements"]["final_result_ns"],
                b["measurements"]["stream_start_ns"] + b["measurements"]["final_result_ns"])
            topology = (not overlap if concurrency == 1 else overlap and
                        ((a["worker_id"] == b["worker_id"]) == case.startswith("shared")))
            for call in job["calls"]:
                call["checks"]["topology"] = topology
        jobs.append(job)
        write(output / "jobs.json", jobs)
        print(f"  {suite['status']}: {len(job['calls'])} recorded calls", flush=True)
    result = summarize(plan, jobs, output)
    valid = all(group["all_checks"] for group in result["groups"].values())
    write(output / "status.json", {"status": "COMPLETE" if valid else "INCOMPLETE"})
    print(output / "comparison.md", flush=True)
    raise SystemExit(0 if valid else 1)


if __name__ == "__main__":
    main()
