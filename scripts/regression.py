#!/usr/bin/env python3
"""Run the maintained fast checks or the complete real-model regression suite."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time
import urllib.request

from setup import ROOT, ensure_environment, require_tools


def wait_for_dashboard(process, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("dashboard exited; inspect pool-dashboard.log")
        try:
            with urllib.request.urlopen("http://127.0.0.1:5173", timeout=1) as response:
                if response.status == 200:
                    return
        except OSError:
            pass
        time.sleep(0.2)
    raise RuntimeError("dashboard startup timed out; inspect pool-dashboard.log")


def stop_process(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


def main():
    if sys.version_info < (3, 11):
        print("Regression requires Python 3.11 or newer", file=sys.stderr)
        return 1
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--full", action="store_true", help="add real native browser, pacing, pool/UI and matrix checks")
    parser.add_argument("--skip-setup", action="store_true", help="use existing builds, frontend packages and input assets")
    parser.add_argument("--skip-downloads", action="store_true", help="setup/build using local assets/packages only")
    parser.add_argument("--python", type=Path, help="Python with JiWER 4.0.0; otherwise bootstrap .venv-test")
    parser.add_argument("--per-language", type=int, default=10, help="unique WAVs/language in full matrix (2..50)")
    parser.add_argument("--output", type=Path, help="new output directory (default results/regression_TIMESTAMP)")
    args = parser.parse_args()
    if not 2 <= args.per_language <= 50:
        parser.error("--per-language must be 2..50")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S_%fZ")
    output = (args.output or ROOT / "results" / f"regression_{stamp}").resolve()
    if output.exists():
        parser.error("output directory already exists; choose a new directory")
    output.mkdir(parents=True)
    summary = {"status": "RUNNING", "full": args.full, "per_language": args.per_language,
               "steps": [], "output": str(output)}

    def save():
        (output / "regression.json").write_text(json.dumps(summary, indent=2) + "\n")

    def step(name, command, *, cwd=ROOT, env=None):
        print(f"\n[{name}] {' '.join(map(str, command))}", flush=True)
        record = {"name": name, "command": list(map(str, command)), "status": "RUNNING"}
        summary["steps"].append(record)
        save()
        started = time.monotonic()
        with (output / f"{name}.log").open("w") as log:
            # Stream merged output and retain it; failures stop subsequent checks.
            with subprocess.Popen(record["command"], cwd=cwd, env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, start_new_session=True) as process:
                try:
                    for line in process.stdout:
                        print(line, end="", flush=True)
                        log.write(line)
                    result = process.wait()
                except BaseException:
                    stop_process(process)
                    raise
        record.update(status="PASS" if result == 0 else "FAIL", exit_code=result,
                      elapsed_seconds=round(time.monotonic() - started, 3))
        save()
        if result:
            raise RuntimeError(f"{name} failed; inspect {output / (name + '.log')}")

    save()
    try:
        require_tools(["cmake", "npm", "node", os.environ.get("ASR_CHROME", "/usr/bin/google-chrome")])
        if not args.skip_setup:
            common = [sys.executable, "scripts/setup.py"]
            offline = ["--skip-downloads"] if args.skip_downloads else []
            step("setup-native", [*common, "--frontend", *(["--with-data"] if args.full else []), *offline])
            step("setup-mock", [*common, "--preset", "dev-mock", *offline])
        # Preserve the venv executable path: resolving its symlink loses the venv.
        python = Path(os.path.abspath(args.python)) if args.python else ROOT / ".venv-test/bin/python"
        if args.python is None and not (args.skip_setup or args.skip_downloads):
            python = ensure_environment(".venv-test", ["jiwer==4.0.0"])
        if not python.is_file():
            raise RuntimeError("Test Python missing; use --python PATH or run setup-enabled regression")
        step("python-dependencies", [python, "-c", "import importlib.metadata; assert importlib.metadata.version('jiwer') == '4.0.0'"])
        step("cpp-native", ["ctest", "--preset", "release-cpu"])
        step("cpp-mock", ["ctest", "--preset", "dev-mock"])
        step("python", [python, "-m", "unittest", "discover", "-s", "tests/python", "-p", "*_test.py", "-v"])
        frontend = ROOT / "frontend"
        step("frontend-build", ["npm", "run", "build"], cwd=frontend)
        step("frontend-unit", ["npm", "test"], cwd=frontend)
        env = dict(os.environ, ASR_CLI=str(ROOT / "build/release-cpu/asr-cli"))
        step("browser-mock", ["npm", "run", "test:e2e"], cwd=frontend, env=env)
        if args.full:
            step("audio-pacing", [ROOT / "build/release-cpu/audio-realtime-60-test"])
            native_env = dict(env, ASR_DEMO_EVIDENCE_DIR=str(output / "native-browser"))
            step("browser-native", ["npm", "run", "test:e2e:native"], cwd=frontend, env=native_env)
            # The runner owns this server; reject an already occupied UI port.
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 5173))
            with (output / "pool-dashboard.log").open("w") as log:
                dashboard = subprocess.Popen(["npm", "run", "dev"], cwd=frontend,
                                             stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    wait_for_dashboard(dashboard)
                    step("pool-service", [python, "tools/testing/verify_pool_service.py", "--output", output / "pool-service"], env=env)
                finally:
                    stop_process(dashboard)
            matrix = output / "shared-matrix"
            step("shared-matrix", [python, "tools/testing/run_shared_pool_matrix.py", "--output", matrix,
                                   "--per-language", args.per_language])
            matrix_status = json.loads((matrix / "status.json").read_text())["status"]
            summary["steps"][-1]["matrix_outcome"] = matrix_status
            if matrix_status != "COMPLETE":
                summary["steps"][-1]["status"] = "FAIL"
                raise RuntimeError(f"Matrix outcome {matrix_status}: inspect {matrix / 'comparison.md'}")
        summary["status"] = "PASS"
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError, KeyboardInterrupt) as error:
        summary.update(status="FAIL", error=str(error) or type(error).__name__)
        if summary["steps"] and summary["steps"][-1]["status"] == "RUNNING":
            summary["steps"][-1]["status"] = "FAIL"
        print(f"\nRegression failed: {error}", file=sys.stderr)
    finally:
        save()
    print(f"\nRegression {summary['status']}: {output / 'regression.json'}", flush=True)
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
