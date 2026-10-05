#!/usr/bin/env python3
"""Cancel one prefix call while another paced call continues."""
import argparse
import asyncio
import json
import time
from pathlib import Path

import websockets
from probe_prefix_websocket import call, read_wav


async def cancelled_call(port, run_id, pcm, start):
    result = {"call_id": "cancelled", "events": []}
    async with websockets.connect(f"ws://127.0.0.1:{port}/v1/asr",
                                  origin="http://127.0.0.1", ping_interval=None) as socket:
        await socket.send(json.dumps({"v": 1, "type": "start", "run_id": run_id,
                                      "call_id": "cancelled", "language": "en", "seed": 42,
                                      "max_chunk_samples": 3200, "partial_every_ms": 400,
                                      "sample_rate_hz": 16000}))
        ready = json.loads(await socket.recv())
        assert ready["type"] == "ready" and ready["credits"] == 1, ready
        for sequence in range(20):
            await asyncio.sleep(max(0, start + sequence * 0.2 - time.monotonic()))
            chunk = pcm[sequence * 6400:(sequence + 1) * 6400]
            await socket.send(json.dumps({"v": 1, "type": "chunk", "sequence": sequence,
                                          "first_sample": sequence * 3200,
                                          "sample_count": len(chunk) // 2}))
            await socket.send(chunk)
            while True:
                message = json.loads(await socket.recv())
                if message["type"] == "event":
                    result["events"].append(message["event"])
                elif message["type"] == "ack":
                    assert message["status"]["code"] == "none", message
                    break
        await asyncio.sleep(max(0, start + 4.0 - time.monotonic()))
        result["cancel_sent_ms"] = round((time.monotonic() - start) * 1000)
        await socket.send(json.dumps({"v": 1, "type": "cancel"}))
        while True:
            message = json.loads(await socket.recv())
            if message["type"] == "event":
                result["events"].append(message["event"])
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
        parser.error("output already exists")
    start = time.monotonic() + 0.5
    run_id = f"prefix_cancel_{time.time_ns()}"
    cancelled, survivor = await asyncio.gather(
        cancelled_call(args.port, run_id, read_wav(args.english), start),
        call(args.port, run_id, "survivor", "id", read_wav(args.indonesian), start))
    assert cancelled["status"]["code"] == "none", cancelled
    assert any(event["kind"] == "stopped" and event["error"]["code"] == "cancelled"
               for event in cancelled["events"]), cancelled
    assert survivor["status"]["code"] == "none", survivor
    assert any(entry["payload"]["event"]["kind"] == "final" for entry in survivor["events"])
    assert all(entry["payload"]["event"]["call_id"] == "survivor"
               for entry in survivor["events"])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"schema": "experimental_prefix_cancel_v1",
                                       "cancelled": cancelled, "survivor": survivor}, indent=2) + "\n")
    print("cancel_delay_ms=", cancelled["done_ms"] - cancelled["cancel_sent_ms"],
          "survivor_done_ms=", survivor["done_ms"])


if __name__ == "__main__":
    asyncio.run(main())
