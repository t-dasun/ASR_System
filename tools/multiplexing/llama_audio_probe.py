"""Probe concurrent complete-audio Qwen requests through a local llama.cpp server.

This is deliberately not a live-PCM test: each request contains a complete WAV.
Token streaming from the response must not be confused with incremental audio input.
"""

from __future__ import annotations

import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen
import wave


def request_json(url: str, timeout: float) -> dict:
    with urlopen(url, timeout=timeout) as response:
        return json.load(response)


def send_call(url: str, model: str, audio_b64: str, language: str, timeout: float, call_id: int) -> dict:
    body = {
        "model": model,
        "messages": [
            {"role": "user", "content": [{"type": "input_audio", "input_audio": {"data": audio_b64}}]},
            {"role": "assistant", "content": f"language {language}<asr_text>"},
        ],
        "stream": True,
        "temperature": 0,
    }
    started = time.monotonic()
    result = {"call_id": call_id, "started_monotonic_s": started, "first_text_ms": None,
              "status": "failed", "text": "", "error": None}
    try:
        data = json.dumps(body, separators=(",", ":")).encode()
        request = Request(url, data=data, headers={"Content-Type": "application/json"}, method="POST")
        with urlopen(request, timeout=timeout) as response:
            for raw_line in response:
                line = raw_line.decode("utf-8", errors="strict").strip()
                if not line.startswith("data: ") or line == "data: [DONE]":
                    continue
                chunk = json.loads(line[6:])
                if "error" in chunk:
                    raise RuntimeError(str(chunk["error"]))
                for choice in chunk.get("choices", []):
                    piece = choice.get("delta", {}).get("content") or ""
                    if piece:
                        if result["first_text_ms"] is None:
                            result["first_text_ms"] = (time.monotonic() - started) * 1000
                        result["text"] += piece
        result["status"] = "complete" if result["text"].strip() else "no_text"
        if result["status"] == "no_text":
            result["error"] = "server completed without transcript text"
    except (HTTPError, URLError, TimeoutError, ValueError, RuntimeError) as exc:
        result["error"] = str(exc)
    result["finished_monotonic_s"] = time.monotonic()
    result["total_ms"] = (result["finished_monotonic_s"] - started) * 1000
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="http://127.0.0.1:8766")
    parser.add_argument("--wav", type=Path, required=True)
    parser.add_argument("--language", choices=("English", "Indonesian", "Chinese"), default="English")
    parser.add_argument("--concurrency", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.concurrency < 1 or args.concurrency > 8:
        parser.error("concurrency must be 1..8")
    if args.output.exists():
        parser.error("output already exists; choose a fresh path")

    wav_bytes = args.wav.read_bytes()
    with wave.open(str(args.wav), "rb") as recording:
        wav_info = {"sample_rate_hz": recording.getframerate(), "channels": recording.getnchannels(),
                    "sample_width_bytes": recording.getsampwidth(), "frames": recording.getnframes()}
    if (wav_info["sample_rate_hz"], wav_info["channels"], wav_info["sample_width_bytes"]) != (16000, 1, 2):
        parser.error("WAV must be 16 kHz mono PCM16")

    base_url = args.server.rstrip("/")
    models = request_json(base_url + "/v1/models", args.timeout)
    model = models["data"][0]["id"]
    audio_b64 = base64.b64encode(wav_bytes).decode("ascii")
    with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        futures = [pool.submit(send_call, base_url + "/v1/chat/completions", model, audio_b64,
                               args.language, args.timeout, i) for i in range(args.concurrency)]
        calls = [future.result() for future in futures]
    report = {
        "schema_version": 1,
        "purpose": "concurrent_complete_audio_only",
        "incremental_pcm_same_request_tested": False,
        "server": base_url,
        "model": model,
        "wav": str(args.wav),
        "wav_sha256": hashlib.sha256(wav_bytes).hexdigest(),
        "wav_info": wav_info,
        "language": args.language,
        "concurrency": args.concurrency,
        "calls": calls,
        "all_complete": all(call["status"] == "complete" for call in calls),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps({"output": str(args.output), "all_complete": report["all_complete"],
                      "total_ms": [round(call["total_ms"], 1) for call in calls]}))
    return 0 if report["all_complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
