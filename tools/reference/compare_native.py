#!/usr/bin/env python3
"""Run native offline inference on the same inputs as the official CPU reference."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

from scoring import score

LANGUAGES = {"en": "English", "zh": "Chinese", "id": "Indonesian"}


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def compare(args):
    manifest = [json.loads(line) for line in args.manifest.read_text().splitlines()]
    references = [json.loads(line) for line in args.reference.read_text().splitlines()]
    index = {row["id"]: row for row in manifest}
    if len(index) != len(manifest) or len({row["id"] for row in references}) != len(references):
        raise ValueError("Duplicate IDs in manifest or reference output")
    if not references:
        raise ValueError("No reference results")
    binary, model = args.binary.resolve(strict=True), args.model.resolve(strict=True)
    acquisition = json.loads((model / "acquisition.json").read_text())
    if acquisition["status"] != "verified":
        raise ValueError("Unverified model acquisition")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    env = {**os.environ, "OMP_NUM_THREADS": str(args.threads),
           "OPENBLAS_NUM_THREADS": str(args.threads), "OMP_DYNAMIC": "FALSE",
           "QWEN_BF16_CACHE_MB": "0"}
    with args.output.open("x") as output:
        for reference in references:
            source = index.get(reference["id"])
            if source is None or source["language"] != reference["language"]:
                raise ValueError(f"Reference not in matching manifest: {reference['id']}")
            wav = Path(source["file"])
            if digest(wav) != source["sha256"]:
                raise ValueError(f"Prepared audio mismatch: {wav}")
            command = [str(binary), "-d", str(model), "-i", str(wav), "-t", str(args.threads),
                       "--language", LANGUAGES[source["language"]], "--silent"]
            start = time.monotonic()
            try:
                process = subprocess.run(command, env=env, capture_output=True, text=True,
                                         timeout=args.timeout, check=False)
                native = process.stdout.strip()
                status = "COMPLETE" if process.returncode == 0 else "FAILED"
                error = process.stderr[-2000:] if status != "COMPLETE" else None
            except subprocess.TimeoutExpired as exc:
                native, status = "", "FAILED"
                partial = exc.stdout or b""
                error = f"timeout after {args.timeout}s; captured_stdout_bytes={len(partial)}"
            row = {"id": source["id"], "language": source["language"],
                   "cohort": source["cohort"], "human_reference": source["reference"],
                   "official_offline_text": reference["text"], "native_offline_text": native,
                   "native_status": status, "native_error": error,
                   "native_elapsed_s": time.monotonic() - start,
                   "official_elapsed_s": reference["elapsed_s"],
                   "native_vs_human": score(source["reference"], native, source["language"]),
                   "official_vs_human": score(source["reference"], reference["text"], source["language"]),
                   "native_vs_official": score(reference["text"], native, source["language"])}
            rows.append(row)
            output.write(json.dumps(row, ensure_ascii=False) + "\n")
            output.flush()
            print(f"{row['id']}: {status}", flush=True)
    result = {"schema_version": 1, "purpose": "M0 paired native/reference CPU offline validation",
              "reference_output_sha256": digest(args.reference),
              "dataset_manifest_sha256": digest(args.manifest),
              "native_binary_sha256": digest(binary),
              "model_revision": acquisition["revision"],
              "threads": args.threads, "timeout_s": args.timeout, "languages": {}}
    for language in LANGUAGES:
        group = [row for row in rows if row["language"] == language]
        if not group:
            continue
        entry = {"count": len(group), "failures": sum(row["native_status"] != "COMPLETE" for row in group)}
        for key in ("native_vs_human", "official_vs_human", "native_vs_official"):
            edits = sum(row[key]["substitutions"] + row[key]["deletions"] + row[key]["insertions"] for row in group)
            units = sum(row[key]["reference_units"] for row in group)
            entry[key] = {"metric": "cer" if language == "zh" else "wer", "edits": edits,
                          "reference_units": units, "rate": edits / units if units else None}
        result["languages"][language] = entry
    summary = args.output.with_suffix(".summary.json")
    with summary.open("x") as target:
        target.write(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if all(row["native_status"] == "COMPLETE" for row in rows) else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=Path("datasets/manifests/fleurs_m0.jsonl"))
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--binary", type=Path, default=Path("build/release-cpu/qwen-native-cli"))
    parser.add_argument("--model", type=Path, default=Path("models/qwen3-asr-0.6b"))
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--output", type=Path, required=True)
    raise SystemExit(compare(parser.parse_args()))
