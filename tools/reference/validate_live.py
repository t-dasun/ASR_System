#!/usr/bin/env python3
"""Run the predeclared M0 paced-live gate on held-out FLEURS WAV files."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from scoring import score

LANGUAGES = {"en": "English", "id": "Indonesian", "zh": "Chinese"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def aggregate(rows, language, key):
    group = [row[key] for row in rows if row["language"] == language]
    edits = sum(value[part] for value in group for part in
                ("substitutions", "insertions", "deletions"))
    units = sum(value["reference_units"] for value in group)
    return {"metric": "cer" if language == "zh" else "wer", "edits": edits,
            "reference_units": units, "rate": edits / units if units else None}


def run(args):
    manifest = [json.loads(line) for line in args.manifest.read_text().splitlines()]
    paired = [json.loads(line) for line in args.paired.read_text().splitlines()]
    index = {row["id"]: row for row in manifest}
    if len(index) != len(manifest) or len(paired) != 15 or len({row["id"] for row in paired}) != 15:
        raise ValueError("Expected unique manifest IDs and 15 paired acceptance rows")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    results = []
    with args.output.open("x") as output:
        for reference in paired:
            source = index[reference["id"]]
            if source["cohort"] != "acceptance" or source["language"] != reference["language"]:
                raise ValueError(f"Mismatched acceptance sample: {reference['id']}")
            wav = Path(source["file"])
            if digest(wav) != source["sha256"]:
                raise ValueError(f"Prepared audio hash mismatch: {wav}")
            command = [sys.executable, "research/native_qwen/run_probe.py", "--wav", str(wav),
                       "--language", LANGUAGES[source["language"]], "--threads", str(args.threads),
                       "--chunk-ms", "200", "--refine-final", "--timeout", str(args.timeout)]
            process = subprocess.run(command, capture_output=True, text=True, check=False)
            first_line = process.stdout.splitlines()[0] if process.stdout else ""
            directory = Path(first_line.removeprefix("Run directory: ")) if first_line.startswith("Run directory: ") else None
            status_path = directory / "status.json" if directory else None
            status = json.loads(status_path.read_text()) if status_path and status_path.exists() else {"status": "FAILED"}
            summary_path = directory / "summary.json" if directory else None
            summary = json.loads(summary_path.read_text()) if summary_path and summary_path.exists() else {}
            final = summary.get("transcript", "")
            stream = summary.get("stream_transcript", "")
            row = {"id": source["id"], "language": source["language"], "cohort": source["cohort"],
                   "run_directory": str(directory) if directory else None,
                   "status": status, "probe_exit_code": process.returncode,
                   "probe_stderr": process.stderr[-1000:] if process.stderr else None,
                   "audio_seconds": source["duration_s"],
                   "stream_text": stream, "final_text": final,
                   "offline_native_text": reference["native_offline_text"],
                   "official_offline_text": reference["official_offline_text"],
                   "text_before_eof": summary.get("text_before_eof", False),
                   "overflow": summary.get("overflow"),
                   "max_send_lag_ms": summary.get("max_send_lag_ms"),
                   "stream_completion_ms": summary.get("completion_ms"),
                   "final_refine_ms": summary.get("final_refine_ms"),
                   "final_completion_ms": summary.get("final_completion_ms"),
                   "stream_vs_offline": score(reference["native_offline_text"], stream, source["language"]),
                   "final_vs_offline": score(reference["native_offline_text"], final, source["language"]),
                   "final_vs_human": score(source["reference"], final, source["language"])}
            results.append(row)
            output.write(json.dumps(row, ensure_ascii=False) + "\n")
            output.flush()
            print(f"{row['id']}: {status['status']} stream={row['stream_vs_offline']['rate']:.3f} "
                  f"final={row['final_vs_offline']['rate']:.3f}", flush=True)
    languages = {}
    for language in LANGUAGES:
        group = [row for row in results if row["language"] == language]
        languages[language] = {"count": len(group),
                               "stream_vs_offline": aggregate(results, language, "stream_vs_offline"),
                               "final_vs_offline": aggregate(results, language, "final_vs_offline"),
                               "final_vs_human": aggregate(results, language, "final_vs_human")}
    gate = {"count_ok": len(results) == 15 and all(item["count"] == 5 for item in languages.values()),
            "all_completed": all(row["status"]["status"] == "COMPLETE" and row["probe_exit_code"] == 0
                                 and row["final_text"] for row in results),
            "no_overflow": all(row["overflow"] is False for row in results),
            "pacing_ok": all(row["max_send_lag_ms"] is not None and
                             row["max_send_lag_ms"] <= 100 for row in results),
            "final_parity_ok": all(item["final_vs_offline"]["rate"] is not None and
                                   item["final_vs_offline"]["rate"] <= 0.05 for item in languages.values()),
            "any_pre_eof_text": any(row["text_before_eof"] for row in results),
            "memory_ok": all(max(row["status"].get("child_peak_rss_kb") or 0,
                                 row["status"].get("sampled_peak_rss_kb") or 0) <= 4 * 1024 * 1024
                             and max(row["status"].get("child_peak_rss_kb") or 0,
                                     row["status"].get("sampled_peak_rss_kb") or 0) > 0 for row in results),
            "threads_ok": all(0 < row["status"].get("sampled_peak_threads", 0) <= 16
                              for row in results)}
    result = {"schema_version": 1, "gate": "docs/decisions/0003-m0-live-gate.md",
              "paired_sha256": digest(args.paired), "manifest_sha256": digest(args.manifest),
              "rows_sha256": digest(args.output), "threads": args.threads,
              "transport_chunk_ms": 200, "decode_step_ms": 2000,
              "watchdog_timeout_s": args.timeout, "languages": languages,
              "max_observed_peak_rss_kb": max(max(row["status"].get("child_peak_rss_kb") or 0,
                                                    row["status"].get("sampled_peak_rss_kb") or 0)
                                                for row in results),
              "max_sampled_peak_threads": max(row["status"].get("sampled_peak_threads", 0)
                                              for row in results),
              "gate_checks": gate, "passed": all(gate.values())}
    summary_path = args.output.with_suffix(".summary.json")
    with summary_path.open("x") as target:
        target.write(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=Path("datasets/manifests/fleurs_m0.jsonl"))
    parser.add_argument("--paired", type=Path, default=Path("results-reference/fleurs_acceptance_compare.jsonl"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=45)
    raise SystemExit(run(parser.parse_args()))
