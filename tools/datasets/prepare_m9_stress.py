#!/usr/bin/env python3
"""Build labelled, deterministic speech/silence stress audio; never synthesize references."""
import argparse
from array import array
import hashlib
import json
from pathlib import Path
import sys
import wave

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.datasets.telephone import read_pcm16

RATE = 16000
VERSION = "m9_replay_interrupt_silence_v1"


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def make(source, output, cycles, gap_ms):
    if cycles < 2 or cycles > 60 or gap_ms < 0 or gap_ms > 10000:
        raise ValueError("cycles or silence gap outside bounded stress range")
    source, output = source.resolve(), output.resolve()
    original = read_pcm16(source)
    if len(original) < RATE:
        raise ValueError("source must have at least one second of speech audio")
    gap = array("h", [0]) * (RATE * gap_ms // 1000)
    # One cycle is deliberately interrupted at its midpoint. The last gap tests EOF after silence.
    interrupted = cycles // 2
    audio = array("h")
    schedule = []
    for index in range(cycles):
        segment = original[:len(original) // 2] if index == interrupted else original
        start = len(audio)
        audio.extend(segment)
        schedule.append({"kind": "interrupted_speech" if index == interrupted else "speech_replay",
                         "start_sample": start, "samples": len(segment)})
        if gap:
            start = len(audio)
            audio.extend(gap)
            schedule.append({"kind": "digital_silence", "start_sample": start,
                             "samples": len(gap)})
    output.mkdir(parents=True, exist_ok=False)
    wav_path = output / "stress.wav"
    with wav_path.open("xb") as binary:
        with wave.open(binary, "wb") as target:
            target.setnchannels(1)
            target.setsampwidth(2)
            target.setframerate(RATE)
            if sys.byteorder != "little":
                audio.byteswap()
            target.writeframes(audio.tobytes())
    metadata = {"schema_version": 1, "version": VERSION,
                "source": str(source), "source_sha256": digest(source),
                "output": str(wav_path), "output_sha256": digest(wav_path),
                "sample_rate_hz": RATE, "samples": len(audio),
                "duration_seconds": len(audio) / RATE, "cycles": cycles,
                "gap_ms": gap_ms, "segments": schedule,
                "scope": "replayed speech/interrupted phrase/digital silence; no human reference or WER/CER"}
    with (output / "schedule.json").open("x", encoding="utf-8") as stream:
        json.dump(metadata, stream, indent=2, allow_nan=False)
        stream.write("\n")
    return metadata


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=8)
    parser.add_argument("--gap-ms", type=int, default=2000)
    args = parser.parse_args()
    print(json.dumps(make(args.source, args.output, args.cycles, args.gap_ms), indent=2))
