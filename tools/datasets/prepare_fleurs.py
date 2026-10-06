#!/usr/bin/env python3
"""Expand cached pinned validation data into globally sentence-disjoint cohorts.

The evaluation cohort is a validation holdout, not the official FLEURS test split.
Missing source shards are downloaded at the pinned revision; no inference is performed.
Paths are relative to the repository root.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.datasets.fleurs_source import CONFIGS, PIN, acquire, immutable_text, sha256


def select_cohorts(identities, excluded, seed, tuning, heldout):
    common = set.intersection(*(set(rows) for rows in identities.values())) - set(excluded)
    # Keep the original rank salt so the retained cohorts stay reproducible.
    ranked = sorted(common, key=lambda value: hashlib.sha256(f"m4:{seed}:{value}".encode()).hexdigest())
    if len(ranked) < tuning + heldout:
        raise ValueError("not enough shared sentence IDs after exclusions")
    return {sentence: ("tuning" if rank < tuning else "heldout_validation", rank)
            for rank, sentence in enumerate(ranked[:tuning + heldout])}


def main(args):
    import pyarrow.parquet as pq
    import soundfile as sf

    metadata = acquire(args.raw)
    excluded = json.loads(args.exclude.read_text())
    if metadata["revision"] != "70bb2e84b976b7e960aa89f1c648e09c59f894dd" or metadata["split"] != "validation":
        raise ValueError("unexpected pinned FLEURS source")
    identities = {}
    for language in CONFIGS:
        shard = args.raw / f"{language}_validation.parquet"
        if sha256(shard) != metadata["files"][language]["sha256"]:
            raise ValueError(f"source checksum mismatch: {shard}")
        identities[language] = {}
        for index, row in enumerate(pq.read_table(shard, columns=["id"]).to_pylist()):
            identities[language].setdefault(str(row["id"]), index)
    chosen = select_cohorts(identities, set(excluded), args.seed,
                            args.tuning, args.heldout)
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    for language, config in CONFIGS.items():
        selected = {identities[language][sentence]: (sentence, cohort, rank)
                    for sentence, (cohort, rank) in chosen.items()}
        offset = 0
        for batch in pq.ParquetFile(args.raw / f"{language}_validation.parquet").iter_batches(batch_size=8):
            for local, row in enumerate(batch.to_pylist()):
                index = offset + local
                if index not in selected:
                    continue
                sentence, cohort, rank = selected[index]
                raw = row["audio"]["bytes"]
                audio, rate = sf.read(io.BytesIO(raw), dtype="float32", always_2d=True)
                if rate != 16000 or audio.shape[1] != 1 or not len(audio):
                    raise ValueError(f"unexpected input format: {language}/{sentence}")
                identifier = f"fleurs_{config}_validation_{sentence}_{index}"
                destination = args.output / f"{identifier}.wav"
                encoded = io.BytesIO()
                sf.write(encoded, audio, rate, format="WAV", subtype="PCM_16")
                data = encoded.getvalue()
                if destination.exists():
                    if destination.read_bytes() != data:
                        raise ValueError(f"refusing to overwrite differing WAV: {destination}")
                else:
                    with destination.open("xb") as target:
                        target.write(data)
                records.append({"schema_version": 1, "id": identifier, "file": str(destination),
                                "language": language, "config": config, "dataset": "google/fleurs",
                                "revision": metadata["revision"], "split": "validation", "cohort": cohort,
                                "selection_rank": rank, "seed": args.seed, "source_id": sentence,
                                "source_row": index, "source_path": row["path"],
                                "speaker_id": row.get("speaker_id"), "reference": row["raw_transcription"],
                                "source_transcription": row["transcription"], "reference_field": "raw_transcription",
                                "num_samples": len(audio), "duration_s": len(audio) / rate, "sample_rate": rate,
                                "channels": 1, "encoding": "PCM_16", "condition": "clean",
                                "sha256": hashlib.sha256(data).hexdigest(),
                                "source_audio_sha256": hashlib.sha256(raw).hexdigest(), "license": "CC-BY-4.0",
                                "conversion": {"library": "soundfile", "version": sf.__version__,
                                               "operation": "float32 decode to PCM16 WAV; no resampling"}})
            offset += len(batch)
    records.sort(key=lambda row: (row["language"], row["selection_rank"]))
    summary = {"schema_version": 1, "seed": args.seed, "source": metadata,
               "excluded_sentence_ids_sha256": sha256(args.exclude), "selection": "shared sentence IDs across languages; first recording per sentence; SHA256 rank v1; listed sentence IDs excluded globally",
               "scope": "validation tuning/holdout; reserve official test split for final evaluation",
               "cohorts": {}}
    for cohort in ("tuning", "heldout_validation"):
        rows = [row for row in records if row["cohort"] == cohort]
        path = args.manifests / ("fleurs_tuning.jsonl" if cohort == "tuning" else "fleurs_validation.jsonl")
        immutable_text(path, "".join(json.dumps(row, ensure_ascii=False) + "\n" for row in rows))
        summary["cohorts"][cohort] = {"manifest": str(path), "sha256": sha256(path), "recordings": len(rows),
                                       "audio_seconds": sum(row["duration_s"] for row in rows),
                                       "per_language": {lang: sum(row["language"] == lang for row in rows) for lang in CONFIGS}}
    immutable_text(args.manifests / "fleurs.summary.json", json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw", type=Path, default=Path("datasets/raw/fleurs"))
    parser.add_argument("--exclude", type=Path, default=Path("datasets/manifests/excluded_sentence_ids.json"))
    parser.add_argument("--output", type=Path, default=Path("datasets/prepared/fleurs"))
    parser.add_argument("--manifests", type=Path, default=Path("datasets/manifests"))
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--tuning", type=int, default=20)
    parser.add_argument("--heldout", type=int, default=50)
    args = parser.parse_args()
    if args.tuning < 1 or args.heldout < 1:
        parser.error("cohort counts must be positive")
    main(args)
