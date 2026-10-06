# Run the system

Run commands from the repository root.

## Setup, rebuild and cleanup

```bash
python3 scripts/setup.py                         # fetch missing pinned assets, verify model, build CPU executable
python3 scripts/setup.py --frontend --with-data  # also install/build UI and prepare FLEURS WAVs
python3 scripts/setup.py --clean-build           # delete release-cpu build and rebuild
python3 scripts/setup.py --clean-results --clean-only
python3 scripts/setup.py --clean-all-builds --clean-results --clean-only
```

Choose `--preset dev-mock` for a model-free build. `--clean-build` removes only the selected preset (and frontend/dist when `--frontend` is selected). `--clean-all-builds` removes the whole `build/` directory plus `frontend/dist`. Without `--clean-only`, cleanup is followed by setup/build. Results cleanup preserves its README; downloaded models, raw/prepared WAVs, dependency sources and node_modules are retained.

`--skip-downloads` uses only existing local assets/packages. Normal setup checks pinned dependency revisions and model file hashes, downloads missing artifacts and refuses mismatched/unverified files. `--with-data` bootstraps `.venv-data` for the pinned preparation packages; `--frontend` installs the npm lockfile. System packages are prerequisites, described in [dependency setup](../third_party/README.md).

Manual CMake remains available: `cmake --preset release-cpu` and `cmake --build --preset release-cpu`.

## Choose a runtime

| Config | Runtime | Calls per worker |
|---|---|---|
| `configs/mock_baseline.yaml` | Deterministic C++ mock | One |
| `configs/qwen_native_single.yaml` | Qwen native progressive streaming | One |
| `configs/qwen_prefix_shared.yaml` | Shared Qwen context, queued prefix/EOF decoding | 1–8 |

Shared runtime supports 1–8 workers. One worker uses a context inside the service; multiple workers use persistent child processes. Each worker runs one decode job at a time while retaining several active call sessions.

## Direct or network WAV simulation

```bash
# Inspect resolved configuration and admission without producing run results.
build/release-cpu/asr-cli load-dry-run \
  --config configs/qwen_prefix_shared.yaml --calls 4 --concurrency 4 \
  --set workers.processes=2 --set workers.max_sessions_per_process=2

# Four calls, two workers, two session slots per worker; use real WebSocket ingress.
build/release-cpu/asr-cli load \
  --config configs/qwen_prefix_shared.yaml --calls 4 --concurrency 4 \
  --set workers.processes=2 --set workers.max_sessions_per_process=2 \
  --languages en --mode network \
  --set output.directory=../results/two-workers
```

Use `--mode direct` to bypass WebSocket transport. Both modes use the C++ WAV reader, sample pacing, call runner, and selected inference engine. Network mode starts its own internal server and C++ clients; a separately running `serve` is unnecessary.

| Layout | `workers.processes` | `workers.max_sessions_per_process` | `--concurrency` |
|---|---:|---:|---:|
| One worker, one session | 1 | 1 | 1 |
| One worker, two sessions | 1 | 2 | 2 |
| Two workers, one session each | 2 | 1 | 2 |
| Two workers, two sessions each | 2 | 2 | 4 |

`--calls` is total calls over the run; `--concurrency` is simultaneous calls. Twenty calls at concurrency four run in successive admissions; they do not create twenty workers. Concurrency is bounded by the available slots and the runner limit of 64. Four threads per worker means two workers can compete for eight compute threads.

With no manifest, all calls replay the configured WAV. With a manifest, calls can use different WAVs:

```bash
build/release-cpu/asr-cli load \
  --config configs/qwen_prefix_shared.yaml \
  --manifest datasets/manifests/demo_calls.jsonl \
  --calls 4 --concurrency 4 --languages en,id,zh --mode network \
  --set workers.processes=2 --set workers.max_sessions_per_process=2
```

Records contain language, WAV path, sample count, and SHA256. Relative WAV paths resolve against the manifest directory, with repository-root fallback for the retained manifests. Eligible records cycle when calls exceed their count. Set `--calls` explicitly when filtering languages. Manifest mode does not require the fallback `audio.path` to exist.

The default 1.2-second fixture is shorter than the 4-second prefix preview, so its first text arrives at finalization. Use longer prepared recordings for a pre-EOF text demonstration.

## Persistent service and dashboard

```bash
build/release-cpu/asr-cli serve \
  --config configs/qwen_prefix_shared.yaml \
  --manifest datasets/manifests/demo_calls.jsonl \
  --set workers.processes=2 --set workers.max_sessions_per_process=2 \
  --set output.directory=../results/service --port 8080
```

The service prints its listening port once ready. `--port 0` selects a free port. Verify readiness with:

```bash
curl http://127.0.0.1:8080/v1/capabilities
curl http://127.0.0.1:8080/v1/runtime
```

Start the dashboard in another terminal:

```bash
cd frontend
npm ci
npm run dev
```

Open `http://127.0.0.1:5173`, set the service origin to `http://127.0.0.1:8080`, then upload a 16 kHz mono PCM16 WAV for a live call or launch a suite. Runtime status shows worker IDs, process IDs, active sessions, queues, and thread counts. The shared-model worker layout is fixed when the service starts; changing worker count or slots requires restarting it. The UI reflects this restriction.

An external live client connects to `ws://127.0.0.1:8080/v1/asr` and sends paced PCM chunks. Python is unnecessary for live calls. To ask the service to run its C++ simulator:

```bash
curl -X POST http://127.0.0.1:8080/v1/suites \
  -H 'Content-Type: application/json' \
  -d '{"kind":"load","mode":"network","calls":4,"concurrency":4,"languages":["en","id","zh"]}'
# Use the returned job_id:
curl http://127.0.0.1:8080/v1/jobs/JOB_ID
```

The service admits live traffic or an exclusive suite. `network` suites use an internal C++ WebSocket server sharing the loaded engine; their WAV clients run inside the C++ job.

## Other CLI commands and output

`validate` resolves configuration; `dry-run` plans a single call; `run` performs it. `load-dry-run` / `load` plan/run repeated calls. `sweep-dry-run` / `sweep` plan/run configuration cases. `--help` lists suite and SLO arguments. `python3 scripts/doctor.py` prints a read-only host/dependency snapshot.

YAML paths and `--set` path overrides resolve relative to the config file. For files outside the project, use absolute paths. Memory admission can reject a cold multi-worker run; reducing sessions alone does not remove each worker's model copy. Read the preflight reasons before changing workers.

See [Testing](TESTING.md) for the experiment driver and output files, and [Architecture](ARCHITECTURE.md) for the runtime differences.
