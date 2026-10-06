#!/usr/bin/env python3
"""Pinned FLEURS acquisition and immutable file helpers."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

CONFIGS = {"en": "en_us", "zh": "cmn_hans_cn", "id": "id_id"}
PIN = json.loads((Path(__file__).resolve().parents[2] / "third_party/revisions.lock").read_text())["fleurs"]


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
        if metadata.get("dataset") != "google/fleurs" or metadata.get("revision") != PIN["revision"]:
            raise ValueError("Existing FLEURS source is not at the pinned revision")
    else:
        revision = PIN["revision"]
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

