#!/usr/bin/env python3
"""Sequential offline experiment orchestration over the measured C++ CLI.

No Python inference or audio pacing. Unique output directories retain every failure.
"""
import argparse
import fcntl
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from evaluation.scoring import AccuracyEvaluator, IdentityNormalizer, aggregate


def digest(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def write_json(path, value):
    with path.open("x", encoding="utf-8") as target:
        json.dump(value, target, ensure_ascii=False, indent=2, allow_nan=False)
        target.write("\n")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def optional_text(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def quantiles(values, unit="ns"):
    values = sorted(values)
    result = {"count": len(values), "population": "completed calls", "unit": unit,
              "estimator": "linear_(n-1)*p_type7"}
    for name, p in (("p50", .5), ("p90", .9), ("p95", .95), ("p99", .99)):
        pos = (len(values) - 1) * p
        result[name] = (values[math.floor(pos)] + (values[math.ceil(pos)] - values[math.floor(pos)]) *
                        (pos - math.floor(pos))) if values else None
    result["max"] = max(values) if values else None
    return result


def select_rows(manifest, per_language):
    rows = [json.loads(line) for line in manifest.read_text(encoding="utf-8").splitlines() if line.strip()]
    if len({row["id"] for row in rows}) != len(rows):
        raise ValueError("duplicate manifest IDs")
    if any(row["language"] not in ("en", "id", "zh") for row in rows):
        raise ValueError("unsupported manifest language")
    groups = {lang: sorted((row for row in rows if row["language"] == lang),
                            key=lambda row: (row.get("selection_rank", 0), row["id"]))
              for lang in ("en", "id", "zh")}
    count = per_language or min(map(len, groups.values()))
    if count < 1 or any(len(group) < count for group in groups.values()):
        raise ValueError("manifest lacks requested balanced cohort")
    if not per_language and len({len(group) for group in groups.values()}) != 1:
        raise ValueError("full experiment requires balanced language counts")
    # Round-robin languages, deterministic within-language manifest rank.
    selected = [groups[lang][i] for i in range(count) for lang in ("en", "id", "zh")]
    for row in selected:
        if not row["id"] or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in row["id"]):
            raise ValueError("unsafe recording ID")
        wav = ROOT / row["file"]
        if digest(wav) != row["sha256"]:
            raise ValueError(f"audio hash mismatch: {row['id']}")
        if row.get("reference_field") != "raw_transcription" or not isinstance(row["reference"], str):
            raise ValueError("human raw_transcription required")
    return selected


def parity_check(accuracy):
    import jiwer
    r, h = accuracy["normalized_reference"], accuracy["normalized_hypothesis"]
    expected = jiwer.process_characters(r, h) if accuracy["metric"] == "cer" else jiwer.process_words(r, h)
    return accuracy["edits"] == expected.substitutions + expected.deletions + expected.insertions


def run(args):
    rows = select_rows(args.manifest, args.per_language)
    args.output.mkdir(parents=True, exist_ok=False)
    args.output_owned = True
    write_json(args.output / "status.json", {"status": "RUNNING"})
    with (args.output / "dataset_snapshot.jsonl").open("x", encoding="utf-8") as target:
        for row in rows:
            target.write(json.dumps(row, ensure_ascii=False) + "\n")
    revision = read_json(args.model / "acquisition.json")
    sources = [path for folder in ("apps", "audio", "benchmark", "configs", "core", "engines", "observability", "evaluation", "storage")
               for path in (ROOT / folder).rglob("*") if path.suffix in (".cpp", ".hpp", ".py", ".yaml")]
    sources += [Path(__file__), ROOT / "CMakeLists.txt", ROOT / "cmake/NativeQwen.cmake"]
    write_json(args.output / "experiment.json", {
        "schema_version": 1, "manifest": str(args.manifest), "manifest_sha256": digest(args.manifest),
        "cli": str(args.cli), "cli_sha256": digest(args.cli),
        "worker_sha256": digest(args.cli.parent / "asr-native-worker"),
        "config_sha256": digest(args.config), "model_acquisition": revision,
        "python": platform.python_version(), "jiwer": importlib.metadata.version("jiwer"),
        "host": {"platform": platform.platform(), "affinity": sorted(os.sched_getaffinity(0)),
                 "cpu_info": optional_text("/proc/cpuinfo"), "memory": optional_text("/proc/meminfo"),
                 "process_limits": optional_text("/proc/self/limits"),
                 "governor_cpu0": optional_text("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")},
        "compile_commands_sha256": digest(args.cli.parent / "compile_commands.json"),
        "scope": "sequential single-call measurement screening; new worker/model load per call; no warmup; OS page cache uncontrolled",
        "ordered_ids": [row["id"] for row in rows], "per_language": len(rows) // 3,
        "resource_sampling": args.resource_sampling, "sampling_interval_ms": args.sample_interval_ms,
        "source_sha256": {str(p.relative_to(ROOT)): digest(p) for p in sorted(sources)}})
    scorer = AccuracyEvaluator()
    literal_scorer = AccuracyEvaluator(IdentityNormalizer())
    results = []
    with (args.output / "accuracy.jsonl").open("x", encoding="utf-8") as target:
        for row in rows:
            call_root = args.output / "calls" / row["id"]
            command = [str(args.cli), "run", "--config", str(args.config),
                       "--set", f"model.path={args.model}", "--set", "audio.source=wav",
                       "--set", f"audio.path={(ROOT / row['file']).resolve()}",
                       "--set", f"dataset.language={row['language']}",
                       "--set", f"output.directory={call_root}",
                       "--set", f"metrics.resource_sampling={'true' if args.resource_sampling else 'false'}",
                       "--set", f"metrics.sample_interval_ms={args.sample_interval_ms}"]
            started = time.monotonic_ns()
            error, exit_code = "", None
            try:
                process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=90, check=False)
                error, exit_code = process.stderr, process.returncode
            except subprocess.TimeoutExpired:
                error = "external 90-second watchdog expired"
            directories = sorted(call_root.glob("native_*"))
            directory = directories[0] if len(directories) == 1 else None
            summary = read_json(directory / "summary.json") if directory else {}
            resolved = read_json(directory / "config.json") if directory else {}
            state = "COMPLETE" if exit_code == 0 and summary.get("status") == "COMPLETE" else "FAILED"
            final = summary.get("transcript", "") if state == "COMPLETE" else ""
            accuracy = scorer.score(row["reference"], final, row["language"])
            events = [json.loads(line) for line in (directory / "events.jsonl").read_text().splitlines()] if directory and (directory / "events.jsonl").exists() else []
            runtime = [json.loads(line) for line in (directory / "runtime_timing.jsonl").read_text().splitlines()] if directory and (directory / "runtime_timing.jsonl").exists() else []
            checks = {
                "completed": state == "COMPLETE", "native": summary.get("is_mock") is False,
                "samples": summary.get("audio_samples") == row["num_samples"],
                "one_final": sum(event["kind"] == "final" for event in events) == 1,
                "no_overflow": summary.get("audio_overflows") == 0,
                "scorer_parity": parity_check(accuracy),
                "resources": not args.resource_sampling or summary.get("resources", {}).get("measurement_valid") is True,
                "thread_budget": not args.resource_sampling or 0 < summary.get("resources", {}).get("sampled_peak_worker_threads", 0) <= 2 * resolved.get("model", {}).get("threads", 0),
                "worker_samples": not args.resource_sampling or any(
                    len(json.loads(line).get("processes", [])) > 1 for line in
                    (directory / "system_metrics.jsonl").read_text().splitlines()) if directory else False,
                "receipt_count": sum(item["stage"] == "worker_received" for item in runtime) == summary.get("chunks"),
                "load_timing": summary.get("measurements", {}).get("model_load_ns") is not None,
                "compute_unavailable": summary.get("measurements", {}).get("inference_compute_rtf", "missing") is None,
                "publication_order": all(event.get("published_ns", -1) >= event["produced_ns"] for event in events),
            }
            result = {"id": row["id"], "language": row["language"], "status": state,
                      "run_directory": str(directory.relative_to(args.output)) if directory else None,
                      "exit_code": exit_code, "stderr": error[-4000:], "external_wall_ns": time.monotonic_ns() - started,
                      "partial_text_on_failure": summary.get("transcript", "") if state != "COMPLETE" else None,
                      "accuracy": accuracy, "literal_accuracy": literal_scorer.score(row["reference"], final, row["language"]),
                      "measurements": summary.get("measurements", {}), "resources": summary.get("resources", {}),
                      "checks": checks}
            results.append(result)
            target.write(json.dumps(result, ensure_ascii=False, allow_nan=False) + "\n")
            target.flush()
            print(f"{row['id']}: {state}; {accuracy['metric']}={accuracy['rate']}; checks={all(checks.values())}", flush=True)
    distributions = {}
    for key in ("first_usable_transcript_ns", "final_result_ns", "finalization_ns", "model_load_ns", "effective_rtf"):
        distributions[key] = quantiles([row["measurements"][key] for row in results
                                       if row["status"] == "COMPLETE" and row["measurements"].get(key) is not None],
                                      "ratio" if key == "effective_rtf" else "ns")
    valid = all(all(row["checks"].values()) for row in results)
    summary = {"schema_version": 1, "status": "COMPLETE" if valid else "FAILED", "calls": len(results),
               "completed_calls": sum(row["status"] == "COMPLETE" for row in results),
               "accuracy": aggregate(results), "latency": distributions, "checks_passed": valid,
               "scope": "measurement/scoring gate, no accuracy threshold or capacity/SLO claim; failure-inclusive accuracy uses empty hypothesis for failed calls"}
    write_json(args.output / "summary.json", summary)
    # Status is the sole mutable checkpoint; raw evidence is never overwritten.
    temporary = args.output / "status.next.json"
    write_json(temporary, {"status": summary["status"]})
    temporary.replace(args.output / "status.json")
    checksums = {str(path.relative_to(args.output)): digest(path)
                 for path in sorted(args.output.rglob("*")) if path.is_file()}
    write_json(args.output / "checksums.json", checksums)
    print(json.dumps(summary, indent=2))
    return 0 if valid else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=ROOT / "datasets/manifests/fleurs_m4_tuning.jsonl")
    parser.add_argument("--cli", type=Path, default=ROOT / "build/release-cpu/asr-cli")
    parser.add_argument("--config", type=Path, default=ROOT / "configs/qwen_native_single.yaml")
    parser.add_argument("--model", type=Path, default=ROOT / "models/qwen3-asr-0.6b")
    parser.add_argument("--per-language", type=int, default=0, help="0 = all balanced manifest rows")
    parser.add_argument("--resource-sampling", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--sample-interval-ms", type=int, default=200)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.per_language < 0 or not 50 <= args.sample_interval_ms <= 5000:
        parser.error("invalid count or sampling interval")
    for name in ("manifest", "cli", "config", "model", "output"):
        setattr(args, name, getattr(args, name).resolve())
    (ROOT / ".cache").mkdir(exist_ok=True)
    with (ROOT / ".cache/m4-experiment.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            outcome = run(args)
        except BaseException as error:
            # Seal a failed orchestration attempt without touching existing outputs.
            if getattr(args, "output_owned", False) and (args.output / "status.json").exists() and read_json(args.output / "status.json").get("status") == "RUNNING":
                write_json(args.output / "orchestration_error.json", {"error": str(error), "type": type(error).__name__})
                checkpoint = args.output / "status.failed.json"
                write_json(checkpoint, {"status": "FAILED", "reason": "orchestration exception; retain partial evidence"})
                checkpoint.replace(args.output / "status.json")
            raise
        raise SystemExit(outcome)
