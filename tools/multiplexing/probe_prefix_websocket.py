#!/usr/bin/env python3
"""Pace two real PCM calls into the experimental shared-model WebSocket server."""
import argparse
import asyncio
import json
import struct
import time
import wave
from pathlib import Path

import websockets


def read_wav(path):
    with wave.open(str(path), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, 16000):
            raise ValueError(f"expected mono PCM16 16 kHz: {path}")
        return wav.readframes(wav.getnframes())


async def call(port, run_id, call_id, language, pcm, start):
    result = {"call_id": call_id, "language": language, "sample_count": len(pcm) // 2,
              "events": [], "ack_count": 0}
    async with websockets.connect(f"ws://127.0.0.1:{port}/v1/asr", origin="http://127.0.0.1",
                                  max_size=65536, ping_interval=None) as socket:
        await socket.send(json.dumps({"v": 1, "type": "start", "run_id": run_id,
                                      "call_id": call_id, "language": language, "seed": 42,
                                      "max_chunk_samples": 3200, "partial_every_ms": 400,
                                      "sample_rate_hz": 16000}))
        ready = json.loads(await socket.recv())
        if ready.get("type") != "ready" or ready["status"]["code"] != "none":
            raise RuntimeError(f"call rejected: {ready}")
        result["worker_id"] = ready["worker_id"]
        sequence = 0
        for offset in range(0, len(pcm), 6400):
            target = start + offset / 32000
            await asyncio.sleep(max(0, target - time.monotonic()))
            chunk = pcm[offset:offset + 6400]
            await socket.send(json.dumps({"v": 1, "type": "chunk", "sequence": sequence,
                                          "first_sample": offset // 2,
                                          "sample_count": len(chunk) // 2,
                                          "scheduled_ready_ns": 0, "sent_ns": 0}))
            await socket.send(chunk)
            while True:
                message = json.loads(await socket.recv())
                if message["type"] == "event":
                    result["events"].append({"elapsed_ms": round((time.monotonic() - start) * 1000),
                                             "payload": message})
                elif message["type"] == "ack":
                    if message["status"]["code"] != "none":
                        raise RuntimeError(f"chunk rejected: {message}")
                    result["ack_count"] += 1
                    break
            sequence += 1
        await asyncio.sleep(max(0, start + len(pcm) / 32000 - time.monotonic()))
        result["eof_sent_ms"] = round((time.monotonic() - start) * 1000)
        await socket.send(json.dumps({"v": 1, "type": "eof",
                                      "expected_next_sequence": sequence,
                                      "total_samples": len(pcm) // 2}))
        while True:
            message = json.loads(await socket.recv())
            if message["type"] == "event":
                result["events"].append({"elapsed_ms": round((time.monotonic() - start) * 1000),
                                         "payload": message})
            elif message["type"] == "done":
                result["done_ms"] = round((time.monotonic() - start) * 1000)
                result["status"] = message["status"]
                break
    return result


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8767)
    parser.add_argument("--english", type=Path, required=True)
    parser.add_argument("--indonesian", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"refusing to overwrite {args.output}")
    english, indonesian = read_wav(args.english), read_wav(args.indonesian)
    start = time.monotonic() + 0.3
    run_id = f"prefix_ws_probe_{time.time_ns()}"
    pending = [asyncio.create_task(call(args.port, run_id, "english", "en", english, start)),
               asyncio.create_task(call(args.port, run_id, "indonesian", "id", indonesian, start))]
    await asyncio.sleep(0.5)
    async with websockets.connect(f"ws://127.0.0.1:{args.port}/v1/asr",
                                  origin="http://127.0.0.1", ping_interval=None) as third:
        await third.send(json.dumps({"v": 1, "type": "start", "run_id": run_id,
                                     "call_id": "third", "language": "en", "seed": 42,
                                     "max_chunk_samples": 3200, "partial_every_ms": 400,
                                     "sample_rate_hz": 16000}))
        third_ready = json.loads(await third.recv())
        assert third_ready["type"] == "ready" and third_ready["credits"] == 0
        assert third_ready["status"]["code"] == "resource_exhausted"
    calls = await asyncio.gather(*pending)
    for item in calls:
        events = [entry["payload"]["event"] for entry in item["events"]]
        assert item["status"]["code"] == "none", item
        assert any(event["kind"] == "partial" and event["before_eof"] and
                   event["text"] and entry["elapsed_ms"] < item["eof_sent_ms"]
                   for entry in item["events"] for event in [entry["payload"]["event"]]), item
        assert sum(event["kind"] == "final" for event in events) == 1, item
        assert all(event["call_id"] == item["call_id"] for event in events), item
    assert calls[0]["worker_id"] == calls[1]["worker_id"] == "prefix_shared_0"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"schema": "experimental_prefix_websocket_v1",
                                       "third_slot_ready": third_ready,
                                       "calls": calls}, indent=2) + "\n")
    for item in calls:
        print(item["call_id"], "preview_ms=", next(x["elapsed_ms"] for x in item["events"]
              if x["payload"]["event"]["kind"] == "partial"), "eof_ms=", item["eof_sent_ms"],
              "done_ms=", item["done_ms"])


if __name__ == "__main__":
    asyncio.run(main())
