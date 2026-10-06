# Shared worker pool results and scheduling decisions

Evidence date: 2026-10-05. [Run and architecture guide](SHARED_WORKER_POOL.md), [full per-language matrix, per-call measurements and 1456 artifact hashes](../tools/multiplexing/shared_worker_pool_comparison_20261005.json), [final C++ service and browser verification](../tools/multiplexing/shared_worker_pool_final_service_20261005.json), [decode-guard regression](../tools/multiplexing/prefix_decode_guard_regression_20261005.json).

## Result summary

The four-layout corpus experiment offered **10 unique WAVs per language per layout**: 30 recordings, 120 calls. **108/120 completed**. The same English recording and two Indonesian recordings hit decode deadlines in every layout. All successful final transcripts matched the single-worker/single-session reference for the same WAV. Mandarin completed 10/10 in every layout. Four additional mixed-language/distinct-WAV C++ network calls completed; the final application build independently repeated that four-call service check successfully.

| Language | Complete in each layout | Failure-inclusive accuracy | Completed-only accuracy |
|---|---:|---:|---:|
| English | 9/10 | WER 16.10% | WER 6.52% |
| Indonesian | 8/10 | WER 24.58% | WER 4.93% |
| Mandarin Chinese | 10/10 | CER 3.25% | CER 3.25% |

Failure-inclusive accuracy scores a failed call with an empty final hypothesis. Completed-only accuracy excludes it; both are shown so failures are visible. WER and CER measure different units and remain separate.

| Failed source recording | Language | Outcome |
|---|---|---|
| `fleurs_en_us_validation_1518_26` | English | Offline generation exceeded the 45 s budget in each layout |
| `fleurs_id_id_validation_1549_131` | Indonesian | Offline generation exceeded the 45 s budget in each layout |
| `fleurs_id_id_validation_1510_256` | Indonesian | Offline generation exceeded the 45 s budget in each layout |

The earlier unguarded English attempt generated 2,048 repeated punctuation tokens and spent about 171 s in EOF decoding. The new optional guard interrupts at generated-token boundaries and rejects incomplete output. A 1 s regression budget stopped after approximately 1.23 s; a fresh-context 5 s test also failed within that budget. This does not repair the model's recognition errors. Worker ingress and later sessions remained available after the guarded call failures.

## Scheduling decision supported by the results

- **Spread before sharing:** least-active routing sends two calls to two available workers before filling a second slot on either. This removes same-worker queue contention for those two calls.
- **First-text priority with finalization fairness:** each worker selects the oldest ready preview before EOF work, with at most two previews bypassing a waiting EOF job. The oldest EOF job then runs.
- **Keep the call assigned to its worker:** audio, callbacks, ordering, cancellation and language stay bound to that call/worker for its lifetime.
- **Bound blocking work:** the guard checks cancellation and decode/total budgets between generated tokens. Encoder/prefill and an individual kernel remain blocking. Priority cannot resume an interrupted decoder context or migrate ongoing decode state.

The new measurements show why both memory and latency matter. Shared one-worker/two-session EN/ID mean EOF delay was **7.87/14.76 s**, compared with **4.43/5.06 s** with two workers/one session each. The two-worker layouts used eight configured compute threads and roughly **5.4–5.5 GiB RSS**, versus four threads and **2.8 GiB RSS** with one worker. Their first-text means were not consistently lower. Allowing two sessions per worker increases throughput, but a long EOF decode can still delay other calls on that worker. Configure admission and decode budgets against the required latency/error envelope; this corpus does not qualify production capacity.

## Final implementation verification

The final service check used two workers, two slots per worker, four distinct WAVs, C++ WebSocket ingress, and an intentionally nonexistent YAML fallback WAV. All four completed with useful text before EOF, exact input sample counts, ordered call-scoped events and zero overflow. Runtime samples reached **[2, 2] active sessions** on two distinct worker PIDs, with both custom Qwen and BLAS budgets set to four per worker. The browser displayed a four-call limit, manifest inputs, fixed worker layout, PID/session/queue rows and a valid C++ dry-run plan with **zero browser errors**.

