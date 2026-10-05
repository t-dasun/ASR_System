# Experimental Qwen call time multiplexing

## Implementation

The fully matched three-layout comparison is now available in [MATCHED_TOPOLOGY_COMPARISON.md](MATCHED_TOPOLOGY_COMPARISON.md): native one worker/one session, shared one worker/two sessions, native two workers/one session each, plus a shared single-session control. All layouts used the same three EN/ID/ZH WAVs twice, with 24/24 completed calls and identical finals. The report links the full startup/first/final/EOF/queue/decode/publication/CPU/memory/accuracy matrices and CSV exports.

The `time-multiplexing` branch adds a shared [`PrefixMultiplexEngine`](../engines/prefix/src/prefix_engine.cpp) to the **main C++ application**. Select `model.runtime: qwen_prefix` using [`configs/qwen_prefix_shared.yaml`](../configs/qwen_prefix_shared.yaml). The main `asr-cli run`, `load`, `sweep`, and `serve` commands select it through the same engine factory. `serve` exposes the existing REST, audio WebSocket, and observation routes. Python programs in `tools/multiplexing/` are verification clients; Qwen inference, PCM ingestion, call scheduling, event delivery, and the load/sweep runner are C++.

The engine loads **one** pinned native Qwen3-ASR-0.6B model context and admits a configurable 1–8 active calls. Each call has its own ordered PCM buffer, identity, events, EOF, and cancellation. A single inference thread schedules calls round robin. It decodes each call's first 4 seconds as a provisional preview and its complete audio at EOF. Native decoding is serial; these calls share model memory and take turns in inference. This is causal **prefix redecoding**, not a resumable native stream state, parallel inference, or batching.

The primary command is `asr-cli serve --config configs/qwen_prefix_shared.yaml --port 8767`. The per-call PCM bound is 60 seconds. The prefix runtime owns multiple sessions directly, while the `qwen_native` runtime continues using the existing one-session `SessionManager` and child worker. `/v1/capabilities` reports the configured call slots, engine, and cancellation/isolation limits. `/v1/runtime` lists the active call IDs, buffered samples, inference activity, queued decode jobs, and process ID. REST benchmark jobs reuse the loaded prefix engine under the existing exclusive service admission gate. Runtime/model/thread/slot/preview/deadline changes require restarting the service; CLI sweeps construct a fresh engine for each case. The `serve-prefix` shortcut and standalone `asr-prefix-server` remain convenience launchers.

Draining closes new-call admission. Idle and total call deadlines are checked at scheduling boundaries, and decode time limits are checked when native decode returns. These are **soft deadlines**: an active native call cannot be interrupted safely. The shared engine executes in the service process and has no hard decode watchdog or crash isolation; these limits are explicit in capabilities. A process-wide owner guard refuses a second simultaneously loaded prefix engine because the pinned runtime uses mutable globals.

## Main C++ 20-call screen

