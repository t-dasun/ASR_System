#!/usr/bin/env python3
"""Measure N paced calls on one experimental shared-model prefix server."""
import argparse
import asyncio
import hashlib
import json
import os
import statistics
import time
from pathlib import Path

from probe_prefix_websocket import call, read_wav


def process_sample(pid):
    stat = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    status = Path(f"/proc/{pid}/status").read_text().splitlines()
    rss_kib = int(next(line.split()[1] for line in status if line.startswith("VmRSS:")))
    return {"time_ns": time.monotonic_ns(), "cpu_ticks": int(stat[11]) + int(stat[12]),
            "rss_bytes": rss_kib * 1024}


async def sample_process(pid, records, stop):
    while not stop.is_set():
        records.append(process_sample(pid))
        try:
            await asyncio.wait_for(stop.wait(), 0.2)
        except asyncio.TimeoutError:
            pass
    records.append(process_sample(pid))


def final_text(item):
    events = [entry["payload"]["event"] for entry in item["events"]]
    finals = [event for event in events if event["kind"] == "final"]
    if len(finals) != 1:
        raise AssertionError(f"expected one final for {item['call_id']}: {events}")
    return finals[0]["text"]


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8767)
    parser.add_argument("--server-pid", type=int, required=True)
    parser.add_argument("--call", action="append", required=True,
                        help="language:path to mono PCM16 16 kHz WAV; repeat for each call")
    parser.add_argument("--reference", type=Path,
                        help="single-call JSON result with expected final text per language")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"refusing to overwrite {args.output}")
    inputs = []
    for spec in args.call:
        language, path = spec.split(":", 1)
        if language not in ("en", "id", "zh"):
            parser.error(f"unsupported language: {language}")
        pcm = read_wav(Path(path))
        inputs.append((language, path, pcm, hashlib.sha256(pcm).hexdigest()))
    expected = {}
    if args.reference:
        for item in json.loads(args.reference.read_text())["calls"]:
            expected[item["language"]] = final_text(item)
    start = time.monotonic() + 0.5
    run_id = f"prefix_load_{time.time_ns()}"
    records = []
    stop = asyncio.Event()
    sampler = asyncio.create_task(sample_process(args.server_pid, records, stop))
    try:
        calls = await asyncio.gather(*(call(args.port, run_id, f"call_{i}", lang, pcm, start)
                                       for i, (lang, _, pcm, _) in enumerate(inputs)))
    finally:
        stop.set()
        await sampler
    for item in calls:
        assert item["worker_id"] == "prefix_shared_0", item
        assert item["status"]["code"] == "none", item
        assert all(entry["payload"]["event"]["call_id"] == item["call_id"]
                   for entry in item["events"]), item
        item["final_text"] = final_text(item)
        item["matches_reference"] = (item["final_text"] == expected[item["language"]]
                                     if item["language"] in expected else None)
        partials = [entry for entry in item["events"]
                    if entry["payload"]["event"]["kind"] == "partial" and
                    entry["payload"]["event"]["text"]]
        item["first_text_ms"] = partials[0]["elapsed_ms"] if partials else item["done_ms"]
        item["first_text_before_eof"] = bool(partials and
                                             partials[0]["elapsed_ms"] < item["eof_sent_ms"] and
                                             partials[0]["payload"]["event"]["before_eof"])
    hz = os.sysconf("SC_CLK_TCK")
    cpu_seconds = (records[-1]["cpu_ticks"] - records[0]["cpu_ticks"]) / hz
    wall_seconds = (records[-1]["time_ns"] - records[0]["time_ns"]) / 1e9
    summary = {"call_count": len(calls),
               "all_completed": all(x["status"]["code"] == "none" for x in calls),
               "pre_eof_text_count": sum(x["first_text_before_eof"] for x in calls),
               "reference_match_count": sum(x["matches_reference"] is True for x in calls),
               "first_text_ms": [x["first_text_ms"] for x in calls],
               "final_ms": [x["done_ms"] for x in calls],
               "mean_first_text_ms": round(statistics.mean(x["first_text_ms"] for x in calls)),
               "mean_final_ms": round(statistics.mean(x["done_ms"] for x in calls)),
               "wall_seconds": round(wall_seconds, 3), "cpu_seconds": round(cpu_seconds, 3),
               "mean_cpu_cores": round(cpu_seconds / wall_seconds, 3),
               "rss_start_bytes": records[0]["rss_bytes"],
               "rss_peak_bytes": max(x["rss_bytes"] for x in records),
               "rss_end_bytes": records[-1]["rss_bytes"]}
    output = {"schema": "experimental_prefix_load_v1", "run_id": run_id,
              "inputs": [{"language": lang, "path": path, "pcm_sha256": digest}
                         for lang, path, _, digest in inputs],
              "reference": str(args.reference) if args.reference else None,
              "summary": summary, "calls": calls}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + "\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    asyncio.run(main())
