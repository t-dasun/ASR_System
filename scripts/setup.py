#!/usr/bin/env python3
"""Set up pinned assets, build the project, or clean generated output."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PRESETS = ("release-cpu", "dev-mock")


def run(command, *, cwd=ROOT, env=None):
    print("+ " + " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=cwd, env=env, check=True)


def remove_generated(path):
    """Delete only a known generated directory; never follow a root symlink."""
    allowed = {ROOT / "build", ROOT / "frontend/dist"}
    allowed.update(ROOT / "build" / preset for preset in PRESETS)
    if path not in allowed:
        raise ValueError(f"not a generated build directory: {path}")
    if path.is_symlink() or path.parent.is_symlink():
        raise ValueError(f"refusing cleanup through a symlink: {path}")
    if path.exists():
        print(f"Removing {path.relative_to(ROOT)}", flush=True)
        shutil.rmtree(path)


def clean_results():
    directory = ROOT / "results"
    if directory.is_symlink():
        raise ValueError("refusing cleanup through a results symlink")
    if not directory.exists():
        return
    print("Removing generated results (keeping results/README.md)", flush=True)
    for path in directory.iterdir():
        if path.name == "README.md":
            continue
        if path.is_dir() and not path.is_symlink():
            shutil.rmtree(path)
        else:
            path.unlink()


def require_tools(names):
    missing = [name for name in names if not shutil.which(name)]
    if missing:
        raise RuntimeError("Install missing system tools first: " + ", ".join(missing))


def ensure_environment(name, requirements):
    """Create a project-local environment and install missing pinned packages."""
    directory = ROOT / name
    if directory.is_symlink():
        raise ValueError(f"refusing environment symlink: {directory}")
    python = directory / "bin/python"
    if not python.exists():
        run([sys.executable, "-m", "venv", directory])
    code = "import importlib.metadata; " + "; ".join(
        f"assert importlib.metadata.version({package!r}) == {version!r}"
        for package, version in (item.split("==") for item in requirements)
    )
    check = subprocess.run([str(python), "-c", code], capture_output=True)
    if check.returncode:
        run([python, "-m", "pip", "install", *requirements])
    return python


def build(args):
    # Validate CLI arguments before touching generated output.
    if args.clean_only and not (args.clean_build or args.clean_all_builds or args.clean_results):
        raise ValueError("--clean-only requires a cleanup option")
    if args.preset == "dev-mock" and args.with_data:
        raise ValueError("--with-data requires --preset release-cpu")
    if args.clean_all_builds:
        remove_generated(ROOT / "build")
        remove_generated(ROOT / "frontend/dist")
    elif args.clean_build:
        remove_generated(ROOT / "build" / args.preset)
        if args.frontend:
            remove_generated(ROOT / "frontend/dist")
    if args.clean_results:
        clean_results()
    if args.clean_only:
        return
    require_tools(["cmake", "ninja", "git", "cc", "c++"])
    if not args.skip_downloads:
        run(["bash", "scripts/fetch_foundation.sh"])
        if args.preset == "release-cpu":
            run(["bash", "scripts/fetch_native.sh"])
            run([sys.executable, "tools/models/acquire_model.py"])
    if args.reports:
        requirements = (ROOT / "tools/testing/plot_requirements.txt").read_text().splitlines()
        if args.skip_downloads:
            if not (ROOT / ".venv-report/bin/python").exists():
                raise RuntimeError("Report environment missing; rerun without --skip-downloads")
        else:
            ensure_environment(".venv-report", requirements)
    if args.with_data:
        requirements = (ROOT / "tools/datasets/requirements.txt").read_text().splitlines()
        if args.skip_downloads:
            # Offline mode never installs packages or fetches source shards.
            missing = [ROOT / relative for relative in (
                "datasets/raw/fleurs/source.json", "datasets/prepared/fleurs"
            ) if not (ROOT / relative).exists()]
            if missing:
                raise RuntimeError("Offline dataset inputs missing: " + ", ".join(map(str, missing)))
        else:
            python = ensure_environment(".venv-data", requirements)
            run([python, "tools/datasets/prepare_fleurs.py"])
    run(["cmake", "--preset", args.preset])
    run(["cmake", "--build", "--preset", args.preset, "--parallel", args.jobs])
    if args.frontend:
        require_tools(["npm", "node"])
        frontend = ROOT / "frontend"
        if not args.skip_downloads:
            # npm ci enforces package-lock.json, even for an existing node_modules.
            run(["npm", "ci"], cwd=frontend)
        elif not (frontend / "node_modules/.bin/vite").exists():
            raise RuntimeError("Frontend packages missing; rerun without --skip-downloads")
        run(["npm", "run", "build"], cwd=frontend)
    print(f"Ready: build/{args.preset}/asr-cli", flush=True)


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--preset", choices=PRESETS, default="release-cpu")
    result.add_argument("--jobs", type=int, default=4)
    result.add_argument("--frontend", action="store_true", help="install locked npm packages and build dashboard")
    result.add_argument("--reports", action="store_true", help="prepare optional Matplotlib report environment")
    result.add_argument("--with-data", action="store_true", help="fetch/prepare pinned FLEURS WAV inputs")
    result.add_argument("--skip-downloads", action="store_true", help="use local assets/packages only; no downloads")
    result.add_argument("--clean-build", action="store_true", help="remove selected preset before rebuilding")
    result.add_argument("--clean-all-builds", action="store_true", help="remove all CMake builds and frontend/dist")
    result.add_argument("--clean-results", action="store_true", help="remove results except its README")
    result.add_argument("--clean-only", action="store_true", help="perform requested cleanup and exit without setup/build")
    return result


def main():
    if sys.version_info < (3, 11):
        print("Setup requires Python 3.11 or newer", file=sys.stderr)
        return 1
    cli = parser()
    args = cli.parse_args()
    if args.jobs < 1:
        cli.error("--jobs must be positive")
    try:
        build(args)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Setup failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
