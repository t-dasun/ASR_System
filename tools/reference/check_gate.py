#!/usr/bin/env python3
"""Evaluate the predeclared small-sample M0 parity gate on paired JSONL."""
import argparse
import hashlib
import json
from pathlib import Path

LANGUAGES = ("en", "id", "zh")


def evaluate(path):
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    checks = {}
    for language in LANGUAGES:
        group = [row for row in rows if row["language"] == language]
        counts_ok = len(group) == 5 and len({row["id"] for row in group}) == 5
        cohort_ok = all(row["cohort"] == "acceptance" for row in group)
        completion_ok = all(row["native_status"] == "COMPLETE" and
                            row["native_offline_text"].strip() and
                            row["official_offline_text"].strip() for row in group)
        def aggregate(key):
            edits = sum(row[key][part] for row in group for part in
                        ("substitutions", "insertions", "deletions"))
            units = sum(row[key]["reference_units"] for row in group)
            return edits / units if units else None
        native_human = aggregate("native_vs_human")
        official_human = aggregate("official_vs_human")
        native_official = aggregate("native_vs_official")
        parity_ok = native_official is not None and native_official <= 0.05
        human_gap_ok = (native_human is not None and official_human is not None and
                        native_human - official_human <= 0.05)
        checks[language] = {
            "count": len(group), "counts_ok": counts_ok, "cohort_ok": cohort_ok,
            "completion_and_nonempty_ok": bool(completion_ok),
            "native_vs_human_rate": native_human, "official_vs_human_rate": official_human,
            "native_vs_official_rate": native_official,
            "parity_ok": parity_ok, "human_gap_ok": human_gap_ok,
            "passed": all((counts_ok, cohort_ok, completion_ok, parity_ok, human_gap_ok)),
        }
    return {
        "schema_version": 1, "gate": "docs/decisions/0002-m0-parity-gate.md",
        "input_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "languages": checks, "passed": len(rows) == 15 and all(
            entry["passed"] for entry in checks.values()),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("paired", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = evaluate(args.paired)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x") as target:
            target.write(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