The final native build passed **19/19 CTest cases**, including scheduler fairness, pool routing/admission, worker-exit isolation, ingress during another call's EOF wait, per-session identity rejection, and a manifest load without a fallback WAV. The frontend production build and **4/4 unit tests** passed. Corpus binary hashes are retained; the final service check records the newer application binary after the manifest-only validation fix. The model, priority scheduler and decoder guard are unchanged between those builds.

Automatic worker respawn, resumable native decoder state, a per-kernel hard watchdog and production capacity qualification remain outside this prototype. Four-worker inference was not measured on this host. Native live-decoding results are separately available in [the earlier matched comparison](MATCHED_TOPOLOGY_COMPARISON.md) and [the available data index](AVAILABLE_RESULTS_BY_LANGUAGE.md).

## Detailed per-language tables

Main C++ `serve`/REST `run_load` performs WAV preparation, chunk pacing, routing and inference. Python orchestrates and scores saved text.

Same 10 unique heldout WAVs per language in each layout. One run per WAV per layout. Native live decoding is not included in this new matrix; all four layouts use the same prefix policy and priority scheduler. One-worker layouts run the model in the service process; two-worker layouts use persistent child processes and IPC.

Services load contexts once before the suites. Admission/startup excludes context loading. Cold CLI preflight plans are saved separately; these are warm service measurements. Four compute threads per worker; two workers have eight configured threads total. No hard OS CPU quota.

## en

| Layout | Complete | Pre-EOF text | First text mean / p95 s | Final mean / p95 s | EOF delay mean s | Error | Peak RSS GiB | Audio s / wall s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| shared_1w_1s | 9/10 | 7/10 | 5.724 / 6.260 | 10.637 / 14.154 | 2.857 | WER 16.10% | 2.79 | 0.459 |
| shared_1w_2s | 9/10 | 8/10 | 5.773 / 7.069 | 15.648 / 40.115 | 7.868 | WER 16.10% | 2.81 | 0.690 |
| shared_2w_1s | 9/10 | 7/10 | 6.623 / 7.538 | 12.207 / 16.333 | 4.426 | WER 16.10% | 5.41 | 0.839 |
| shared_2w_2s | 9/10 | 6/10 | 7.216 / 9.544 | 18.575 / 40.794 | 10.787 | WER 16.10% | 5.42 | 0.948 |

### Stage timings (means in ms for completed calls)

| Stage | shared_1w_1s | shared_1w_2s | shared_2w_1s | shared_2w_2s |
|---|---:|---:|---:|---:|
| startup_ns | 0.004 | 0.003 | 0.184 | 0.133 |
| shared_model_load_ns | 359.623 | 369.642 | 353.309 | 356.212 |
| prefix_decode_wall_ns | 1768.223 | 1634.784 | 2683.032 | 2848.567 |
| prefix_decode_queue_wait_ns | 2.002 | 210.079 | 0.130 | 547.382 |
| eof_refinement_wall_ns | 2749.747 | 2454.418 | 4119.878 | 3738.713 |
| eof_decode_queue_wait_ns | 107.164 | 5413.727 | 305.943 | 7047.654 |
| runtime_queue_wait_ns | 108.944 | 5600.464 | 306.059 | 7534.215 |
| offline_decode_wall_ns | 4321.501 | 3907.559 | 6504.796 | 6270.772 |

Shared-model load is reused context metadata, not a new model load per call. Failed-call decode durations remain in the per-call JSON and the phase CPU/throughput measurements.

| Layout | Configured compute threads | Mean / peak CPU cores | Peak PSS GiB | Effective RTF mean | Offline wall RTF mean |
|---|---:|---:|---:|---:|---:|
| shared_1w_1s | 4 | 2.41 / 4.65 | 2.79 | 1.379 | 0.560 |
| shared_1w_2s | 4 | 3.38 / 4.60 | 2.80 | 1.904 | 0.506 |
| shared_2w_1s | 8 | 5.27 / 8.87 | 3.94 | 1.592 | 0.838 |
| shared_2w_2s | 8 | 5.85 / 9.08 | 3.95 | 2.332 | 0.818 |

### Chunk/event delay p95 (ms)

