#!/usr/bin/env python3
"""Compare matched clean/telephone calls from one M4 measured experiment."""
import argparse
import json
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.datasets.telephone import VERSION


def read_jsonl(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()]


def quantile(values, fraction):
    if not values:
        return None
    values = sorted(values)
    position = (len(values) - 1) * fraction
    lower = math.floor(position)
    return values[lower] + (values[math.ceil(position)] - values[lower]) * (position - lower)


def compare(manifest_rows, results):
    metadata = {row["id"]: row for row in manifest_rows}
    if len(metadata) != len(manifest_rows) or len(results) != len(metadata):
        raise ValueError("manifest/result count or IDs differ")
    by_id = {row["id"]: row for row in results}
    if len(by_id) != len(results) or set(by_id) != set(metadata):
        raise ValueError("manifest/result IDs differ")
    pairs = {}
    for run_id, meta in metadata.items():
        condition = meta["condition"]
        if condition not in ("clean", VERSION):
            raise ValueError(f"unsupported condition: {condition}")
        group = pairs.setdefault(meta["pair_id"], {})
        if condition in group:
            raise ValueError("duplicate condition in pair")
        row = by_id[run_id]
        if row["language"] != meta["language"]:
            raise ValueError("language mismatch")
        group[condition] = row
    if any(set(group) != {"clean", VERSION} for group in pairs.values()):
        raise ValueError("incomplete clean/telephone pair")
    rows = []
    for pair_id, group in pairs.items():
        clean, telephone = group["clean"], group[VERSION]
        expected_metric = "cer" if clean["language"] == "zh" else "wer"
        if clean["accuracy"]["metric"] != expected_metric or telephone["accuracy"]["metric"] != expected_metric:
            raise ValueError("pair metric mismatch")
        if clean["accuracy"]["reference_units"] != telephone["accuracy"]["reference_units"]:
            raise ValueError("pair reference denominators differ")
        if clean["accuracy"].get("reference") != telephone["accuracy"].get("reference"):
            raise ValueError("pair references differ")
        rows.append({"pair_id": pair_id, "language": clean["language"],
                     "metric": clean["accuracy"]["metric"],
                     "reference_units": clean["accuracy"]["reference_units"],
                     "clean_status": clean["status"], "telephone_status": telephone["status"],
                     "clean_edits": clean["accuracy"]["edits"],
                     "telephone_edits": telephone["accuracy"]["edits"],
                     "edit_delta": telephone["accuracy"]["edits"] - clean["accuracy"]["edits"],
                     "clean_first_ms": clean["measurements"].get("first_usable_transcript_ns") / 1e6
                     if clean["status"] == "COMPLETE" and clean["measurements"].get("first_usable_transcript_ns") is not None else None,
                     "telephone_first_ms": telephone["measurements"].get("first_usable_transcript_ns") / 1e6
                     if telephone["status"] == "COMPLETE" and telephone["measurements"].get("first_usable_transcript_ns") is not None else None,
                     "clean_final_ms": clean["measurements"].get("final_result_ns", 0) / 1e6
                     if clean["status"] == "COMPLETE" else None,
                     "telephone_final_ms": telephone["measurements"].get("final_result_ns", 0) / 1e6
                     if telephone["status"] == "COMPLETE" else None})
    summary = {}
    for language in ("en", "id", "zh"):
        selected = [row for row in rows if row["language"] == language]
        if not selected:
            continue
        units = sum(row["reference_units"] for row in selected)
        clean_edits = sum(row["clean_edits"] for row in selected)
        telephone_edits = sum(row["telephone_edits"] for row in selected)
        clean_final = [row["clean_final_ms"] for row in selected if row["clean_final_ms"] is not None]
        tel_final = [row["telephone_final_ms"] for row in selected
                     if row["telephone_final_ms"] is not None]
        clean_first = [row["clean_first_ms"] for row in selected if row["clean_first_ms"] is not None]
        tel_first = [row["telephone_first_ms"] for row in selected
                     if row["telephone_first_ms"] is not None]
        summary[language] = {"metric": "cer" if language == "zh" else "wer",
                             "pairs": len(selected), "reference_units": units,
                             "clean_failure_inclusive_rate": clean_edits / units if units else None,
                             "telephone_failure_inclusive_rate": telephone_edits / units if units else None,
                             "absolute_error_rate_delta": (telephone_edits - clean_edits) / units
                             if units else None,
                             "clean_failures": sum(row["clean_status"] != "COMPLETE" for row in selected),
                             "telephone_failures": sum(row["telephone_status"] != "COMPLETE"
                                                       for row in selected),
                             "clean_p50_final_ms": quantile(clean_final, .5),
                             "telephone_p50_final_ms": quantile(tel_final, .5),
                             "clean_p50_first_ms": quantile(clean_first, .5),
                             "telephone_p50_first_ms": quantile(tel_first, .5)}
    return {"schema_version": 1, "status": "COMPLETE", "pairs": rows, "by_language": summary,
            "scope": "paired tuning-cohort screening; WER and CER are never pooled; "
                     "failure-inclusive error rates use empty hypotheses for failed calls; "
                     "small populations do not establish accuracy/latency SLOs"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--pairs", type=Path, required=True,
                        help="directory from prepare_m9_pairs.py")
    parser.add_argument("--experiment", type=Path, required=True,
                        help="directory from run_measured.py on combined.jsonl")
    args = parser.parse_args()
    state = json.loads((args.experiment / "status.json").read_text())
    if state.get("status") != "COMPLETE":
        raise ValueError("measured experiment is not complete")
    report = compare(read_jsonl(args.pairs / "combined.jsonl"),
                     read_jsonl(args.experiment / "accuracy.jsonl"))
    report["experiment"] = str(args.experiment.resolve())
    report["pairing"] = str((args.pairs / "pairing.json").resolve())
    output = args.pairs / "comparison.json"
    with output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps(report["by_language"], ensure_ascii=False, indent=2))
