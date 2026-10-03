# CPU ASR evaluation platform

Modular C++20 platform in development for evaluating Qwen3-ASR on CPU using progressively streamed audio, reproducible experiments, and Google FLEURS as the initial dataset.

M0–M4 are implemented: pinned CPU Qwen artifacts, modular C++ contracts, WAV/paced delivery, a process-isolated native adapter, measured single calls, and offline accuracy evaluation. The native stream can truncate provisional text; the adapter performs separate full-audio EOF refinement. A sequential measured experiment is runnable, while the concurrent experiment service, server/UI, and production capacity qualification remain ahead.

See [implementation status and measured smoke evidence](docs/IMPLEMENTATION_STATUS.md).
For a file-by-file explanation of the M0 C++/Python code, generated JSON/JSONL, third-party boundaries, and current YAML status, see the [M0 code and artifact guide](docs/m0-code.md).

M1 adds interchangeable C++ engine/session contracts, a deterministic mock engine, clocks, strict YAML configuration, repositories, and a runnable mock CLI. M2 adds WAV preparation and paced delivery. M3 adds a process-isolated Qwen3-ASR adapter through the same C++ engine interface. See the [M1 guide](docs/m1-code.md), [M2 audio guide](docs/m2-code.md), and [M3 native guide](docs/m3-code.md).

## Build and test

Requirements: Linux, CMake 3.24+, Ninja, GCC/G++ with C++20, Git, Python 3 standard library, and OpenBLAS development headers/library. On Fedora use `sudo dnf install openblas-devel`; a project-local header fallback is documented in [native dependencies](third_party/README.md).

Run from the repository root:

```bash
# One-time pinned C++ dependency bootstrap (requires network).
bash scripts/fetch_foundation.sh
# After bootstrap: no models, Python, network, or native dependencies required.
cmake --preset dev-mock
cmake --build --preset dev-mock
ctest --preset dev-mock

./build/dev-mock/asr-cli validate --config configs/mock_baseline.yaml
./build/dev-mock/asr-cli dry-run --config configs/mock_baseline.yaml
./build/dev-mock/asr-cli run --config configs/mock_baseline.yaml

# WAV input, exact dry-run count, and optional real-time pacing.
./build/dev-mock/asr-cli dry-run --config configs/mock_baseline.yaml \
  --set audio.source=wav --set audio.path=../third_party/qwen-asr/samples/jfk.wav
./build/dev-mock/audio-realtime-60-test # explicit 60-second pacing gate

# Pinned native CPU build; fetch requires network access.
bash scripts/fetch_native.sh
cmake --preset release-cpu
cmake --build --preset release-cpu
ctest --preset release-cpu

# Native engine through the modular CLI; requires the verified local model.
./build/release-cpu/asr-cli run --config configs/mock_baseline.yaml \
  --set model.runtime=qwen_native \
  --set model.path=../models/qwen3-asr-0.6b \
  --set audio.source=wav \
  --set audio.path=../third_party/qwen-asr/samples/jfk.wav \
  --set audio.realtime_pacing=true

# About 1.88 GB of official weights plus metadata/tokenizer files.
python3 tools/models/acquire_model.py --metadata-only
python3 tools/models/acquire_model.py

# C++ real-time paced input; Python only launches and records the diagnostic.
python3 research/native_qwen/run_probe.py \
  --wav third_party/qwen-asr/samples/jfk.wav --threads 4 --chunk-ms 200
```

The probe prints its unique result directory. Read `summary.json`, `events.jsonl`, `config.json`, `stderr.log`, and `status.json` there. The binary uses host-specific CPU instructions. The source WAV is withheld from inference until its scheduled chunk deadlines. A 200 ms transport chunk does not mean 200 ms recognition latency: the tested model decoding step is 2 seconds and commits can require more context.

## Small FLEURS/reference validation