| Delay | shared_1w_1s | shared_1w_2s | shared_2w_1s | shared_2w_2s |
|---|---:|---:|---:|---:|
| send_lag | 0.469 | 0.286 | 0.215 | 0.259 |
| controller_queue_wait | 0.005 | 0.004 | 0.005 | 0.005 |
| submit_wall | 0.079 | 0.140 | 1.262 | 1.297 |
| publication_delay | 0.071 | 0.080 | 0.602 | 0.618 |

## id

| Layout | Complete | Pre-EOF text | First text mean / p95 s | Final mean / p95 s | EOF delay mean s | Error | Peak RSS GiB | Audio s / wall s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| shared_1w_1s | 8/10 | 10/10 | 5.764 / 6.182 | 13.894 / 20.225 | 3.626 | WER 24.58% | 2.81 | 0.366 |
| shared_1w_2s | 8/10 | 10/10 | 6.033 / 7.145 | 25.031 / 61.186 | 14.763 | WER 24.58% | 2.82 | 0.520 |
| shared_2w_1s | 8/10 | 10/10 | 6.601 / 7.304 | 15.328 / 23.739 | 5.060 | WER 24.58% | 5.51 | 0.666 |
| shared_2w_2s | 8/10 | 7/10 | 8.901 / 10.855 | 22.141 / 44.749 | 11.872 | WER 24.58% | 5.52 | 0.951 |

### Stage timings (means in ms for completed calls)

| Stage | shared_1w_1s | shared_1w_2s | shared_2w_1s | shared_2w_2s |
|---|---:|---:|---:|---:|
| startup_ns | 0.003 | 0.004 | 0.123 | 0.145 |
| shared_model_load_ns | 359.623 | 369.642 | 353.309 | 356.212 |
| prefix_decode_wall_ns | 1763.866 | 1633.836 | 2599.492 | 2851.449 |
| prefix_decode_queue_wait_ns | 0.141 | 399.372 | 0.164 | 2048.395 |
| eof_refinement_wall_ns | 3625.956 | 3316.057 | 5058.807 | 5134.152 |
| eof_decode_queue_wait_ns | 0.126 | 11446.805 | 0.325 | 6737.476 |
| runtime_queue_wait_ns | 0.267 | 11846.177 | 0.489 | 8785.871 |
| offline_decode_wall_ns | 5389.823 | 4949.893 | 7658.298 | 7985.601 |

Shared-model load is reused context metadata, not a new model load per call. Failed-call decode durations remain in the per-call JSON and the phase CPU/throughput measurements.

| Layout | Configured compute threads | Mean / peak CPU cores | Peak PSS GiB | Effective RTF mean | Offline wall RTF mean |
|---|---:|---:|---:|---:|---:|
| shared_1w_1s | 4 | 2.64 / 4.73 | 2.81 | 1.352 | 0.535 |
| shared_1w_2s | 4 | 3.57 / 4.72 | 2.82 | 2.390 | 0.491 |
| shared_2w_1s | 8 | 5.34 / 8.86 | 4.04 | 1.484 | 0.758 |
| shared_2w_2s | 8 | 7.57 / 9.05 | 4.05 | 2.301 | 0.800 |

### Chunk/event delay p95 (ms)

| Delay | shared_1w_1s | shared_1w_2s | shared_2w_1s | shared_2w_2s |
|---|---:|---:|---:|---:|
| send_lag | 0.465 | 0.395 | 0.390 | 0.375 |
| controller_queue_wait | 0.005 | 0.005 | 0.005 | 0.005 |
| submit_wall | 0.086 | 0.112 | 1.230 | 1.375 |
| publication_delay | 0.095 | 0.064 | 0.618 | 0.630 |

## zh

