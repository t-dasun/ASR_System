#!/usr/bin/env python3
"""Persist one M0 native diagnostic attempt, including failed/timeout output.

This orchestrates validation only. Audio pacing and inference execute in C++.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import time
import uuid


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def process_snapshot(pid):
    """Best-effort Linux sample; sampled peak threads may miss short spikes."""
    try:
        fields = {}
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith(("Threads:", "VmRSS:")):
                key, value = line.split(":", 1)
                fields[key] = int(value.split()[0])
        return fields
    except (FileNotFoundError, PermissionError):
        return {}


def run(args):
    binary = args.binary.resolve(strict=True)
    model = args.model.resolve(strict=True)
    wav = args.wav.resolve(strict=True)
    acquisition = json.loads((model / "acquisition.json").read_text())
    if acquisition["status"] != "verified":
        raise ValueError("Model acquisition is not verified")
    now = datetime.datetime.now(datetime.timezone.utc)
    directory = Path("results") / (now.strftime("%Y%m%dT%H%M%SZ") + "_native_probe_" + uuid.uuid4().hex[:8])
    directory.mkdir(parents=True, exist_ok=False)
    command = [str(binary), str(model), str(wav), str(args.threads), str(args.chunk_ms)]
    if args.language or args.max_new_tokens != 32 or args.refine_final:
        command.append(args.language or "auto")
        command.append(str(args.max_new_tokens))
        if args.refine_final:
            command.append("refine")
    settings = {"OMP_NUM_THREADS": str(args.threads), "OPENBLAS_NUM_THREADS": str(args.threads),
                "OMP_DYNAMIC": "FALSE", "QWEN_BF16_CACHE_MB": "0"}
    config = {"schema_version": 1, "purpose": "M0 diagnostic, not a capacity benchmark",
              "command": command, "environment_overrides": settings,
              "binary_sha256": sha256(binary), "wav_sha256": sha256(wav),
              "model_acquisition": acquisition, "timeout_s": args.timeout,
              "transport_chunk_ms": args.chunk_ms, "decode_step_ms": 2000,
              "max_new_tokens": args.max_new_tokens,
              "refine_final": args.refine_final}
    (directory / "config.json").write_text(json.dumps(config, indent=2) + "\n")
    print(f"Run directory: {directory}", flush=True)
    status = {"status": "RUNNING"}
    (directory / "status.json").write_text(json.dumps(status) + "\n")
    sampled_threads = 0
    sampled_rss_kb = 0
    child_peak_rss_kb = None
    try:
        with (directory / "events.jsonl").open("wb") as out, (directory / "stderr.log").open("wb") as err:
            process = subprocess.Popen(command, env={**os.environ, **settings}, stdout=out, stderr=err)
            deadline = time.monotonic() + args.timeout
            timed_out = False
            while True:
                snapshot = process_snapshot(process.pid)
                sampled_threads = max(sampled_threads, snapshot.get("Threads", 0))
                sampled_rss_kb = max(sampled_rss_kb, snapshot.get("VmRSS", 0))
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    timed_out = True
                    process.kill()
                    process.wait()
                    break
                try:
                    process.wait(timeout=min(0.05, remaining))
                    break
                except subprocess.TimeoutExpired:
                    pass
            child_peak_rss_kb = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        status = {"status": "FAILED" if timed_out else
                  ("COMPLETE" if process.returncode == 0 else "FAILED"),
                  "exit_code": process.returncode, "timeout": timed_out}
        if not timed_out and process.returncode == 0:
            records = [json.loads(line) for line in (directory / "events.jsonl").read_text().splitlines()]
            summaries = [record for record in records if record["type"] == "summary"]
            if len(summaries) != 1 or not summaries[0]["success"]:
                raise ValueError("Missing or unsuccessful diagnostic summary")
            (directory / "summary.json").write_text(json.dumps(summaries[0], indent=2) + "\n")
            print(json.dumps(summaries[0], indent=2), flush=True)
    except Exception as error:
        status = {"status": "FAILED", "reason": str(error)}
    finally:
        status["child_peak_rss_kb"] = child_peak_rss_kb
        status["sampled_peak_rss_kb"] = sampled_rss_kb
        status["sampled_peak_threads"] = sampled_threads
        (directory / "status.json").write_text(json.dumps(status, indent=2) + "\n")
        hashes = {p.name: sha256(p) for p in directory.iterdir() if p.is_file()}
        (directory / "checksums.json").write_text(json.dumps(hashes, indent=2) + "\n")
    return 0 if status["status"] == "COMPLETE" else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/release-cpu/asr-native-probe"))
    parser.add_argument("--model", type=Path, default=Path("models/qwen3-asr-0.6b"))
    parser.add_argument("--wav", type=Path, required=True)
    parser.add_argument("--threads", type=int, choices=range(1, 17), default=4)
    parser.add_argument("--chunk-ms", type=int, choices=[100, 200, 500, 1000], default=200)
    parser.add_argument("--language", choices=["English", "Chinese", "Indonesian"])
    parser.add_argument("--max-new-tokens", type=int, choices=range(1, 257), default=32)
    parser.add_argument("--refine-final", action="store_true")
    parser.add_argument("--timeout", type=float, default=300)
    raise SystemExit(run(parser.parse_args()))
