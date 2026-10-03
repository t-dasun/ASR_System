#!/usr/bin/env python3
"""Validate the C++ M3 adapter against the pinned M0 acceptance cohort."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from scoring import score

MANIFEST_SHA256 = "06e3e91da21530ec5e0e9e6a4dd212b846e5a35cdf61265614cefdcf38432376"
PAIRED_SHA256 = "b3b3063f2f3970927ba32585b13668854caf0fc3467c8f85ea54a1a61e942ac9"

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def aggregate(rows, language):
    values = [row["final_vs_offline"] for row in rows if row["language"] == language]
    edits = sum(item[key] for item in values for key in ("substitutions", "insertions", "deletions"))
    units = sum(item["reference_units"] for item in values)
    return {"count": len(values), "edits": edits, "reference_units": units,
            "rate": edits / units if units else None,
            "metric": "cer" if language == "zh" else "wer"}


def main(args):
    if digest(args.manifest) != MANIFEST_SHA256 or digest(args.paired) != PAIRED_SHA256:
        raise ValueError("M3 acceptance manifest or paired reference differs from the locked M0 cohort")
    manifest = [json.loads(line) for line in args.manifest.read_text().splitlines()]
    paired = [json.loads(line) for line in args.paired.read_text().splitlines()]
    index = {item["id"]: item for item in manifest}
    if len(index) != len(manifest) or len(paired) != 15 or len({item["id"] for item in paired}) != 15:
        raise ValueError("expected unique manifest IDs and 15 held-out paired rows")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    with args.output.open("x") as target:
        for reference in paired:
            source = index[reference["id"]]
            if source["cohort"] != "acceptance" or source["language"] != reference["language"]:
                raise ValueError(f"cohort mismatch: {reference['id']}")
            wav = Path(source["file"])
            if digest(wav) != source["sha256"]:
                raise ValueError(f"prepared audio hash mismatch: {wav}")
            command = [str(args.cli), "run", "--config", str(args.config),
                       "--set", "model.runtime=qwen_native",
                       "--set", f"model.path={args.model.resolve()}",
                       "--set", "model.threads=4",
                       "--set", "model.decode_step_ms=2000",
                       "--set", "model.max_new_tokens=32",
                       "--set", "model.timeout_ms=45000",
                       "--set", "model.refine_final=true",
                       "--set", "audio.source=wav",
                       "--set", f"audio.path={wav.resolve()}",
                       "--set", "audio.realtime_pacing=true",
                       "--set", f"dataset.language={source['language']}"]
            try:
                process = subprocess.run(command, capture_output=True, text=True, timeout=60, check=False)
                output = json.loads(process.stdout) if process.stdout.strip() else {}
                exit_code = process.returncode
                error = process.stderr[-2000:]
            except subprocess.TimeoutExpired as timeout:
                output = {}
                exit_code = None
                error = f"external timeout after {timeout.timeout}s"
            directory = Path(output["run_directory"]) if "run_directory" in output else None
            status = json.loads((directory / "status.json").read_text()) if directory else {}
            summary = json.loads((directory / "summary.json").read_text()) if directory else {}
            events = [json.loads(line) for line in (directory / "events.jsonl").read_text().splitlines()] if directory else []
            final = summary.get("transcript", "")
            row = {"id": source["id"], "language": source["language"],
                   "wav_sha256": source["sha256"], "run_directory": str(directory) if directory else None,
                   "exit_code": exit_code, "stderr": error, "status": status.get("status"),
                   "is_mock": status.get("is_mock"), "audio_samples": summary.get("audio_samples"),
                   "expected_samples": source["num_samples"], "max_send_lag_ns": summary.get("max_send_lag_ns"),
                   "audio_overflows": summary.get("audio_overflows"),
                   "final_count": sum(event["kind"] == "final" for event in events),
                   "pre_eof_text": any(event["kind"] == "partial" and event.get("before_eof") and event["text"].strip()
                                       for event in events),
                   "final_text": final, "native_offline_text": reference["native_offline_text"],
                   "final_vs_offline": score(reference["native_offline_text"], final, source["language"])}
            rows.append(row)
            target.write(json.dumps(row, ensure_ascii=False) + "\n")
            target.flush()
            print(f"{source['id']}: {row['status']} final={row['final_vs_offline']['rate']:.3f} "
                  f"preEOF={row['pre_eof_text']}", flush=True)
    languages = {lang: aggregate(rows, lang) for lang in ("en", "id", "zh")}
    checks = {
        "cohort": len(rows) == 15 and all(item["count"] == 5 for item in languages.values()),
        "completed": all(row["exit_code"] == 0 and row["status"] == "COMPLETE" and
                         row["is_mock"] is False and row["final_count"] == 1 and row["final_text"]
                         for row in rows),
        "audio": all(row["audio_samples"] == row["expected_samples"] and row["audio_overflows"] == 0 and
                     row["max_send_lag_ns"] is not None and row["max_send_lag_ns"] <= 100_000_000
                     for row in rows),
        "parity": all(item["rate"] is not None and item["rate"] <= 0.05 for item in languages.values()),
        "pre_eof": any(row["pre_eof_text"] for row in rows),
    }
    summary = {"schema_version": 1, "gate": "docs/decisions/0004-m3-native-adapter-gate.md",
               "manifest_sha256": digest(args.manifest), "paired_sha256": digest(args.paired),
               "rows_sha256": digest(args.output), "languages": languages, "checks": checks,
               "passed": all(checks.values())}
    args.output.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=Path("datasets/manifests/fleurs_m0.jsonl"))
    parser.add_argument("--paired", type=Path, default=Path("results-reference/fleurs_acceptance_compare.jsonl"))
    parser.add_argument("--cli", type=Path, default=Path("build/release-cpu/asr-cli"))
    parser.add_argument("--config", type=Path, default=Path("configs/mock_baseline.yaml"))
    parser.add_argument("--model", type=Path, default=Path("models/qwen3-asr-0.6b"))
    parser.add_argument("--output", type=Path, required=True)
    raise SystemExit(main(parser.parse_args()))
