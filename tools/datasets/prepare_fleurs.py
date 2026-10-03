#!/usr/bin/env python3
"""Pin/download only FLEURS validation splits and prepare disjoint M0 cohorts."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import urllib.request

CONFIGS = {"en": "en_us", "zh": "cmn_hans_cn", "id": "id_id"}


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def fetch_json(url):
    with urllib.request.urlopen(url, timeout=90) as response:
        return json.load(response)


def immutable_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        if path.read_text() != text:
            raise ValueError(f"Refusing to replace different artifact: {path}")
    else:
        with path.open("x") as output:
            output.write(text)


def acquire(raw):
    raw.mkdir(parents=True, exist_ok=True)
    lock = raw / "source.json"
    if lock.exists():
        metadata = json.loads(lock.read_text())
    else:
        info = fetch_json("https://huggingface.co/api/datasets/google/fleurs")
        revision = info["sha"]
        files = {}
        for language, config in CONFIGS.items():
            entries = fetch_json(f"https://huggingface.co/api/datasets/google/fleurs/tree/{revision}/parquet-data/{config}")
            selected = [entry for entry in entries if Path(entry["path"]).name.startswith("validation-")]
            if len(selected) != 1:
                raise ValueError("Expected one pinned validation shard per language")
            entry = selected[0]
            files[language] = {"path": entry["path"], "size": entry["size"], "sha256": entry["lfs"]["oid"]}
        metadata = {"dataset": "google/fleurs", "revision": revision, "split": "validation", "files": files}
        immutable_text(lock, json.dumps(metadata, indent=2) + "\n")
    for language, item in metadata["files"].items():
        target = raw / f"{language}_validation.parquet"
        if not target.exists():
            temporary = target.with_suffix(".partial")
            if temporary.exists():
                raise ValueError(f"Inspect interrupted download before retrying: {temporary}")
            print(f"Downloading {language} validation ({item['size'] / 1e6:.1f} MB)", flush=True)
            url = f"https://huggingface.co/datasets/google/fleurs/resolve/{metadata['revision']}/{item['path']}"
            with urllib.request.urlopen(url, timeout=180) as response, temporary.open("xb") as output:
                while block := response.read(1024 * 1024):
                    output.write(block)
            if temporary.stat().st_size != item["size"] or sha256(temporary) != item["sha256"]:
                raise ValueError(f"Download integrity failed: {temporary}")
            temporary.rename(target)
        elif sha256(target) != item["sha256"]:
            raise ValueError(f"Cached shard checksum mismatch: {target}")
    return metadata


def prepare(raw, metadata, output, seed, per_language):
    import pyarrow.parquet as pq
    import soundfile as sf

    output.mkdir(parents=True, exist_ok=True)
    records = []
    for language, config in CONFIGS.items():
        shard = raw / f"{language}_validation.parquet"
        identities = pq.read_table(shard, columns=["id", "path"]).to_pylist()
        # One recording per sentence ID; no alternate reading crosses cohorts.
        selected = {}
        for index, row in enumerate(identities):
            key = str(row["id"])
            selected.setdefault(key, index)
        ranked = sorted(selected, key=lambda key: hashlib.sha256(f"{seed}:{key}".encode()).hexdigest())
        if len(ranked) < per_language:
            raise ValueError("Not enough independent sentence IDs")
        chosen = {selected[key]: rank for rank, key in enumerate(ranked[:per_language])}
        offset = 0
        for batch in pq.ParquetFile(shard).iter_batches(batch_size=8):
            for local, row in enumerate(batch.to_pylist()):
                index = offset + local
                if index not in chosen:
                    continue
                source_bytes = row["audio"]["bytes"]
                audio, rate = sf.read(io.BytesIO(source_bytes), dtype="float32", always_2d=True)
                if rate != 16000 or audio.shape[1] != 1 or len(audio) == 0:
                    raise ValueError(f"Unexpected FLEURS audio format: {config}/{row['id']}")
                recording_id = f"fleurs_{config}_validation_{row['id']}_{index}"
                destination = output / f"{recording_id}.wav"
                encoded = io.BytesIO()
                sf.write(encoded, audio, rate, format="WAV", subtype="PCM_16")
                data = encoded.getvalue()
                if destination.exists():
                    if destination.read_bytes() != data:
                        raise ValueError(f"Prepared audio differs: {destination}")
                else:
                    with destination.open("xb") as target:
                        target.write(data)
                records.append({
                    "schema_version": 1, "id": recording_id, "file": str(destination),
                    "language": language, "config": config, "dataset": "google/fleurs",
                    "revision": metadata["revision"], "split": "validation",
                    "cohort": "exploratory" if chosen[index] < per_language // 2 else "acceptance",
                    "selection_rank": chosen[index], "seed": seed, "source_id": str(row["id"]),
                    "source_path": row["path"], "source_row": index,
                    "reference": row["raw_transcription"], "source_transcription": row["transcription"],
                    "reference_field": "raw_transcription", "sample_rate": rate, "channels": 1,
                    "num_samples": len(audio), "duration_s": len(audio) / rate, "encoding": "PCM_16",
                    "condition": "clean", "source_audio_sha256": hashlib.sha256(source_bytes).hexdigest(),
                    "sha256": hashlib.sha256(data).hexdigest(), "license": "CC-BY-4.0",
                    "conversion": {"library": "soundfile", "version": sf.__version__, "operation": "float32 decode to PCM16 WAV; no resampling"},
                })
            offset += len(batch)
        print(f"Prepared {language}: {per_language} recordings", flush=True)
    records.sort(key=lambda row: (row["language"], row["selection_rank"]))
    manifest = Path("datasets/manifests/fleurs_m0.jsonl")
    immutable_text(manifest, "".join(json.dumps(row, ensure_ascii=False) + "\n" for row in records))
    summary = {"source": metadata, "seed": seed, "recordings": len(records),
               "audio_seconds": sum(row["duration_s"] for row in records), "manifest_sha256": sha256(manifest)}
    immutable_text(Path("datasets/manifests/fleurs_m0.summary.json"), json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw", type=Path, default=Path("datasets/raw/fleurs_m0"))
    parser.add_argument("--output", type=Path, default=Path("datasets/prepared/fleurs_m0"))
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--per-language", type=int, default=10)
    args = parser.parse_args()
    if args.per_language < 2 or args.per_language % 2:
        parser.error("per-language must be a positive even number >= 2")
    prepare(args.raw, acquire(args.raw), args.output, args.seed, args.per_language)
