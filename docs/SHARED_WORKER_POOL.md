# Multi-call, multi-worker WAV simulation

## Implemented architecture

The main C++ `asr-cli` now supports one endpoint with multiple shared-model workers. The same `IASREngine` interface is used by CLI runs, benchmark suites, REST jobs and live WebSocket calls.

| Configuration | Placement at the stated concurrency |
|---|---|
| 1 worker, 2 slots/worker, concurrency 2 | Both calls share one model; decode jobs run serially |
| 2 workers, 1 slot/worker, concurrency 2 | One call/model per worker; workers decode independently |
| 2 workers, 2 slots/worker, concurrency 2 | One call per worker; the extra slots are available |
| 2 workers, 2 slots/worker, concurrency 4 | Two calls share each worker; two independent model decoders |

With one worker, the existing prefix engine runs inside the CLI/service process. With two to four workers, `PrefixProcessPool` starts an `asr-prefix-worker` process per worker. Each child loads one Qwen context and reuses it across calls. A bounded Unix socket protocol carries audio, commands, acknowledgements, call-scoped results and runtime observations. EOF waits run separately from ingress so another session can keep receiving chunks during refinement. Completed wait threads are reaped.

Qwen still uses the pinned C CPU runtime. Calls have independent audio, identity, language, event sequence and EOF state. Workers do not share mutable decoder state. Each worker has one active native inference invocation; workers can run in parallel. This is prefix redecoding, not resumable per-call KV-cache decoding or inference batching.

## Scheduling

1. **Worker admission:** `least_active` selects the healthy worker with the fewest reserved sessions. Rotating ties spread calls fairly. Thus two available workers receive one call each before either receives a second. `round_robin` is also supported for worker routing.
2. **Within each worker:** select the oldest ready preview before EOF refinement. At most two previews may bypass a waiting EOF job; then the oldest EOF job runs. Expired calls are retired first.
3. **During a decode:** the pinned native invocation is blocking. A newly ready preview waits until that invocation returns. Calls stay assigned to their worker for their lifetime.

The previous matched results showed that sharing two sessions added approximately 0.52 s preview queue wait and 1.26 s EOF queue wait. That supports spreading calls before sharing and preserving first-text priority with bounded finalization fairness. The new policy is tested separately from the historical round-robin measurements; historical results have not been relabelled as measurements of this scheduler.

## Build

Run from the repository root:

```bash
cmake --preset release-cpu
cmake --build --preset release-cpu
```

## Simulate chunks from one WAV

Two workers, two allowed sessions per worker, four active calls:

```bash
build/release-cpu/asr-cli load \
  --config configs/qwen_prefix_shared.yaml \
  --set workers.processes=2 \
  --set workers.max_sessions_per_process=2 \
  --calls 20 --concurrency 4 --languages en --mode direct
```

This command repeats the configured YAML WAV. `--calls` is total calls, `--concurrency` is overlapping calls, and `max_sessions_per_process` is each worker's admission ceiling. It uses 200 ms audio chunks delivered at their availability deadlines. Use `--set audio.chunk_ms=100` to change chunk size. `--mode network` uses C++ loopback WebSocket ingress with the same worker pool.

Cold load preflight reserves 2 GiB plus 3 GiB per shared worker, 16 MiB per concurrent call, and prepared manifest PCM. Two workers therefore need approximately 8.1 GiB available for the example. The check may reject a run on this host when other applications reduce available RAM. Four-worker inference has not been measured here.

## Different WAVs for different calls

A JSONL manifest accepts one object per line:

```json
{"id":"english_01","file":"/absolute/path/english.wav","language":"en","reference":"Human reference text"}
{"id":"mandarin_01","file":"/absolute/path/mandarin.wav","language":"zh","reference":"人工转写文本"}
```

`id`, `file` and `language` are required. Optional `sha256` and `num_samples` are checked. References are retained for offline accuracy evaluation. Relative files resolve from the current directory when present, otherwise from the manifest directory. WAVs are prepared with the existing C++ resampler and mono-channel validation. There are bounds of 200 recordings and 256 MiB prepared PCM; prefix calls remain bounded to 60 seconds each.

```bash
build/release-cpu/asr-cli load \
  --config configs/qwen_prefix_shared.yaml \
  --manifest datasets/manifests/multiplex_mixed_smoke.jsonl \
  --set workers.processes=2 \
  --set workers.max_sessions_per_process=2 \
  --concurrency 4 --mode direct
```

Without `--calls`, the CLI offers one call per manifest entry. Calls cycle through eligible entries in manifest order; a selected language comes from that WAV's record. `--languages en` filters entries to English. Set `--calls` to the desired count when filtering: otherwise the default count is the complete manifest size and eligible recordings repeat. `--repetitions` repeats the corpus; repeats do not add independent accuracy coverage. Every call artifact stores its selected recording ID, actual WAV path, SHA256, prepared sample count, reference, timings and transcript.

The same manifest option works with the native runtime (one call per native worker). Existing non-manifest single-WAV behavior is retained.

## Service and UI

```bash
build/release-cpu/asr-cli serve \
  --config configs/qwen_prefix_shared.yaml \
  --manifest datasets/manifests/multiplex_mixed_smoke.jsonl \
  --set workers.processes=2 \
  --set workers.max_sessions_per_process=2 \
  --port 8767
```

Start the dashboard with `npm run dev` inside `frontend`, open `http://127.0.0.1:5173`, and connect to `http://127.0.0.1:8767`.

