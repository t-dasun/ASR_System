# Tests and experiment results

## Combined regression command

```bash
python3 scripts/regression.py
python3 scripts/regression.py --full
```

The default runs setup/build for both CMake presets, builds the dashboard, then runs native-build CTest (19), mock-build CTest (15), Python checks (17, including JiWER parity), frontend unit tests (4), and the mock browser workflow. It bootstraps `.venv-test` with JiWER 4.0.0 when needed. These fast checks do not measure model quality.

`--full` additionally prepares dataset inputs, runs the 60-second audio pacing gate, a native three-language browser demo, the four-distinct-WAV shared-pool/UI check, and the four-layout matrix. Its default ten WAVs per language means 120 direct matrix calls plus four matrix network calls, four pool-service calls and three native browser calls. Use `--per-language 2` for a shorter full workflow; this verifies integration rather than quality/capacity.

```bash
# Existing builds/data and an existing Python with JiWER; no setup/downloads.
python3 scripts/regression.py --full --skip-setup \
  --python .venv-reference/bin/python --per-language 2
# Build from existing assets/packages, then test with an existing Python.
python3 scripts/regression.py --skip-downloads --python .venv-reference/bin/python
```

`--skip-setup` needs both CMake builds, the frontend packages, and a Python with JiWER (default `.venv-test/bin/python`, or `--python PATH`). `--skip-downloads` also suppresses test-environment package installation; supply a prepared environment. Browser workflows own ports 5173/4173, so stop other dashboard instances first.

Output goes to a new `results/regression_TIMESTAMP` directory, or `--output PATH`. `regression.json` records every step, command, exit code and duration; individual `.log` files retain output. Full-model bundles are nested beneath that directory. Any failed check stops the runner and returns a nonzero exit code. A matrix with call failures also fails the aggregate regression, even when its report was generated successfully; inspect the language/failure tables.

## Individual regression checks

```bash
cmake --preset release-cpu
cmake --build --preset release-cpu
ctest --preset release-cpu
python3 -m unittest discover -s tests/python -p '*_test.py' -v
cd frontend
npm run build
npm test
npm run test:e2e
```

C++: 19 tests covering audio, scheduling, worker/session lifecycles, prefix queues/pool IPC, metrics, load/sweep, REST/WebSocket, CLI configuration, manifests, and artifact contracts. They use mocks/stub workers and do not establish real-model accuracy. `dev-mock` is an alternative build without Qwen/OpenBLAS. The optional `build/release-cpu/audio-realtime-60-test` checks one minute of wall-clock PCM pacing; it is separate from the fast CTest suite.

Python: 17 checks for input selection/checksums, acquisition pins, Unicode scoring, failed/empty transcripts, descriptive timing distributions, verified model reuse and safe cleanup boundaries. The JiWER parity test skips unless `jiwer==4.0.0` is installed. The existing local `.venv-reference` can run it; Python inference dependencies are unnecessary for the matrix driver.

Frontend: seven unit tests, TypeScript/build checks, and browser workflows. `test:e2e` starts its own mock service and Vite. It needs Chrome (default `/usr/bin/google-chrome`, override `ASR_CHROME`). Do not run another Vite on port 5173 during that test.

## Real-model pool experiment

Prepare the [model](../models/README.md) and [FLEURS WAV inputs](../datasets/README.md), then:

```bash
python3 tools/testing/run_shared_pool_matrix.py \
  --output results/shared-pool --per-language 10
```

The output directory must be new. Python starts `asr-cli serve` for each layout, posts C++ load jobs, reads saved artifacts, and scores their transcripts. Language groups are reported separately; English, Indonesian, and Mandarin are not averaged into one latency/accuracy table.

| Case in `tools/testing/run_shared_pool_matrix.py::CASES` | Workers | Slots/worker | Concurrency |
|---|---:|---:|---:|
| `shared_1w_1s` | 1 | 1 | 1 |
| `shared_1w_2s` | 1 | 2 | 2 |
| `shared_2w_1s` | 2 | 1 | 2 |
| `shared_2w_2s` | 2 | 2 | 4 |

At ten files per language: 30 unique WAVs replayed over four layouts = 120 direct calls, plus four mixed-language WebSocket calls on the two-worker/two-slot layout. Every layout uses the same selected recordings. `--per-language 2` is a short integration check (24 direct + four network calls), not a capacity or accuracy study. `--per-language` accepts 2–50.

These four cases compare the shared-prefix runtime. They do not include the native runtime: use `asr-cli load --config configs/qwen_native_single.yaml` with the same manifest for an additional native comparison. One shared worker with one slot is the shared-runtime control.

Failed calls stay in the report. Completed-call timing percentiles are labelled with their population; accuracy aggregates include empty hypotheses for failures. Transcript consistency across layouts is checked. A successful script execution does not by itself mean the model met an accuracy/SLO threshold; inspect status, failures and language tables.

Regenerate the tables from an existing run without inference:

```bash
python3 tools/testing/run_shared_pool_matrix.py \
  --output results/shared-pool --report-only
```

## CPU/RAM capacity curves

```bash
python3 tools/testing/run_shared_pool_matrix.py --stress \
  --output results/capacity --per-language 10 --repetitions 3 \
  --max-workers 8 --soak-seconds 900
```

Stress mode holds eight session slots per worker and samples total concurrency at one call and 1/2/4/8 calls per worker, deduplicating points. It tests worker counts 1–8 and measures a loaded idle baseline at zero calls. The simulator supports 64 concurrent calls. Add `--dense` to measure every integer concurrency; this considerably increases runtime. `--plan-only` writes the input identities, point grid and call counts without starting models.

