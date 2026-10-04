#!/usr/bin/env python3
"""Read-only host/build discovery; optional exclusive JSON evidence output."""
import argparse
import json
import os
import pathlib
import platform
import shutil
import subprocess
import datetime


def command(args):
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=15)
        return {"exit_code": result.returncode, "stdout": result.stdout.strip(), "stderr": result.stderr.strip()}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"error": str(error)}


def optional_text(path):
    try:
        return pathlib.Path(path).read_text().strip()
    except OSError:
        return None


def capture():
    return {
        "schema_version": 1,
        "captured_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(),
        "cpu_affinity": sorted(os.sched_getaffinity(0)),
        "cpu": command(["lscpu", "--json"]),
        "memory": pathlib.Path("/proc/meminfo").read_text(),
        "tools": {name: shutil.which(name) for name in ["cmake", "ninja", "gcc", "g++", "git", "python3", "uv", "ffmpeg"]},
        "compiler": command(["g++", "--version"]),
        "cmake": command(["cmake", "--version"]),
        "native_revision": command(["git", "-C", "third_party/qwen-asr", "rev-parse", "HEAD"]),
        "governor": optional_text("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "note": "Preflight snapshot; not a benchmark result or complete experiment environment capture.",
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    payload = json.dumps(capture(), indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x") as destination:
            destination.write(payload)
        print(args.output)
    else:
        print(payload)
