# CLI quick start

Run commands from the repository root. The application executable is `build/release-cpu/asr-cli`. It starts `asr-prefix-worker` automatically for multiple shared workers; you do not launch that worker executable yourself.

## Build

```bash
cmake --preset release-cpu
cmake --build --preset release-cpu
```

## Normal simulation: direct C++ ingress

```bash
build/release-cpu/asr-cli load \
  --config configs/qwen_prefix_shared.yaml \
  --set workers.processes=1 \
  --set workers.max_sessions_per_process=2 \
  --set output.directory=../results/demo_direct \
  --calls 20 --concurrency 2 --languages en --mode direct
```

This repeats the YAML's English WAV for 20 calls, keeping up to two calls active on one shared model. Audio arrives as paced 200 ms chunks. The command exits after writing the results.

## Network simulation: C++ WebSocket ingress

```bash
build/release-cpu/asr-cli load \
  --config configs/qwen_prefix_shared.yaml \
  --set workers.processes=1 \
  --set workers.max_sessions_per_process=2 \
  --set output.directory=../results/demo_network \
  --calls 20 --concurrency 2 --languages en --mode network
```

The CLI automatically starts its loopback WebSocket server and C++ clients, runs the same chunk simulation, saves results and shuts down. No browser, Python process or separately started `serve` command is required. Network mode includes WebSocket transport overhead; direct mode calls the engine interface directly.

## Select workers and calls per worker

| Desired layout | `workers.processes` | `workers.max_sessions_per_process` | `--concurrency` |
|---|---:|---:|---:|
| One worker, one active call | 1 | 1 | 1 |
| One worker, two active calls | 1 | 2 | 2 |
| Two workers, one active call each | 2 | 1 | 2 |
| Two workers, two active calls each | 2 | 2 | 4 |

`--calls` is total calls for each repetition. `--concurrency` is total overlapping calls. Slots per worker are an admission ceiling; the least-active scheduler assigns calls dynamically. Total calls processed by each worker need not be equal. Each worker decodes one job at a time, while separate workers can decode independently.

For two workers with two active calls each, change the examples to `workers.processes=2`, `workers.max_sessions_per_process=2`, and `--concurrency 4`. The runtime supports 1–4 workers and 1–8 slots per worker; the load runner allows at most 16 concurrent calls. Cold-start memory preflight can reject two workers when less than approximately 8.1 GiB is available. Use `load-dry-run` in place of `load` to inspect the plan before running.

## Use different WAVs

Add this option to either load command:

```bash
--manifest datasets/manifests/multiplex_mixed_smoke.jsonl
```

For all four distinct recordings, set `--calls 4 --languages en,id,zh`. Each record supplies its own WAV and language. Calls cycle through eligible records when the requested count exceeds the number of recordings. Without a manifest, all calls repeat the YAML WAV. A manifest does not require the YAML fallback WAV to exist.

To select your own single WAV, add `--set audio.path=/absolute/path/input.wav` and set its language with `--languages en`, `id`, or `zh`. Change chunk size with `--set audio.chunk_ms=100`. Prefix calls are limited to 60 seconds of input each.

## Find results

The examples write to `results/demo_direct/load_<id>/` or `results/demo_network/load_<id>/`. Relative configuration paths, including output overrides, resolve from the YAML directory; hence `../results/...` above. The CLI prints the full suite directory in its final JSON.

```text
load_<id>/
  plan.json                         configuration and call assignments
  summary.json                      completion, latency, throughput and memory
  status.json                       COMPLETE / FAILED
  phase_0_system_metrics.jsonl       CPU/RSS/PSS samples
  <individual_call_id>/
    summary.json                    transcript and all call timings
    events.jsonl                    partial/final/failure events
    audio_timing.jsonl              chunk deadlines, send/submit delays
    runtime_timing.jsonl            model, decode and scheduler queue timings
    errors.jsonl                    failure details
```

Without an output override, the shared YAML writes under `results/time_multiplexing/main_cpp/`. Existing measured tables and all evidence links are in [SHARED_WORKER_POOL_RESULTS.md](SHARED_WORKER_POOL_RESULTS.md).

## C++ tests versus Python experiments

```bash
ctest --preset release-cpu
```

The 19 C++ unit/integration tests use mocks and worker stubs for deterministic contracts; they do not run the full real-model corpus. `asr-cli load` itself is a real Qwen C++ benchmark in both direct and network modes.

For the complete real-model corpus experiment with separate language accuracy tables:

```bash
python3 tools/multiplexing/run_shared_pool_matrix.py \
  --output results/time_multiplexing/my_matrix --per-language 10
```

Python starts the main C++ services, submits C++ benchmark jobs, checks artifacts, computes WER/CER from references and writes the report. WAV preparation, chunk pacing, worker routing, model inference and runtime measurements are C++. See [the full architecture/run guide](SHARED_WORKER_POOL.md) for service/UI operation and limitations.