The default scaling grid up to eight workers has 39 concurrency/layout points. Ten distinct WAVs per EN/ID/ZH and three repetitions require at most 6,840 direct curve calls. At concurrency above ten, a whole number of ten-file cohorts is repeated in each phase, keeping equal file weighting. Each worker count also gets a maximum-concurrency mixed-language WebSocket suite. `--soak-seconds 900` additionally sustains maximum configured occupancy for at least 15 minutes on the largest successfully measured worker layout.

Services are monitored during context loading, idle and active work. The guard reserves 2 GiB of available RAM and 2 GiB of free swap. It also stops escalation when PSI full avg10 stays at least 10% for 15 seconds together with more than 64 MiB of swap I/O in the last minute. Host swap and per-process swapped bytes/major faults are recorded separately. Model load is recorded separately from warm calls. Conservative cold CLI estimates are retained; the warm suite memory gate remains enforced. Host swap without memory stalls is recorded without stopping the run. Actual resource limits may stop the sweep before eight workers. All call failures stay in the curve; no latency acceptance target is imposed. Resource aborts and untouched higher layouts are labelled separately from model failures.

Output: `report.md`, `curve.csv`, `curve.json`, raw `jobs.json`, per-worker host/swap/process-tree telemetry, runtime snapshots, C++ suite/call artifacts, model/binary/input identities and final checksums. Language tables are separate, including mixed-network subgroups. Curves report achieved occupancy, CPU, RSS/PSS, available RAM, failures, WER/CER, first text, finalization, queue/decode stages and throughput. Stress tables and accuracy plots now use completed-call WER/CER as primary quality, with failures separate and supplementary failure-inclusive scores retained. Maximum observed throughput is not a claim of maximum usable production capacity. The current partial study and its known gaps are summarized in [FINAL_REPORT.md](FINAL_REPORT.md).

To export standalone plots after a run:

```bash
python3 scripts/setup.py --reports
.venv-report/bin/python tools/testing/plot_capacity.py results/capacity
```

Matplotlib is isolated in `.venv-report`; it is not an inference dependency. Figures are PNG/PDF per language and are linked from the report. `--stress --report-only --output PATH` regenerates stress tables from stored artifacts.

## Service and UI verification

With the dashboard running (`cd frontend && npm run dev`) in another terminal:

```bash
python3 tools/testing/verify_pool_service.py \
  --output results/pool-service-check
```

This starts a real two-worker/two-slot C++ service, verifies dashboard controls, uses four distinct manifest WAVs through C++ WebSockets, checks pre-EOF text, and checks calls retire to idle. It also verifies manifest input works when the fallback WAV path is missing.

`frontend/tests/dashboard.native.e2e.mjs` separately exercises live browser calls with the native runtime. `dashboard.pool.e2e.mjs` is a read-only pool UI check against an existing service; the verification driver supplies its origin/output path.

## Where generated results go

All normal run outputs are under the configured `output.directory` (defaults to `results/`). CLI runs produce unique directories. Load/sweep outputs contain suite and per-call directories. The matrix driver writes its own bundle under `--output`:

- `plan.json`, `command.json`, capabilities and input hashes describe what ran.
- `jobs.json`, `status.json`, comparison JSON and Markdown hold the suite/report data.
- Per-call `summary.json`, `events.jsonl`, `audio_timing.jsonl`, `runtime_timing.jsonl` retain transcripts and timing boundaries.
- `system_metrics.jsonl`, worker/call/error files expose resources, routing and failures where collected.
- `checksums.json` seals the matrix files after a completed run.

`results/` is ignored except its README. Historical result bundles and old report Markdown were cleared on `dev-clean`; input manifests, fixtures, model acquisition metadata, and dependency pins remain because they are needed to run the project.

## Read timing fields correctly

| Field | Boundary |
|---|---|
| `startup_ns` | Session request to ready |
| `first_usable_transcript_ns` | Stream start to first nonempty partial **or final** |
| `first_partial_ns` | Stream start to first nonempty partial |
| `final_result_ns` | Stream start to final publication |
| `finalization_ns` | Worker EOF receipt to final publication; controller EOF request fallback |
| `scheduled_final_lag_ns` | Final publication minus scheduled audio end |
| `prefix_decode_queue_wait_ns` / `eof_decode_queue_wait_ns` | Ready job to worker decoding |
| `offline_decode_wall_rtf` | Prefix + EOF invocation wall time / unique WAV duration |
| `effective_rtf` | Stream start to final publication / WAV duration, including pacing |

EOF delay is **not** first-text-to-last-text time. Server publication timing excludes browser rendering; browser tests measure their own arrival boundaries. Active inference compute RTF and stable-prefix latency remain unavailable when the runtime cannot expose them; nulls must not be treated as zeros. Small-sample p95 uses type-7 interpolation and is descriptive.

## Shared-runtime live controls check

```bash
cd frontend
ASR_DEMO_EVIDENCE_DIR=../results/ui-controls-new npm run test:e2e:shared
```

This starts its own two-worker C++ shared service and browser preview on port 4173, sends three short language recordings using 100 ms chunks and a 2 s preview, and checks partial sample counts, arrival delays and reference-based WER/CER. Save evidence in a new directory; stop other preview instances on port 4173 first. Known timeout recordings are excluded from this demonstration selection, so this is integration evidence rather than a replacement quality benchmark.