- **Live calls:** use multiple dashboard tabs, select a WAV/language in each and start streaming. All tabs use the same endpoint; the pool assigns workers.
- **Experiment editor:** Load offers the server's configured manifest entries, filtered by the language selection. Set concurrency to 2 for one call per worker or 4 for two calls per worker. The dataset count is displayed. Direct and network ingress both run C++ suites.
- **Runtime:** each worker row shows its PID, active sessions/capacity, active call IDs and decode queue.
- **Stop:** benchmark Stop prevents remaining calls from starting; active calls finish. Live-stream Stop invokes per-call cancellation, checked between generated tokens.
- **Layout changes:** restart the shared service to change worker count, slots, model, threads or scheduler. REST suites reuse its loaded models. The shared worker-count field is fixed to that service layout.
- **Sweeps:** a manifest-configured service supports Load suites. Use a single-WAV service or CLI sweeps for parameter sweeps. Browser uploads are live-call inputs; REST suites use the server's configured WAV/manifest.

A warmed service is checked against additional memory requirements for its suite: the contexts already occupy RAM and are not counted again. This differs from the cold CLI estimate; warm results report this explicitly. Live calls and benchmark jobs retain the existing exclusive admission gate.

See [the measured results and scheduling decisions](SHARED_WORKER_POOL_RESULTS.md) for the 10-WAV-per-language matrix and final service verification.

## Reproduce per-language results

```bash
python3 tools/multiplexing/run_shared_pool_matrix.py \
  --output results/time_multiplexing/my_shared_pool_matrix \
  --per-language 10
```

This selects 10 unique heldout FLEURS recordings in each of English, Indonesian and Mandarin. Every layout uses the same 30 recordings, with separate language suites. It checks these four layouts: shared 1w/1s, 1w/2s, 2w/1s and 2w/2s (four calls in the last case). A separate four-call mixed-WAV network suite verifies C++ WebSocket ingress through the pool. Python starts services, submits C++ jobs, checks results and scores saved text; all inference, chunk simulation and timing are C++.

The output contains per-language Markdown tables, complete JSON measurements, per-call text/accuracy records, raw suite artifacts, resource samples, runtime occupancy/PIDs, source hashes, binaries, cold plans and commands. Use a new output directory each time. To regenerate tables without inference:

```bash
python3 tools/multiplexing/run_shared_pool_matrix.py \
  --output results/time_multiplexing/my_shared_pool_matrix --report-only
```

## Scope and verification

The pool supports 1–4 configured worker processes and 1–8 sessions per worker. The bounded load runner supports up to 16 concurrent calls. Admission ceilings are not measured safe capacity. First text, final, EOF delay, scheduler queue waits, decode invocation wall time, IPC publication, RTF and resources are recorded; stable-word timing and vendor active-compute time remain unavailable.

A failed/disconnected worker fails its active calls and is excluded from new assignments; surviving workers continue accepting available work. Restart the service to restore full worker count. Automatic respawn and seamless call replay are not implemented. The optional offline decode guard checks cancellation and decode/total deadlines between generated tokens. It aborts generation and rejects the incomplete result. Encoder/prefill and an individual kernel remain blocking, so this is cooperative cancellation rather than a hard wall-clock watchdog. A process RPC deadline terminates an unresponsive worker and can affect every call on it.

The native live-streaming runtime remains available separately. Historical aggregate comparisons and the available data index are in [available results by language](AVAILABLE_RESULTS_BY_LANGUAGE.md).

## Bounded offline decoding extension

A larger corpus exposed an upstream offline decode that generated 2,048 repeated punctuation tokens and occupied a worker for about 171 seconds. The initial failed run is preserved under `results/time_multiplexing/shared_pool_10wav_20261005`; it is not a successful matrix.

`scripts/prepare_qwen_guard.py` derives CPU build sources in the build directory from the unchanged pinned vendor checkout. It adds an optional context guard and abort flag. `PrefixMultiplexEngine` installs a guard for each job, checking cancellation, shutdown and decode/total deadlines between generated tokens. Incomplete text is rejected and a call-scoped failure is published. Unset guards retain the upstream behavior, including the native-live baseline. A regression on the affected WAV with a 1-second decode budget returned a failure after approximately 1.23 seconds of EOF decode, rather than continuing to the 2,048-token limit.

This bounds repeated generation at token boundaries; it does not fix the model's recognition quality. Failed calls stay in failure-inclusive accuracy and completion counts. The corpus reporter retains failed-call timing and excludes missing final timestamps from successful-call latency distributions.

## Main files introduced

- `engines/prefix/include/asr/engines/prefix_pool.hpp` and `engines/prefix/src/prefix_pool.cpp`: persistent worker pool, routing, call ownership, response/event dispatch and failed-worker exclusion.
- `engines/prefix/src/process_worker.cpp` and `process_wire.hpp`: child worker and bounded PCM/control/event IPC.
- `engines/prefix/include/asr/engines/prefix_scheduler.hpp`: oldest-ready preview priority with bounded EOF fairness.
- `scripts/prepare_qwen_guard.py`: optional token-boundary guard in generated CPU build sources.
- `tools/multiplexing/run_shared_pool_matrix.py` and `verify_pool_service.py`: corpus orchestration, scoring, all readings, service verification and evidence sealing; inference remains C++.
- `frontend/tests/dashboard.pool.e2e.mjs`: read-only browser verification of pool/manifest controls.
- New pool/scheduler/manifest tests and fixtures exercise routing, admission, worker failure, cancellation, concurrent ingress and manifest-only loading.

Existing CLI, YAML resolver, load runner, prefix adapter, native build configuration and dashboard panels were extended. Public engine/session interfaces remain the same.
