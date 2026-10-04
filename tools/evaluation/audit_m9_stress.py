#!/usr/bin/env python3
"""Audit a native long-form stress call without inventing accuracy labels."""
import argparse
import hashlib
import json
from pathlib import Path


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def audit(schedule, run):
    plan = read_json(schedule)
    wav = Path(plan["output"]).resolve()
    summary, state, config = (read_json(run / name) for name in
                              ("summary.json", "status.json", "config.json"))
    events = [json.loads(line) for line in (run / "events.jsonl").read_text(encoding="utf-8").splitlines()
              if line.strip()]
    timeout_ms = config["model"]["timeout_ms"]
    expected_chunks = (plan["samples"] + config["audio"]["chunk_ms"] * 16 - 1) // (
        config["audio"]["chunk_ms"] * 16)
    checks = {"source_hash": digest(Path(plan["source"])) == plan["source_sha256"],
              "stress_hash": digest(wav) == plan["output_sha256"],
              "native": state.get("is_mock") is False and summary.get("is_mock") is False,
              "resolved_wav": Path(summary.get("audio", {}).get("path", "")).resolve() == wav,
              "sample_count": summary.get("audio_samples") == plan["samples"],
              "all_chunks": summary.get("chunks") == expected_chunks,
              "no_overflow": summary.get("audio_overflows") == 0,
              "one_final": sum(event.get("kind") == "final" for event in events) == 1,
              "resources_valid": summary.get("resources", {}).get("measurement_valid") is True}
    return {"schema_version": 1, "status": state["status"], "run": str(run.resolve()),
            "schedule": str(schedule.resolve()), "duration_seconds": plan["duration_seconds"],
            "model_timeout_ms": timeout_ms,
            "timeout_shorter_than_audio": timeout_ms < plan["duration_seconds"] * 1000,
            "checks": checks, "checks_passed": state["status"] == "COMPLETE" and all(checks.values()),
            "first_usable_ms": (summary.get("measurements", {}).get("first_usable_transcript_ns") or 0) / 1e6
            if summary.get("measurements", {}).get("first_usable_transcript_ns") is not None else None,
            "final_result_ms": (summary.get("measurements", {}).get("final_result_ns") or 0) / 1e6
            if summary.get("measurements", {}).get("final_result_ns") is not None else None,
            "partial_events": sum(event.get("kind") == "partial" for event in events),
            "pre_eof_partial_events": sum(event.get("kind") == "partial" and event.get("before_eof") is True
                                          for event in events),
            "transcript_characters": len(summary.get("transcript", "")),
            "error": summary.get("error"),
            "scope": "synthetic replay/interrupt/silence schedule; no human reference or WER/CER"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--schedule", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.schedule, args.run)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps(result, indent=2))