This repeats the earlier experiment's **20 calls per level** with the 1.2-second English fixture, paced PCM, one shared model per level, and the main C++ CLI sweep. **Both Qwen's custom pool and OpenMP/OpenBLAS are limited to four compute threads**, and the BF16 conversion cache is disabled to match the baseline worker policy. The engine sets OpenMP on the actual inference thread: this host's OpenBLAS OpenMP build can overwrite its own thread setter from OpenMP's maximum ([upstream issue](https://github.com/OpenMathLib/OpenBLAS/issues/5806)). The corrected process CPU samples averaged 1.19, 2.16, 3.32, and 3.32 core equivalents at the four levels, rather than the much larger values seen before the correction. Short sampled peaks around 4.3 include timing/tick granularity and service overhead.

The model is kept loaded across calls within each level. There are no warm-up calls; the first call and retained runtime scratch are included in the measured suite. All **80 calls** completed, used `prefix_shared_0`, and produced exactly the same final string as the single-call reference. First text equals final text here: the fixture is shorter than the 4-second preview threshold, so this screen evaluates EOF throughput and contention rather than live preview timing.

| Concurrency | Completed | First/final mean | First/final p50 | First/final p95 | First/final p99 | Audio seconds / wall second | Peak RSS |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 20/20 | 1.719 s | 1.706 s | 1.769 s | 1.835 s | 0.698 | 2.59 GiB |
| 2 | 20/20 | 1.734 s | 1.702 s | 1.779 s | 2.159 s | 1.363 | 2.59 GiB |
| 4 | 20/20 | 2.113 s | 2.019 s | 2.795 s | 3.174 s | 2.118 | 2.59 GiB |
| 8 | 20/20 | 3.833 s | 4.026 s | 4.821 s | 5.214 s | 2.115 | 2.59 GiB |

Moving from four to eight calls gave essentially unchanged aggregate throughput while p95 latency rose from 2.80 s to 4.82 s. Memory stayed near one model plus shared scratch rather than duplicating a model for every active call. These are finite, single-host screens with an unchanged repeated fixture; no SLO profile or sustained safe capacity is asserted. [Sealed main C++ screen with verified budgets, CPU readings, source hashes, and transcript parity](../tools/multiplexing/main_cpp_openmp4_screen_20261005.json). The [earlier main-CLI screen](../tools/multiplexing/main_cpp_short_screen_20261005.json) used OpenMP defaults and is retained as historical evidence; it is not the current four-thread result.

The historical process-isolated 20-call screen reported throughput 0.467/0.770 audio seconds per wall second and peak RSS 2.59/5.17 GiB at one/two workers. The corrected shared runtime measured 0.698/1.363 and 2.59/2.59 GiB at one/two calls. This is a comparison of historical screens, **not a controlled paired speedup**: the shared engine reuses its loaded model and follows a different decode policy. See Q19 in the [assignment answers](ASSIGNMENT_QUESTIONS_AND_ANSWERS.md) for the original population and metrics.

## Main service and C++ runner verification

The final main `serve` command accepted **three simultaneous English/Indonesian/Mandarin paced sockets**, reported three calls on the shared worker and both compute budgets as four in `/v1/runtime`, and correctly prevented a benchmark suite from starting during live calls. All three emitted useful text before their own EOF and completed with call-scoped events:

| Language | Audio duration | First text | Final |
|---|---:|---:|---:|
| English | 12.58 s | 5.60 s | 14.93 s |
| Indonesian | 15.48 s | 7.00 s | 20.09 s |
| Mandarin | 14.28 s | 4.60 s | 16.85 s |

After they finished, a REST load job ran **four calls through the C++ network runner**, reusing the service's loaded model. A live call during the suite received overload with zero credits. The job completed 4/4 with zero failures, and runtime returned to zero active sessions with the same process ID. The final-budget direct eight-call and REST network four-call screens on the 12.58-second English fixture yielded:

| Main C++ path | Calls | Mean first text | p95 first text | Mean final | p95 final | Sampled peak RSS |
|---|---:|---:|---:|---:|---:|---:|
| `asr-cli load`, direct | 8/8 | 8.89 s | 12.48 s | 23.31 s | 30.66 s | 2.66 GiB |
| `asr-cli serve` → REST job → C++ WebSocket runner | 4/4 | 6.61 s | 7.98 s | 18.49 s | 21.63 s | 2.67 GiB |

The eight-call direct run delivered pre-EOF text for **7/8** calls, with one first-text event at 12.83 s after the 12.58 s audio ended. Every final matched the same per-language final string. Therefore the final thread correction improves contention but does not establish an eight-call admission limit satisfying pre-EOF text for this fixture. The four-call network run delivered pre-EOF previews to all four calls. These levels are separate finite screens, not a controlled direct/network latency comparison.

Evidence: [final-budget direct eight-call run and event checks](../tools/multiplexing/main_cpp_openmp4_long_n8_20261005.json), [final main service/runtime/thread/gate/network job](../tools/multiplexing/main_cpp_openmp4_service_verification_20261005.json). Earlier [direct four-call](../tools/multiplexing/main_cpp_direct_n4_20261005.json) and [two-language service](../tools/multiplexing/main_cpp_service_verification_20261005.json) records are retained with OpenMP defaults. A separate 500 ms total deadline screen deliberately failed the call with `deadline_expired`, persisted the failed result, and returned a failing CLI exit status: [deadline record](../tools/multiplexing/main_cpp_deadline_20261005.json). After integration, the native CPU build passed **16/16 CTest cases**, and the dashboard passed its **4/4 unit tests** and production build.

## Earlier mixed-language screen, before OpenMP budget enforcement

Host: Ryzen 9 9955HX, 16 physical cores / 32 logical CPUs, CPU-only native Qwen runtime. **This initial screen used four Qwen custom threads and OpenBLAS/OpenMP defaults; its CPU/latency values are historical rather than the final configured-thread result.** One process was kept warm while levels were tested in order 1, 2, 4, 8. Calls used paced 200 ms PCM chunks, with English 12.58 s and Indonesian 15.48 s FLEURS prepared clips. The same clips were repeated at higher levels. Each level is **one run**, so the table is diagnostic rather than a p95 or sustainable-capacity result. CPU seconds and RSS came from `/proc` at 200 ms intervals. `First text` includes a late preview after EOF if one occurred.

| Active calls | Complete | Text before own EOF | Exact final match to two-call reference | Mean first text | Mean final | Last final | Mean process CPU cores | Peak process RSS |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 EN | 1/1 | 1/1 | n/a | 5.40 s | 15.75 s | 15.75 s | 5.00 | 2.65 GiB |
| 2 EN/ID | 2/2 | 2/2 | reference | 5.70 s | 17.21 s | 19.08 s | 7.18 | 2.67 GiB |
| 4 alternating | 4/4 | 4/4 | 4/4 | 7.60 s | 20.98 s | 26.31 s | 11.25 | 2.68 GiB |
| 8 alternating | 8/8 | 6/8 | 8/8 | 10.51 s | 31.73 s | 44.93 s | 14.36 | 2.68 GiB |

At eight calls, English `call_4` first published text 3 ms **after** its 12.58 s EOF and Indonesian `call_7` first published at 15.84 s, after its 15.48 s EOF. Those two fail the pre-EOF preview condition even though all finals completed and matched the reference. The current scheduling and host therefore do **not** support a claim of eight live calls with pre-EOF text for this fixture mix. Four passed this one screen, but four is not a validated safe admission number. Peak RSS stayed near 2.68 GiB after warm-up; this does not establish memory stability over many calls or model-sharing savings relative to a controlled multi-process run. The initial server RSS sampled at the start of the one-call run was 1.71 GiB.

| Additional check | Observation |
|---|---|
| Configured admission | Eight idle active sessions accepted; ninth received `resource_exhausted` and zero credits. |
| Cancellation isolation | One call sent cancel at 4.00 s and received a `stopped/cancelled` event 1.13 s later; the other paced call completed normally at 19.46 s. Cancellation waits for any active native decode and is not instantaneous. |
| Two-call transport | Separate English/Indonesian sockets shared worker ID `prefix_shared_0`; both got pre-EOF text and call-scoped final events. |
| Native call-order check | Preview and final strings were invariant when the English/Indonesian research probe order was reversed. |

Raw records: [N=1](../tools/multiplexing/prefix_load_n1_20261005.json), [N=2](../tools/multiplexing/prefix_load_n2_20261005.json), [N=4](../tools/multiplexing/prefix_load_n4_20261005.json), [N=8](../tools/multiplexing/prefix_load_n8_20261005.json), [admission](../tools/multiplexing/prefix_admission_n8_20261005.json), [cancellation](../tools/multiplexing/prefix_cancel_survivor_20261005.json), and [two-call transport](../tools/multiplexing/prefix_websocket_two_calls_admission_20261005.json). Reproduction commands and the pinned source/model details are in the [experiment README](../tools/multiplexing/README.md).

The same paced two-call test was also run through **`asr-cli serve-prefix`**, not only the standalone executable. Both calls received pre-EOF text (English 6.80 s before 12.58 s EOF; Indonesian 5.40 s before 15.48 s EOF), completed, and shared the same model worker ID. [Main-CLI raw result](../tools/multiplexing/asr_cli_serve_prefix_two_calls_20261005.json).

## Design and sizing decision

`MAX_CALLS` is an **admission ceiling**, not a capacity rating. More accepted calls increase waiting for the single native inference thread. The one-preview policy limits repeated work but also gives no later pre-EOF revisions. A future `C_safe` must be the largest admitted call count that meets a declared first-text/final latency, accuracy, error, memory, and CPU envelope for a sustained representative mix. Then a conditional fleet calculation is `workers = ceil(simultaneous_legs / C_safe)` and `nodes = ceil(workers / measured_safe_workers_per_node)`, with failure-domain reserve added after measurement. Neither `C_safe` nor safe workers per node is established by these short screens. The historical process-per-call topology and the new shared-model memory/queue measurements must be sized separately; the main service can now select either runtime.

For a production replacement, the shared engine still needs process isolation and crash recovery, a long-lived multi-call worker protocol, interruption or watchdog during blocking decode, sustained mixed-duration testing, held-out accuracy and transcript stability, and target-host saturation/headroom measurements. PCM buffers and active admission are bounded, and the WebSocket protocol retains one-credit backpressure. A resumable per-call decoder or a different online ASR runtime is needed if serial prefix redecoding cannot meet the target first-text deadline.