| Layout | Complete | Pre-EOF text | First text mean / p95 s | Final mean / p95 s | EOF delay mean s | Error | Peak RSS GiB | Audio s / wall s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| shared_1w_1s | 10/10 | 9/10 | 5.178 / 5.611 | 14.293 / 22.017 | 2.834 | CER 3.25% | 2.84 | 0.802 |
| shared_1w_2s | 10/10 | 9/10 | 5.277 / 5.849 | 14.650 / 22.291 | 3.192 | CER 3.25% | 2.84 | 1.498 |
| shared_2w_1s | 10/10 | 9/10 | 5.286 / 5.875 | 14.436 / 21.816 | 2.977 | CER 3.25% | 5.53 | 1.507 |
| shared_2w_2s | 10/10 | 9/10 | 6.381 / 8.946 | 15.540 / 22.746 | 4.080 | CER 3.25% | 5.54 | 2.781 |

### Stage timings (means in ms for completed calls)

| Stage | shared_1w_1s | shared_1w_2s | shared_2w_1s | shared_2w_2s |
|---|---:|---:|---:|---:|
| startup_ns | 0.003 | 0.005 | 0.089 | 0.113 |
| shared_model_load_ns | 359.623 | 369.642 | 353.309 | 356.212 |
| prefix_decode_wall_ns | 1177.769 | 1166.222 | 1285.192 | 1614.247 |
| prefix_decode_queue_wait_ns | 0.118 | 110.973 | 0.095 | 765.687 |
| eof_refinement_wall_ns | 2772.092 | 2754.386 | 2863.263 | 3380.155 |
| eof_decode_queue_wait_ns | 62.184 | 437.158 | 113.028 | 699.996 |
| runtime_queue_wait_ns | 62.302 | 548.131 | 113.124 | 1465.683 |
| offline_decode_wall_ns | 3949.861 | 3920.608 | 4148.456 | 4994.403 |

Shared-model load is reused context metadata, not a new model load per call. Failed-call decode durations remain in the per-call JSON and the phase CPU/throughput measurements.

| Layout | Configured compute threads | Mean / peak CPU cores | Peak PSS GiB | Effective RTF mean | Offline wall RTF mean |
|---|---:|---:|---:|---:|---:|
| shared_1w_1s | 4 | 1.27 / 4.75 | 2.84 | 1.261 | 0.373 |
| shared_1w_2s | 4 | 2.21 / 4.74 | 2.84 | 1.314 | 0.369 |
| shared_2w_1s | 8 | 2.47 / 8.98 | 4.06 | 1.283 | 0.397 |
| shared_2w_2s | 8 | 5.07 / 9.23 | 4.07 | 1.397 | 0.464 |

### Chunk/event delay p95 (ms)

| Delay | shared_1w_1s | shared_1w_2s | shared_2w_1s | shared_2w_2s |
|---|---:|---:|---:|---:|
| send_lag | 0.440 | 0.356 | 0.440 | 0.287 |
| controller_queue_wait | 0.004 | 0.004 | 0.004 | 0.005 |
| submit_wall | 0.051 | 0.055 | 1.085 | 1.215 |
| publication_delay | 0.097 | 0.075 | 0.419 | 0.472 |

## Scheduling and limitations

Least-active worker routing spreads calls across healthy workers before sharing a worker. Inside each worker, the oldest ready preview receives priority; at most two previews may bypass a waiting EOF refinement. The oldest EOF job then runs. Expired calls are retired first. A native decode cannot be preempted mid-invocation, so priority affects only the next job.

Failed calls use an empty final hypothesis for failure-inclusive WER/CER. First/final latency distributions use completed calls only; phase throughput includes time spent on failures. Failure details remain in each per-call record.

EOF delay is worker EOF receipt to final publication, not first-to-final time. Queue, offline decode, IPC publication, startup, effective RTF, and unavailable active-compute/stable-word fields are retained in `comparison.json`. Descriptive p95 from ten recordings is not a production tail guarantee. Accuracy uses corpus edits/reference units separately per language.

Warm model contexts are reused across language suites, which run separately. Language-level memory/throughput values are suite measurements; multi-language smoke suites are separately marked. Automatic worker restart is not implemented: failed workers reject new assignments, surviving workers continue; restart the service to restore full capacity. A worker failure can fail every active session on that worker.

Artifacts: `plan.json`, `jobs.json`, `comparison.json`, `calls.json`, `checksums.json`, service logs, cold CLI plans, runtime snapshots, and each original C++ suite. All raw timing records remain in the suite directories.
