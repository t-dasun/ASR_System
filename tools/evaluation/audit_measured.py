#!/usr/bin/env python3
"""Read-only checksum, alignment, and population audit of a sealed M4 experiment."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from evaluation.scoring import aggregate


def audit(directory):
    directory = directory.resolve()
    checksums = json.loads((directory / "checksums.json").read_text())
    for name, expected in checksums.items():
        path = (directory / name).resolve()
        if not path.is_relative_to(directory):
            raise ValueError("checksum path escaped experiment")
        with path.open("rb") as source:
            actual = hashlib.file_digest(source, "sha256").hexdigest()
        if actual != expected:
            raise ValueError(f"artifact hash mismatch: {name}")
    rows = [json.loads(line) for line in (directory / "accuracy.jsonl").read_text().splitlines()]
    snapshot = [json.loads(line) for line in (directory / "dataset_snapshot.jsonl").read_text().splitlines()]
    summary = json.loads((directory / "summary.json").read_text())
    if [row["id"] for row in rows] != [row["id"] for row in snapshot]:
        raise ValueError("missing/reordered offered calls")
    if summary["accuracy"] != aggregate(rows):
        raise ValueError("accuracy populations disagree with raw rows")
    for row, source in zip(rows, snapshot):
        score = row["accuracy"]
        if score["reference"] != source["reference"]:
            raise ValueError("reference changed")
        if row["status"] != "COMPLETE" and score["hypothesis"] != "":
            raise ValueError("failed workload call did not count full deletions")
        edits = sum(op["op"] != "equal" for op in score["alignment"])
        if edits != score["edits"] or edits != sum(score[key] for key in ("substitutions", "deletions", "insertions")):
            raise ValueError("alignment/edit disagreement")
    return {"artifact_hashes_verified": len(checksums), "offered_calls_verified": len(rows),
            "status": summary["status"], "accuracy_populations_verified": True}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.directory), indent=2))
