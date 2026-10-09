# CPU multilingual ASR

C++20 Qwen3-ASR-0.6B service with paced WAV simulation, live WebSocket calls, a multi-worker shared-model runtime, and a React dashboard. Inference and WAV call simulation run in C++; Python drives experiments and scores transcripts.

## Start here

- [English five-WAV decode-step/chunk experiment and measured results](docs/ENGLISH_STEP_CHUNK_EXPERIMENT.md)

- [Resumable multi-call streaming: commands, design and evidence](docs/RESUMABLE_STREAMING.md)
- [Final technical report: measurements, architecture and capacity sizing](report.md)
- [Run commands and worker/call selection](docs/CLI_QUICKSTART.md)
- [Architecture and code map](docs/ARCHITECTURE.md)
- [Tests, experiment layouts, and result files](docs/TESTING.md)
- [Assignment questions, decisions, and conditional sizing](docs/ASSIGNMENT_QUESTIONS_AND_ANSWERS.md)
- [50–1,000-call conditional sizing from resumable measurements](report.md#10-conditional-sizing-for-501000-call-legs)

## Setup and cleanup

```bash
# Fetch missing pinned dependencies/model, build C++, and build the dashboard.
python3 scripts/setup.py --frontend
# Also prepare the WAV corpus used by full regression.
python3 scripts/setup.py --frontend --with-data
# Rebuild the selected preset from scratch; clear generated results.
python3 scripts/setup.py --clean-build --clean-results --frontend
# Remove all builds/results without downloading or rebuilding.
python3 scripts/setup.py --clean-all-builds --clean-results --clean-only
```

`--skip-downloads` uses existing local assets/packages. Cleanup retains model weights, dataset inputs and `results/README.md`. System compiler/CMake/Ninja/OpenSSL/OpenBLAS prerequisites are listed in [dependency setup](third_party/README.md).

## Combined regression

```bash
python3 scripts/regression.py          # C++, Python, frontend and mock browser
python3 scripts/regression.py --full   # Also native browser, real pool/UI, matrix and 60s pacing
```

The runner sets up assets/builds, records step logs and `regression.json` under a new `results/regression_*` directory, and fails when a check fails. See [Testing](docs/TESTING.md) for offline/existing-build options and test scopes.

## Build and run

With the pinned dependencies and model already available:

```bash
cmake --preset release-cpu
cmake --build --preset release-cpu
build/release-cpu/asr-cli load --config configs/qwen_prefix_shared.yaml \
  --calls 4 --concurrency 4 --mode network \
  --set workers.processes=2 --set workers.max_sessions_per_process=2
```

The default WAV is a short committed smoke fixture. For distinct longer WAVs, prepare the [dataset inputs](datasets/README.md) and add `--manifest datasets/manifests/demo_calls.jsonl`.

For a new machine, see [dependency setup](third_party/README.md) and [model acquisition](models/README.md). The `dev-mock` preset runs without model weights or OpenBLAS.

## Repository map

```text
apps/          CLI/service wiring and worker executable entry points
src/           C++ audio, runtime engines, scheduling, transport, metrics, storage
configs/       Three runnable presets: mock, native single-call, shared prefix
frontend/      Dashboard and browser tests
scripts/       Dependency fetching and generated Qwen decode guard
tools/         Dataset/model acquisition, experiment driver, scoring
tests/        C++ contracts, Python checks, and small WAV fixtures
datasets/     Input manifests; downloaded/prepared audio stays outside Git
models/       Model acquisition guide; downloaded weights stay outside Git
third_party/   Pinned dependency identities and setup guide
docs/         Current run guide, architecture, testing guide, assignment Q&A
results/      Generated experiment output; ignored except its README
```

`asr-cli` is the user executable. `asr-native-worker` and `asr-prefix-worker` are child-process helpers, launched automatically. `serve` and `load` are commands of the same executable.

Historical results and exploratory reports were removed on `dev-clean`. Existing dataset and model files are inputs and were retained. New results belong under `results/`; previous tracked experiment history remains on `time-multiplexing`.
