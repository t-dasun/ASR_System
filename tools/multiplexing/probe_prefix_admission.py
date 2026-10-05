#!/usr/bin/env python3
"""Verify configured active-call admission without sending audio."""
import argparse
import asyncio
import json
import time
from pathlib import Path

import websockets


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8767)
    parser.add_argument("--slots", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists")
    run_id = f"prefix_admission_{time.time_ns()}"
    accepted = []
    sockets = []
    try:
        for i in range(args.slots + 1):
            socket = await websockets.connect(f"ws://127.0.0.1:{args.port}/v1/asr",
                                               origin="http://127.0.0.1", ping_interval=None)
            sockets.append(socket)
            await socket.send(json.dumps({"v": 1, "type": "start", "run_id": run_id,
                                          "call_id": f"call_{i}", "language": "en",
                                          "seed": 42, "max_chunk_samples": 3200,
                                          "partial_every_ms": 400, "sample_rate_hz": 16000}))
            ready = json.loads(await socket.recv())
            accepted.append(ready)
        assert all(x["status"]["code"] == "none" and x["credits"] == 1
                   for x in accepted[:-1]), accepted
        assert accepted[-1]["status"]["code"] == "resource_exhausted"
        assert accepted[-1]["credits"] == 0
        for socket in sockets[:-1]:
            await socket.send(json.dumps({"v": 1, "type": "cancel"}))
            while True:
                message = json.loads(await socket.recv())
                if message["type"] == "done":
                    assert message["status"]["code"] == "none"
                    break
    finally:
        await asyncio.gather(*(socket.close() for socket in sockets), return_exceptions=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"schema": "experimental_prefix_admission_v1",
                                       "slots": args.slots, "ready": accepted}, indent=2) + "\n")
    print(f"accepted={args.slots} rejected=1")


if __name__ == "__main__":
    asyncio.run(main())