The project-local `.venv-reference` is a Python 3.12 environment with CPU-only PyTorch, the pinned official Qwen3-ASR source (see `third_party/revisions.lock`), `pyarrow`, `soundfile`, and `jiwer`. Python is used only for data preparation/reference validation, not the measured native core. From the repository root (setup needs `uv` and network access):

```bash
bash tools/reference/setup_reference.sh
.venv-reference/bin/python tools/datasets/prepare_fleurs.py
.venv-reference/bin/python tools/reference/qwen_reference.py --cohort acceptance --output results-reference/fleurs_acceptance_reference.jsonl
.venv-reference/bin/python tools/reference/compare_native.py --reference results-reference/fleurs_acceptance_reference.jsonl --output results-reference/fleurs_acceptance_compare.jsonl
python3 tools/reference/check_gate.py results-reference/fleurs_acceptance_compare.jsonl
.venv-reference/bin/python tools/reference/validate_live.py --output results-reference/my_acceptance_live.jsonl
python3 tools/datasets/make_edge_fixtures.py
python3 research/native_qwen/run_lifecycle.py
.venv-reference/bin/python -m unittest discover -s tests/unit -p scoring_test.py
```

Data preparation acquires only the pinned English, Mandarin, and Indonesian validation shards; it writes five exploratory and five held-out acceptance recordings per language. Keep the reference, model, and prepared data files local; see [attribution and licenses](THIRD_PARTY_NOTICES.md). The [offline parity gate](docs/decisions/0002-m0-parity-gate.md) and [paced-live gate](docs/decisions/0003-m0-live-gate.md) passed on the 15 held-out clips, but that cohort is too small for final accuracy claims. Use a fresh `--output` path for each rerun because validation artifacts are created without overwriting prior evidence. A diagnostic streamed run can add `--language Indonesian --refine-final` to compare provisional stream text with a full-audio EOF result; the two timings are reported separately.

`dev-mock` builds the model-free mock CLI, seven fast tests, and a separate 60-second timing gate. `release-cpu` adds the native worker and a process-contract test; native inference needs OpenBLAS and the verified model, while offline scoring uses Python. The native probe remains a separate M0 diagnostic. Model files, vendor checkouts, raw data, and run artifacts are ignored by Git. The environment provides a protected `.git` directory, so no repository initialization or commit was attempted.

## Measured experiments (M4)

See [the M4 code and artifact guide](docs/m4-code.md) for modular timing, resource sampling, Unicode WER/CER, cohort preparation, and metric limitations.

```bash
.venv-reference/bin/python tools/datasets/prepare_m4_fleurs.py
.venv-reference/bin/python tests/unit/evaluation_test.py
.venv-reference/bin/python tests/unit/m4_workflow_test.py
.venv-reference/bin/python tools/evaluation/run_measured.py --per-language 3 --output results/my_m4_run
.venv-reference/bin/python tools/evaluation/audit_measured.py results/my_m4_run
```

## Architecture and implementation plan

Read the planning documents in this order:

1. [Implementation plan](docs/planning/IMPLEMENTATION_PLAN.md) — milestones, dependencies, acceptance gates, and risks.
2. [Architecture and contracts](docs/planning/ARCHITECTURE_AND_CONTRACTS.md) — module boundaries, interchangeable components, ownership, and streaming protocol.
3. [Configuration and benchmarks](docs/planning/CONFIGURATION_AND_BENCHMARKS.md) — configuration design, FLEURS preparation, metric definitions, and fair comparisons.
4. [Repository structure](docs/planning/REPOSITORY_STRUCTURE.md) — proposed folders, files, CMake targets, and creation order.
5. [Requirements and research](docs/planning/REQUIREMENTS_AND_RESEARCH.md) — source review, requirement coverage, runtime candidates, and evidence gaps.

The next milestone is M5 isolated sessions and workers. Active compute RTF remains explicitly unavailable because the native API does not expose active-decode boundaries. General no-speech detection, optimal process/thread/chunk settings, and production capacity remain experimental decisions.
