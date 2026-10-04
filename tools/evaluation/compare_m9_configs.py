#!/usr/bin/env python3
"""Compare two pinned Qwen runs on the same clips with threads as sole YAML factor."""
import argparse
import json
import math
from pathlib import Path

import yaml


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def load_jsonl(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def quantile(values, p):
    if not values:
        return None
    values = sorted(values)
    position = (len(values) - 1) * p
    lower = math.floor(position)
    return values[lower] + (values[math.ceil(position)] - values[lower]) * (position - lower)


def one_factor(left_config, right_config):
    left = yaml.safe_load(left_config.read_text(encoding="utf-8"))
    right = yaml.safe_load(right_config.read_text(encoding="utf-8"))
    left_threads = left["model"].pop("threads")
    right_threads = right["model"].pop("threads")
    if left != right or left_threads == right_threads:
        raise ValueError("configs must differ only in model.threads")
    return left_threads, right_threads


def compare(manifest, left_rows, right_rows, left_threads, right_threads):
    expected = {row["id"]: row for row in manifest}
    if len(expected) != len(manifest) or len(expected) == 0:
        raise ValueError("manifest must contain unique nonempty IDs")
    maps = []
    for rows in (left_rows, right_rows):
        mapping = {row["id"]: row for row in rows}
        if len(mapping) != len(rows) or not set(expected).issubset(mapping):
            raise ValueError("missing or duplicate measured clip")
        maps.append(mapping)
    paired = []
    for clip_id, source in expected.items():
        a, b = (mapping[clip_id] for mapping in maps)
        language = source["language"]
        metric = "cer" if language == "zh" else "wer"
        if (a["language"] != language or b["language"] != language or
                a["accuracy"]["metric"] != metric or b["accuracy"]["metric"] != metric or
                a["accuracy"]["reference_units"] != b["accuracy"]["reference_units"] or
                a["accuracy"].get("reference") != b["accuracy"].get("reference")):
            raise ValueError("language, metric, or reference mismatch")
        def latency(row, key):
            value = row["measurements"].get(key) if row["status"] == "COMPLETE" else None
            return value / 1e6 if value is not None else None
        paired.append({"id": clip_id, "language": language, "metric": metric,
                       "reference_units": a["accuracy"]["reference_units"],
                       "left_status": a["status"], "right_status": b["status"],
                       "left_edits": a["accuracy"]["edits"],
                       "right_edits": b["accuracy"]["edits"],
                       "left_first_ms": latency(a, "first_usable_transcript_ns"),
                       "right_first_ms": latency(b, "first_usable_transcript_ns"),
                       "left_final_ms": latency(a, "final_result_ns"),
                       "right_final_ms": latency(b, "final_result_ns")})
    by_language = {}
    for language in ("en", "id", "zh"):
        rows = [row for row in paired if row["language"] == language]
        if not rows:
            continue
        units = sum(row["reference_units"] for row in rows)
        by_language[language] = {"metric": rows[0]["metric"], "clips": len(rows),
                                 "reference_units": units,
                                 "left_edits": sum(row["left_edits"] for row in rows),
                                 "right_edits": sum(row["right_edits"] for row in rows),
                                 "left_failures": sum(row["left_status"] != "COMPLETE" for row in rows),
                                 "right_failures": sum(row["right_status"] != "COMPLETE" for row in rows)}
        for side in ("left", "right"):
            by_language[language][f"{side}_error_rate"] = (by_language[language][f"{side}_edits"] / units
                                                            if units else None)
            for metric in ("first", "final"):
                values = [row[f"{side}_{metric}_ms"] for row in rows
                          if row[f"{side}_{metric}_ms"] is not None]
                by_language[language][f"{side}_p50_{metric}_ms"] = quantile(values, .5)
    return {"schema_version": 1, "factor": "model.threads", "left_threads": left_threads,
            "right_threads": right_threads, "pairs": paired, "by_language": by_language,
            "scope": "sequential tuning-cohort screen; independent new model process per call; "
                     "OS cache/thermal order uncontrolled; no capacity or tail-latency claim"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--left", type=Path, required=True)
    parser.add_argument("--right", type=Path, required=True)
    parser.add_argument("--left-config", type=Path, required=True)
    parser.add_argument("--right-config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    threads = one_factor(args.left_config, args.right_config)
    for folder in (args.left, args.right):
        if load_json(folder / "status.json")["status"] != "COMPLETE":
            raise ValueError(f"experiment is not COMPLETE: {folder}")
    left_provenance, right_provenance = (load_json(folder / "experiment.json") for folder in
                                         (args.left, args.right))
    for key in ("cli_sha256", "worker_sha256", "model_acquisition"):
        if left_provenance[key] != right_provenance[key]:
            raise ValueError(f"runtime/model provenance differs: {key}")
    result = compare(load_jsonl(args.manifest), load_jsonl(args.left / "accuracy.jsonl"),
                     load_jsonl(args.right / "accuracy.jsonl"), *threads)
    result.update({"left": str(args.left.resolve()), "right": str(args.right.resolve()),
                   "manifest": str(args.manifest.resolve())})
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps(result["by_language"], ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
