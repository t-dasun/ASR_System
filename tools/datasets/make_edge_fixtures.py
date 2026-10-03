#!/usr/bin/env python3
"""Deterministic PCM16 fixtures for M0 silence and short-tail diagnostics."""
import hashlib
import json
from pathlib import Path
import wave

ROOT = Path("results/m0-fixtures")
RATE = 16000


def write_silence(path, seconds):
    samples = round(seconds * RATE)
    with wave.open(str(path), "wb") as target:
        target.setnchannels(1)
        target.setsampwidth(2)
        target.setframerate(RATE)
        target.writeframes(b"\x00\x00" * samples)
    return {"file": str(path), "duration_s": seconds, "samples": samples,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    records = [write_silence(ROOT / f"silence_{int(seconds * 1000)}ms.wav", seconds)
               for seconds in (0.6, 3.0)]
    manifest = ROOT / "manifest.json"
    manifest.write_text(json.dumps({"schema_version": 1, "sample_rate": RATE,
                                    "encoding": "PCM_16", "fixtures": records}, indent=2) + "\n")
    print(manifest)


if __name__ == "__main__":
    main()
