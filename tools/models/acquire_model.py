#!/usr/bin/env python3
"""Acquire the official small Qwen checkpoint at an immutable revision.

Uses only the Python standard library. Model data stays outside Git.
"""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

MODEL = "Qwen/Qwen3-ASR-0.6B"
REQUIRED = {"config.json", "generation_config.json", "model.safetensors", "vocab.json", "merges.txt"}
OPTIONAL = {"preprocessor_config.json", "processor_config.json", "tokenizer_config.json", "tokenizer.json", "chat_template.json", "README.md"}
PIN = json.loads((Path(__file__).resolve().parents[2] / "third_party/revisions.lock").read_text())["qwen3_asr_0_6b"]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def acquire(destination, metadata_only=False):
    destination.mkdir(parents=True, exist_ok=True)
    lock = destination / "acquisition.json"
    if lock.exists():
        manifest = json.loads(lock.read_text())
        if manifest.get("model") != MODEL or manifest.get("revision") != PIN["revision"]:
            raise RuntimeError("Existing model acquisition is not at the pinned revision")
        missing = (REQUIRED | OPTIONAL) - {item["name"] for item in manifest["files"]}
        # A verified acquisition already records every available pinned artifact.
        # Absent optional files need not force a network lookup on each setup.
        if missing & REQUIRED or (missing and manifest.get("status") != "verified"):
            with urllib.request.urlopen(f"https://huggingface.co/api/models/{MODEL}/revision/{manifest['revision']}?blobs=true", timeout=60) as response:
                pinned_info = json.load(response)
            available = {item["rfilename"]: item for item in pinned_info["siblings"]}
            if not REQUIRED <= available.keys():
                raise RuntimeError("Pinned model revision no longer exposes required files")
            for name in sorted(missing & available.keys()):
                manifest["files"].append({"name": name, "expected_size": available[name].get("size"),
                                          "expected_sha256": available[name].get("lfs", {}).get("sha256")})
            manifest["files"].sort(key=lambda item: item["name"])
            manifest["status"] = "metadata_only"
            lock.write_text(json.dumps(manifest, indent=2) + "\n")
    else:
        with urllib.request.urlopen(
                f"https://huggingface.co/api/models/{MODEL}/revision/{PIN['revision']}?blobs=true",
                timeout=60) as response:
            info = json.load(response)
        if info.get("sha") != PIN["revision"]:
            raise RuntimeError("Model metadata does not match pinned revision")
        available = {item["rfilename"]: item for item in info["siblings"]}
        if not REQUIRED <= available.keys():
            raise RuntimeError("Required native checkpoint files are missing")
        manifest = {
            "model": MODEL, "revision": PIN["revision"], "status": "metadata_only",
            "files": [{"name": name, "expected_size": available[name].get("size"),
                       "expected_sha256": available[name].get("lfs", {}).get("sha256")}
                      for name in sorted((REQUIRED | OPTIONAL) & available.keys())],
        }
        with lock.open("x") as output:
            json.dump(manifest, output, indent=2)
    print(json.dumps(manifest, indent=2), flush=True)
    if metadata_only:
        return
    for item in manifest["files"]:
        target = destination / item["name"]
        if target.exists():
            expected = item.get("sha256") or item["expected_sha256"]
            if not expected or digest(target) != expected:
                raise RuntimeError(f"Existing unverified file: {target}; refusing to overwrite")
        else:
            temporary = target.with_suffix(target.suffix + ".partial")
            if temporary.exists():
                raise RuntimeError(f"Partial download exists: {temporary}; inspect before retrying")
            url = f"https://huggingface.co/{MODEL}/resolve/{manifest['revision']}/{item['name']}"
            print(f"Downloading {item['name']}", flush=True)
            with urllib.request.urlopen(url, timeout=120) as response, temporary.open("xb") as output:
                while block := response.read(1024 * 1024):
                    output.write(block)
            if item["expected_size"] is not None and temporary.stat().st_size != item["expected_size"]:
                raise RuntimeError(f"Size mismatch: {temporary}")
            downloaded_sha256 = digest(temporary)
            if item["expected_sha256"] and downloaded_sha256 != item["expected_sha256"]:
                raise RuntimeError(f"Hash mismatch: {temporary}")
            if item["name"] == "model.safetensors" and downloaded_sha256 != PIN["weights_sha256"]:
                raise RuntimeError("Downloaded model weights differ from repository pin")
            temporary.rename(target)
        item["sha256"] = digest(target)
        if item["name"] == "model.safetensors" and item["sha256"] != PIN["weights_sha256"]:
            raise RuntimeError("Model weights differ from repository pin")
        # Checkpoint acquisition state only; these are not sealed experiment results.
        lock.write_text(json.dumps(manifest, indent=2) + "\n")
    manifest["status"] = "verified"
    lock.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Verified checkpoint at {destination}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--destination", type=Path, default=Path("models/qwen3-asr-0.6b"))
    parser.add_argument("--metadata-only", action="store_true")
    args = parser.parse_args()
    acquire(args.destination, args.metadata_only)
