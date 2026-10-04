#!/usr/bin/env python3
"""Create immutable, hash-checked clean/telephone FLEURS pair manifests."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.datasets.telephone import (HIGH_HZ, LOW_HZ, MU, TAPS, VERSION, write_telephone)
from tools.evaluation.run_measured import select_rows


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def prepare(manifest, count, output):
    rows = select_rows(manifest, count)
    output.mkdir(parents=True, exist_ok=False)
    (output / "prepared").mkdir()
    write_json(output / "status.json", {"status": "PREPARING"})
    clean, telephone, combined, pairs = [], [], [], []
    languages = {"en": 0, "id": 1, "zh": 2}
    for row in rows:
        original = ROOT / row["file"]
        derived = output / "prepared" / f"{row['id']}_tel.wav"
        samples = write_telephone(original, derived)
        if samples != row["num_samples"]:
            raise ValueError(f"sample count changed for {row['id']}")
        clean_row = {**row, "pair_id": row["id"], "condition": "clean"}
        tel_row = {**row, "id": f"{row['id']}_tel", "pair_id": row["id"],
                   "file": str(derived), "sha256": digest(derived),
                   "condition": VERSION, "source_audio_sha256": row["sha256"],
                   "conversion": {"operation": VERSION, "input_sha256": row["sha256"],
                                  "source_sample_rate_hz": 16000, "codec_sample_rate_hz": 8000,
                                  "output_sample_rate_hz": 16000, "fir_taps": TAPS,
                                  "band_hz": [LOW_HZ, HIGH_HZ], "mulaw_mu": MU}}
        clean.append(clean_row)
        telephone.append(tel_row)
        pair_index = row["selection_rank"]
        telephone_first = (pair_index + languages[row["language"]]) % 2 == 1
        for position, item in enumerate((tel_row, clean_row) if telephone_first else
                                        (clean_row, tel_row)):
            combined.append({**item, "selection_rank": pair_index * 2 + position})
        pairs.append({"pair_id": row["id"], "language": row["language"],
                      "clean_sha256": row["sha256"], "telephone_sha256": tel_row["sha256"],
                      "samples": samples})
    for name, values in (("clean.jsonl", clean), ("telephone.jsonl", telephone),
                         ("combined.jsonl", combined)):
        with (output / name).open("x", encoding="utf-8") as stream:
            for value in values:
                stream.write(json.dumps(value, ensure_ascii=False, allow_nan=False) + "\n")
    write_json(output / "pairing.json", {
        "schema_version": 1, "source_manifest": str(manifest), "source_manifest_sha256": digest(manifest),
        "telephone_version": VERSION, "telephone_code_sha256": digest(ROOT / "tools/datasets/telephone.py"),
        "cohort": "FLEURS validation tuning, not test", "calls_per_condition": len(rows),
        "combined_order": "paired, language-interleaved, alternating condition first by pair rank and language",
        "pairs": pairs})
    write_json(output / "status.next.json", {"status": "PREPARED"})
    (output / "status.next.json").replace(output / "status.json")
    return output / "pairing.json"


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path,
                        default=ROOT / "datasets/manifests/fleurs_m4_tuning.jsonl")
    parser.add_argument("--per-language", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.per_language < 1:
        parser.error("--per-language must be positive")
    print(prepare(args.manifest.resolve(), args.per_language, args.output.resolve()))
