#!/usr/bin/env python3
"""Exercise the main C++ serve path and its C++ REST benchmark jobs."""
import argparse
import asyncio
import json
import time
import urllib.error
import urllib.request
from pathlib import Path

import websockets
from probe_prefix_websocket import call, read_wav


def request(port, route, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}{route}", data=data,
                                  headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8767)
    parser.add_argument("--english", type=Path, required=True)
    parser.add_argument("--indonesian", type=Path, required=True)
    parser.add_argument("--mandarin", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists")
    status, capabilities = request(args.port, "/v1/capabilities")
    assert status == 200 and capabilities["engine"] == "qwen_prefix_multiplex_experimental"
    assert capabilities["max_sessions_per_process"] == 8 and not capabilities["process_isolated"]
    status, sweep = request(args.port, "/v1/suites/dry-run", {
        "kind": "sweep", "strategy": "scale", "axes": [{"key": "concurrency", "values": ["1", "2", "4", "8"]}],
        "calls": 2, "concurrency": 1, "languages": ["en"], "mode": "direct"})
    assert status == 200 and len(sweep["cases"]) == 4, sweep
    assert all(not case["skip_reasons"] for case in sweep["cases"]), sweep
    run_id = f"main_service_{time.time_ns()}"
    start = time.monotonic() + 0.5
    pending = [asyncio.create_task(call(args.port, run_id, "english", "en", read_wav(args.english), start)),
               asyncio.create_task(call(args.port, run_id, "indonesian", "id", read_wav(args.indonesian), start))]
    if args.mandarin:
        pending.append(asyncio.create_task(call(args.port, run_id, "mandarin", "zh", read_wav(args.mandarin), start)))
    await asyncio.sleep(1)
    status, active = request(args.port, "/v1/runtime")
    assert status == 200 and active["workers"][0]["active_sessions"] == len(pending), active
    assert active["workers"][0]["runtime_threads"] == active["workers"][0]["blas_threads"] == 4, active
    assert active["system"]["queue_depth_available"]
    status, live_gate = request(args.port, "/v1/suites", {"calls": 1, "concurrency": 1})
    assert status == 409, live_gate
    calls = await asyncio.gather(*pending)
    assert all(item["status"]["code"] == "none" for item in calls)
    assert all(entry["payload"]["event"]["call_id"] == item["call_id"]
               for item in calls for entry in item["events"])
    assert all(any(entry["payload"]["event"]["before_eof"] and entry["payload"]["event"]["text"]
                   for entry in item["events"]) for item in calls)
    print(f"main service: {len(calls)} mixed-language live calls and runtime/gate passed", flush=True)
    suite_body = {"kind": "load", "calls": 4, "concurrency": 4, "languages": ["en"],
                  "mode": "network", "max_failure_rate": 0, "max_p95_send_lag_ms": 1000}
    status, job = request(args.port, "/v1/suites", suite_body)
    assert status == 202, job
    await asyncio.sleep(0.5)
    async with websockets.connect(f"ws://127.0.0.1:{args.port}/v1/asr",
                                  origin="http://127.0.0.1", ping_interval=None) as socket:
        await socket.send(json.dumps({"v": 1, "type": "start", "run_id": run_id,
                                      "call_id": "during_suite", "language": "en", "seed": 42,
                                      "max_chunk_samples": 3200, "partial_every_ms": 400, "sample_rate_hz": 16000}))
        blocked = json.loads(await socket.recv())
        assert blocked["status"]["code"] == "resource_exhausted" and blocked["credits"] == 0
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        status, latest = request(args.port, f"/v1/jobs/{job['job_id']}")
        assert status == 200, latest
        if latest["status"] != "RUNNING":
            break
        await asyncio.sleep(0.5)
    assert latest["status"] == "COMPLETE", latest
    result = latest["result"]
    assert result["completed_calls"] == 4 and result["failed_calls"] == 0, result
    assert result["preflight"]["shared_model_already_loaded"]
    for phase in result["phases"]:
        for item in phase["calls"]:
            assert item["summary"]["engine"] == capabilities["engine"]
            assert item["summary"]["worker_id"] == "prefix_shared_0"
    status, idle = request(args.port, "/v1/runtime")
    assert status == 200 and idle["workers"][0]["active_sessions"] == 0, idle
    assert idle["workers"][0]["process_id"] == active["workers"][0]["process_id"]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"schema": "main_cpp_prefix_service_v1", "capabilities": capabilities,
        "sweep_dry_run": sweep, "active_runtime": active, "interactive_suite_gate": live_gate,
        "live_calls": calls, "suite_live_gate": blocked, "cpp_suite_job": latest,
        "idle_runtime": idle}, indent=2) + "\n")
    print("main service: four-call C++ network suite, model reuse, and suite gate passed", flush=True)


if __name__ == "__main__":
    asyncio.run(main())
