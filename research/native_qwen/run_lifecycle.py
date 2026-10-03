#!/usr/bin/env python3
"""Persist one sequential native create/free and language-reset diagnostic."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import uuid


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main(args):
    paths = [args.model, args.english, args.indonesian, args.chinese, args.silence]
    paths = [path.resolve(strict=True) for path in paths]
    binary = args.binary.resolve(strict=True)
    directory = Path("results") / (datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ") +
                                   "_native_lifecycle_" + uuid.uuid4().hex[:8])
    directory.mkdir(parents=True, exist_ok=False)
    command = [str(binary), *(str(path) for path in paths)]
    config = {"schema_version": 1, "purpose": "M0 same-process lifecycle diagnostic",
              "command": command, "binary_sha256": digest(binary),
              "audio_sha256": {str(path): digest(path) for path in paths[1:]},
              "model_revision": json.loads((paths[0] / "acquisition.json").read_text())["revision"],
              "timeout_s": args.timeout, "environment_overrides":
              {"OMP_NUM_THREADS": "4", "OPENBLAS_NUM_THREADS": "4",
               "OMP_DYNAMIC": "FALSE", "QWEN_BF16_CACHE_MB": "0"}}
    (directory / "config.json").write_text(json.dumps(config, indent=2) + "\n")
    print(f"Run directory: {directory}", flush=True)
    try:
        process = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout,
                                 env={**os.environ, **config["environment_overrides"]}, check=False)
        (directory / "stdout.log").write_text(process.stdout)
        (directory / "stderr.log").write_text(process.stderr)
        result = json.loads(process.stdout)
        status = {"status": "COMPLETE" if process.returncode == 0 and result["passed"] else "FAILED",
                  "exit_code": process.returncode, "child_peak_rss_kb":
                  resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss}
        (directory / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    except subprocess.TimeoutExpired as error:
        (directory / "stdout.log").write_bytes(error.stdout or b"")
        (directory / "stderr.log").write_bytes(error.stderr or b"")
        status = {"status": "FAILED", "reason": "timeout"}
    except Exception as error:
        status = {"status": "FAILED", "reason": str(error)}
    (directory / "status.json").write_text(json.dumps(status, indent=2) + "\n")
    (directory / "checksums.json").write_text(json.dumps(
        {path.name: digest(path) for path in directory.iterdir() if path.is_file()}, indent=2) + "\n")
    print(json.dumps(status, indent=2))
    return 0 if status["status"] == "COMPLETE" else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/release-cpu/asr-native-lifecycle"))
    parser.add_argument("--model", type=Path, default=Path("models/qwen3-asr-0.6b"))
    parser.add_argument("--english", type=Path, default=Path("datasets/prepared/fleurs_m0/fleurs_en_us_validation_1625_233.wav"))
    parser.add_argument("--indonesian", type=Path, default=Path("datasets/prepared/fleurs_m0/fleurs_id_id_validation_1625_159.wav"))
    parser.add_argument("--chinese", type=Path, default=Path("datasets/prepared/fleurs_m0/fleurs_cmn_hans_cn_validation_1625_303.wav"))
    parser.add_argument("--silence", type=Path, default=Path("results/m0-fixtures/silence_600ms.wav"))
    parser.add_argument("--timeout", type=float, default=60)
    raise SystemExit(main(parser.parse_args()))
