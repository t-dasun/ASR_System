# Available ASR results by language

Updated: 2026-10-05. This historical inventory reorganizes the earlier evidence. The newer [10-WAV-per-language shared worker pool results](SHARED_WORKER_POOL_RESULTS.md) contain the complete multi-worker corpus matrix, failures, decoder guard, final C++/browser check and links to all measurements.

See the newer [shared worker pool implementation and run guide](SHARED_WORKER_POOL.md) for multi-worker routing, distinct-WAV chunk simulation and the per-language corpus runner. The historical data inventory below describes results available before that extension.

## Coverage and the missing experiment

**The matched four-layout experiment does not yet contain 10 unique WAVs per language.** It contains one unique WAV per language, offered twice per layout (24 calls total). Its earlier combined latency means describe the three-clip workload, not language-specific model performance. The tables below separate languages.

The older native holdout test contains five unique source recordings per language, each in clean and simulated telephone form. That is 10 audio variants/calls per language, but only five independent source recordings. Telephone degradation and worker sharing are different experiments.

| Evidence | Independent recordings per language | Calls | Layout / purpose |
|---|---:|---:|---|
| Current matched topology comparison | 1 | 2 per language per layout; 24 total | Native one worker, shared single-session control, shared two sessions, native two workers |
| Older native holdout clean/telephone comparison | 5 | 10 per language; 30 total | Native clean versus telephone degradation |
| Older measured tuning gate | 3 | 3 per language; 9 total | Native measurement/scoring check |
| Native chunk/decode matrix | 3 per configuration | 9 per configuration | Four chunk/decode settings; same small tuning cohort |
| Shared concurrency screen | One short English fixture | 20 at each concurrency 1/2/4/8; 80 total | Short-clip throughput and resource screen |
| Shared long concurrency screen | One English recording | 8 total | Long-input contention / pre-EOF screen |
| Shared mixed-language live service verification | 1 in each language | 3 simultaneous live calls, plus separate REST suite | Distinct-language routing and service admission |

Available prepared FLEURS manifests contain 50 heldout and 20 tuning recordings per language. Available audio is not evidence that it has been benchmarked.

## Current matched comparison: separate language tables

Each row has **two calls of one unique recording**. Means describe these two repetitions only. First text and final time start at stream start after readiness; EOF delay is worker EOF receipt to final publication. Shared startup is admission after the model is loaded; the shared context load (~340 ms) is separate. Accuracy is sum of edits divided by sum of reference units within a language. English/Indonesian use WER; Mandarin uses CER.

Native and shared engines have different preview policies. Native uses live decoding with a 2 s step; shared uses a single 4 s prefix preview plus EOF refinement. Native two workers have eight configured compute threads total; the other layouts have four.

### English

Input: `fleurs_en_us_validation_1605_16.wav`, 12.58 seconds.

| Layout | Complete / offered | Pre-EOF text | Startup mean ms | First text mean s | Final mean s | EOF delay mean s | Effective RTF | Error rate | Peak suite RSS GiB | Audio s / phase wall s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| native_1w_1s | 2/2 | 2/2 | 369.283 | 7.200 | 16.180 | 3.600 | 1.286 | WER 7.69% | 2.64 | 0.760 |
| shared_1w_1s_control | 2/2 | 2/2 | 0.005 | 5.045 | 14.943 | 2.363 | 1.188 | WER 7.69% | 2.65 | 0.842 |
| shared_1w_2s | 2/2 | 2/2 | 0.006 | 5.587 | 16.096 | 3.516 | 1.279 | WER 7.69% | 2.65 | 1.457 |
| native_2w_1s | 2/2 | 2/2 | 336.585 | 7.800 | 18.005 | 5.424 | 1.431 | WER 7.69% | 5.27 | 1.371 |

#### Available stage timings (means in milliseconds)

| Stage | native_1w_1s | shared_1w_1s_control | shared_1w_2s | native_2w_1s |
|---|---:|---:|---:|---:|
| model_load_ns | 364.477 | unavailable | unavailable | 331.918 |
| shared_model_load_ns | unavailable | 348.293 | 338.283 | unavailable |
| prefix_decode_wall_ns | unavailable | 1044.742 | 1046.927 | unavailable |
| prefix_decode_queue_wait_ns | unavailable | 0.201 | 540.311 | unavailable |
| eof_decode_queue_wait_ns | unavailable | 0.106 | 1172.027 | unavailable |
| runtime_queue_wait_ns | unavailable | 0.306 | 1712.338 | unavailable |
| eof_refinement_wall_ns | 2340.096 | 2362.405 | 2343.731 | 3492.647 |
| offline_decode_wall_ns | unavailable | 3407.147 | 3390.658 | unavailable |
| live_invocation_wall_ns | 13742.803 | unavailable | unavailable | 14416.050 |
| scheduled_final_lag_ns | 3599.904 | 2362.917 | 3516.003 | 5424.623 |
| worker_cpu_ns | 34908.082 | unavailable | unavailable | 49462.156 |

#### Individual calls

| Layout | Call | Worker | First text s | Final s | EOF delay s |
|---|---:|---|---:|---:|---:|
| native_1w_1s | 1 | `worker_0_g1` | 7.200 | 16.220 | 3.640 |
| native_1w_1s | 2 | `worker_0_g2` | 7.200 | 16.140 | 3.560 |
| shared_1w_1s_control | 1 | `prefix_shared_0` | 5.084 | 14.951 | 2.370 |
| shared_1w_1s_control | 2 | `prefix_shared_0` | 5.006 | 14.935 | 2.355 |
| shared_1w_2s | 1 | `prefix_shared_0` | 5.081 | 14.924 | 2.344 |
| shared_1w_2s | 2 | `prefix_shared_0` | 6.094 | 17.268 | 4.687 |
| native_2w_1s | 1 | `worker_0_g1` | 7.800 | 17.998 | 5.417 |
| native_2w_1s | 2 | `worker_1_g1` | 7.800 | 18.012 | 5.431 |

### Indonesian

Input: `fleurs_id_id_validation_1520_4.wav`, 15.48 seconds.

| Layout | Complete / offered | Pre-EOF text | Startup mean ms | First text mean s | Final mean s | EOF delay mean s | Effective RTF | Error rate | Peak suite RSS GiB | Audio s / phase wall s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| native_1w_1s | 2/2 | 2/2 | 348.491 | 7.200 | 19.976 | 4.495 | 1.290 | WER 8.33% | 2.67 | 0.762 |
| shared_1w_1s_control | 2/2 | 2/2 | 0.005 | 5.287 | 18.781 | 3.301 | 1.213 | WER 8.33% | 2.66 | 0.824 |
| shared_1w_2s | 2/2 | 2/2 | 0.006 | 5.951 | 20.428 | 4.948 | 1.320 | WER 8.33% | 2.66 | 1.402 |
| native_2w_1s | 2/2 | 2/2 | 334.071 | 7.600 | 21.936 | 6.455 | 1.417 | WER 8.33% | 5.32 | 1.390 |

#### Available stage timings (means in milliseconds)

| Stage | native_1w_1s | shared_1w_1s_control | shared_1w_2s | native_2w_1s |
|---|---:|---:|---:|---:|
| model_load_ns | 342.892 | unavailable | unavailable | 328.701 |
| shared_model_load_ns | unavailable | 341.556 | 333.605 | unavailable |
| prefix_decode_wall_ns | unavailable | 1286.666 | 1292.751 | unavailable |
| prefix_decode_queue_wait_ns | unavailable | 0.159 | 657.637 | unavailable |
| eof_decode_queue_wait_ns | unavailable | 0.138 | 1646.588 | unavailable |
| runtime_queue_wait_ns | unavailable | 0.297 | 2304.225 | unavailable |
| eof_refinement_wall_ns | 3235.232 | 3300.485 | 3300.849 | 4687.223 |
| offline_decode_wall_ns | unavailable | 4587.151 | 4593.601 | unavailable |
| live_invocation_wall_ns | 16648.507 | unavailable | unavailable | 17168.284 |
| scheduled_final_lag_ns | 4495.717 | 3300.906 | 4947.731 | 6455.599 |
| worker_cpu_ns | 46752.505 | unavailable | unavailable | 66435.566 |

#### Individual calls

| Layout | Call | Worker | First text s | Final s | EOF delay s |
|---|---:|---|---:|---:|---:|
| native_1w_1s | 1 | `worker_0_g1` | 7.200 | 19.975 | 4.494 |
| native_1w_1s | 2 | `worker_0_g2` | 7.200 | 19.976 | 4.496 |
| shared_1w_1s_control | 1 | `prefix_shared_0` | 5.308 | 18.769 | 3.288 |
| shared_1w_1s_control | 2 | `prefix_shared_0` | 5.266 | 18.793 | 3.313 |
| shared_1w_2s | 1 | `prefix_shared_0` | 5.315 | 18.773 | 3.293 |
| shared_1w_2s | 2 | `prefix_shared_0` | 6.586 | 22.082 | 6.602 |
| native_2w_1s | 1 | `worker_0_g1` | 7.600 | 21.939 | 6.459 |
| native_2w_1s | 2 | `worker_1_g1` | 7.600 | 21.932 | 6.452 |

### Mandarin Chinese

Input: `fleurs_cmn_hans_cn_validation_1559_6.wav`, 14.28 seconds.

| Layout | Complete / offered | Pre-EOF text | Startup mean ms | First text mean s | Final mean s | EOF delay mean s | Effective RTF | Error rate | Peak suite RSS GiB | Audio s / phase wall s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| native_1w_1s | 2/2 | 2/2 | 356.285 | 7.000 | 17.817 | 3.536 | 1.248 | CER 0.00% | 2.64 | 0.786 |
| shared_1w_1s_control | 2/2 | 2/2 | 0.004 | 4.715 | 16.256 | 1.976 | 1.138 | CER 0.00% | 2.65 | 0.878 |
| shared_1w_2s | 2/2 | 2/2 | 0.005 | 5.050 | 17.193 | 2.912 | 1.204 | CER 0.00% | 2.65 | 1.571 |
| native_2w_1s | 2/2 | 2/2 | 340.645 | 7.200 | 18.924 | 4.644 | 1.325 | CER 0.00% | 5.28 | 1.481 |

#### Available stage timings (means in milliseconds)

| Stage | native_1w_1s | shared_1w_1s_control | shared_1w_2s | native_2w_1s |
|---|---:|---:|---:|---:|
| model_load_ns | 351.482 | unavailable | unavailable | 335.833 |
| shared_model_load_ns | unavailable | 330.021 | 342.188 | unavailable |
| prefix_decode_wall_ns | unavailable | 715.037 | 687.675 | unavailable |
| prefix_decode_queue_wait_ns | unavailable | 0.182 | 361.738 | unavailable |
| eof_decode_queue_wait_ns | unavailable | 0.168 | 966.277 | unavailable |
| runtime_queue_wait_ns | unavailable | 0.350 | 1328.014 | unavailable |
| eof_refinement_wall_ns | 1921.171 | 1975.367 | 1946.055 | 2396.834 |
| offline_decode_wall_ns | unavailable | 2690.404 | 2633.730 | unavailable |
| live_invocation_wall_ns | 15804.328 | unavailable | unavailable | 16444.010 |
| scheduled_final_lag_ns | 3536.682 | 1975.934 | 2912.524 | 4643.800 |
| worker_cpu_ns | 34152.622 | unavailable | unavailable | 46101.431 |

#### Individual calls

| Layout | Call | Worker | First text s | Final s | EOF delay s |
|---|---:|---|---:|---:|---:|
| native_1w_1s | 1 | `worker_0_g1` | 7.000 | 17.802 | 3.522 |
| native_1w_1s | 2 | `worker_0_g2` | 7.000 | 17.831 | 3.551 |
| shared_1w_1s_control | 1 | `prefix_shared_0` | 4.740 | 16.267 | 1.987 |
| shared_1w_1s_control | 2 | `prefix_shared_0` | 4.691 | 16.245 | 1.964 |
| shared_1w_2s | 1 | `prefix_shared_0` | 4.724 | 16.213 | 1.933 |
| shared_1w_2s | 2 | `prefix_shared_0` | 5.376 | 18.172 | 3.892 |
| native_2w_1s | 1 | `worker_0_g1` | 7.200 | 18.938 | 4.658 |
| native_2w_1s | 2 | `worker_1_g1` | 7.200 | 18.909 | 4.629 |

Native live invocation includes paced waiting; shared offline decode excludes paced waiting and queue wait. Worker CPU is CPU time and can exceed wall time. Active inference compute time and stable-word delay are unavailable. Suite RSS is a sampled process-tree peak. See the JSON for CPU sampling, every per-call measurement, raw chunk/event durations, transcripts, alignments and artifact hashes.

## Older native holdout: separate language and audio-condition results

Five unique source recordings per language. Clean and telephone variants are paired. These older runs have different configuration/build provenance and should be read as their own experiment.

| Language | Sources | Clean error | Telephone error | Clean first-text p50 s | Telephone first-text p50 s | Clean final p50 s | Telephone final p50 s |
|---|---:|---:|---:|---:|---:|---:|---:|
| English | 5 | WER 5.83% | WER 12.62% | 7.200 | 7.000 | 11.395 | 11.495 |
| Indonesian | 5 | WER 5.68% | WER 14.77% | 9.609 | 9.242 | 14.193 | 14.179 |
| Mandarin Chinese | 5 | CER 5.59% | CER 2.48% | 7.200 | 7.200 | 13.485 | 13.517 |

[Per-recording clean/telephone readings](../results/m9_fleurs_holdout5_20261004/comparison.json), [raw accuracy and alignments](../results/m9_fleurs_holdout5_20261004/measured/accuracy.jsonl), [native call artifacts](../results/m9_fleurs_holdout5_20261004/measured/calls).

## One place to find all existing readings

### Current C++ multiplexing evidence

- [Matched comparison report](MATCHED_TOPOLOGY_COMPARISON.md): definitions, CPU/PSS, all timing stages and reproduction commands.
- [Full matched JSON](../tools/multiplexing/matched_topology_comparison_20261005.json): 24 per-call records, raw delay arrays, transcript scoring, commands and 320 artifact hashes.
- [Per-call CSV](../tools/multiplexing/matched_topology_calls_20261005.csv): individual call timings and final text.
- [Metric CSV](../tools/multiplexing/matched_topology_metrics_20261005.csv): descriptive aggregate percentiles across languages; use the language tables above for language comparisons.
- [Time multiplexing results](TIME_MULTIPLEXING_RESULTS.md): controlled screens, live API verification, deadlines and historical experiments.
- [Original matched C++ artifacts](../results/time_multiplexing/matched_topologies_20261005): configuration, events, audio/runtime timings, process metrics, logs and status.

### Every saved multiplexing result JSON

- [asr_cli_serve_prefix_two_calls_20261005.json](../tools/multiplexing/asr_cli_serve_prefix_two_calls_20261005.json)
- [llama_complete_audio_two_calls_20261005.json](../tools/multiplexing/llama_complete_audio_two_calls_20261005.json)
- [main_cpp_deadline_20261005.json](../tools/multiplexing/main_cpp_deadline_20261005.json)
- [main_cpp_direct_n4_20261005.json](../tools/multiplexing/main_cpp_direct_n4_20261005.json)
- [main_cpp_openmp4_long_n8_20261005.json](../tools/multiplexing/main_cpp_openmp4_long_n8_20261005.json)
- [main_cpp_openmp4_screen_20261005.json](../tools/multiplexing/main_cpp_openmp4_screen_20261005.json)
- [main_cpp_openmp4_service_verification_20261005.json](../tools/multiplexing/main_cpp_openmp4_service_verification_20261005.json)
- [main_cpp_service_verification_20261005.json](../tools/multiplexing/main_cpp_service_verification_20261005.json)
- [main_cpp_short_screen_20261005.json](../tools/multiplexing/main_cpp_short_screen_20261005.json)
- [matched_topology_comparison_20261005.json](../tools/multiplexing/matched_topology_comparison_20261005.json)
- [native_prefix_two_calls_20261005.json](../tools/multiplexing/native_prefix_two_calls_20261005.json)
- [native_prefix_two_calls_reversed_20261005.json](../tools/multiplexing/native_prefix_two_calls_reversed_20261005.json)
- [prefix_admission_n8_20261005.json](../tools/multiplexing/prefix_admission_n8_20261005.json)
- [prefix_cancel_survivor_20261005.json](../tools/multiplexing/prefix_cancel_survivor_20261005.json)
- [prefix_load_n1_20261005.json](../tools/multiplexing/prefix_load_n1_20261005.json)
- [prefix_load_n2_20261005.json](../tools/multiplexing/prefix_load_n2_20261005.json)
- [prefix_load_n4_20261005.json](../tools/multiplexing/prefix_load_n4_20261005.json)
- [prefix_load_n8_20261005.json](../tools/multiplexing/prefix_load_n8_20261005.json)
- [prefix_websocket_two_calls_20261005.json](../tools/multiplexing/prefix_websocket_two_calls_20261005.json)
- [prefix_websocket_two_calls_admission_20261005.json](../tools/multiplexing/prefix_websocket_two_calls_admission_20261005.json)

Historical probes may use different thread settings, policies or timing origins. The controlled `openmp4` and matched results are identified in the linked reports; do not combine all probe records into one statistical sample.

### Earlier reports and experiments

- [Final technical report](../reports/FINAL_TECHNICAL_REPORT.md): earlier native architecture, qualification limits and production proposal.
- [Report generation and M10 evidence guide](../reports/README.md).
- [Assignment questions and answers](ASSIGNMENT_QUESTIONS_AND_ANSWERS.md).

Every existing top-level result directory is indexed below, including unsuccessful diagnostics and mock runs. Presence in this list does not imply a successful or comparable benchmark. Each directory retains its own status/configuration where recorded.

- [20261001T092231Z_native_probe_f54dab6c](../results/20261001T092231Z_native_probe_f54dab6c) — [summary.json](../results/20261001T092231Z_native_probe_f54dab6c/summary.json), [status.json](../results/20261001T092231Z_native_probe_f54dab6c/status.json)
- [20261001T092258Z_native_probe_ef9a496d](../results/20261001T092258Z_native_probe_ef9a496d) — [summary.json](../results/20261001T092258Z_native_probe_ef9a496d/summary.json), [status.json](../results/20261001T092258Z_native_probe_ef9a496d/status.json)
- [20261001T135434Z_native_probe_71811f88](../results/20261001T135434Z_native_probe_71811f88) — [summary.json](../results/20261001T135434Z_native_probe_71811f88/summary.json), [status.json](../results/20261001T135434Z_native_probe_71811f88/status.json)
- [20261001T135455Z_native_probe_d22ab067](../results/20261001T135455Z_native_probe_d22ab067) — [summary.json](../results/20261001T135455Z_native_probe_d22ab067/summary.json), [status.json](../results/20261001T135455Z_native_probe_d22ab067/status.json)
- [20261001T135536Z_native_probe_8da2842b](../results/20261001T135536Z_native_probe_8da2842b) — [summary.json](../results/20261001T135536Z_native_probe_8da2842b/summary.json), [status.json](../results/20261001T135536Z_native_probe_8da2842b/status.json)
- [20261001T135658Z_native_probe_6610dd72](../results/20261001T135658Z_native_probe_6610dd72) — [summary.json](../results/20261001T135658Z_native_probe_6610dd72/summary.json), [status.json](../results/20261001T135658Z_native_probe_6610dd72/status.json)
- [20261001T135817Z_native_probe_cf26eb8f](../results/20261001T135817Z_native_probe_cf26eb8f) — [summary.json](../results/20261001T135817Z_native_probe_cf26eb8f/summary.json), [status.json](../results/20261001T135817Z_native_probe_cf26eb8f/status.json)
- [20261001T140145Z_native_probe_cc154bac](../results/20261001T140145Z_native_probe_cc154bac) — [status.json](../results/20261001T140145Z_native_probe_cc154bac/status.json)
- [20261001T140218Z_native_probe_5c389e6e](../results/20261001T140218Z_native_probe_5c389e6e) — [summary.json](../results/20261001T140218Z_native_probe_5c389e6e/summary.json), [status.json](../results/20261001T140218Z_native_probe_5c389e6e/status.json)
- [20261001T140226Z_native_probe_77b460e4](../results/20261001T140226Z_native_probe_77b460e4) — [summary.json](../results/20261001T140226Z_native_probe_77b460e4/summary.json), [status.json](../results/20261001T140226Z_native_probe_77b460e4/status.json)
- [20261001T145905Z_native_probe_a3f34844](../results/20261001T145905Z_native_probe_a3f34844) — [summary.json](../results/20261001T145905Z_native_probe_a3f34844/summary.json), [status.json](../results/20261001T145905Z_native_probe_a3f34844/status.json)
- [20261001T150053Z_native_probe_87c70c41](../results/20261001T150053Z_native_probe_87c70c41) — [summary.json](../results/20261001T150053Z_native_probe_87c70c41/summary.json), [status.json](../results/20261001T150053Z_native_probe_87c70c41/status.json)
- [20261001T150107Z_native_probe_5e0c24aa](../results/20261001T150107Z_native_probe_5e0c24aa) — [summary.json](../results/20261001T150107Z_native_probe_5e0c24aa/summary.json), [status.json](../results/20261001T150107Z_native_probe_5e0c24aa/status.json)
- [20261001T150124Z_native_probe_2962eb80](../results/20261001T150124Z_native_probe_2962eb80) — [summary.json](../results/20261001T150124Z_native_probe_2962eb80/summary.json), [status.json](../results/20261001T150124Z_native_probe_2962eb80/status.json)
- [20261001T150143Z_native_probe_a6aeeb19](../results/20261001T150143Z_native_probe_a6aeeb19) — [summary.json](../results/20261001T150143Z_native_probe_a6aeeb19/summary.json), [status.json](../results/20261001T150143Z_native_probe_a6aeeb19/status.json)
- [20261001T150157Z_native_probe_852e8376](../results/20261001T150157Z_native_probe_852e8376) — [summary.json](../results/20261001T150157Z_native_probe_852e8376/summary.json), [status.json](../results/20261001T150157Z_native_probe_852e8376/status.json)
- [20261001T150207Z_native_probe_1bed52e5](../results/20261001T150207Z_native_probe_1bed52e5) — [summary.json](../results/20261001T150207Z_native_probe_1bed52e5/summary.json), [status.json](../results/20261001T150207Z_native_probe_1bed52e5/status.json)
- [20261001T150222Z_native_probe_c9736fe3](../results/20261001T150222Z_native_probe_c9736fe3) — [summary.json](../results/20261001T150222Z_native_probe_c9736fe3/summary.json), [status.json](../results/20261001T150222Z_native_probe_c9736fe3/status.json)
- [20261001T150239Z_native_probe_970e20cd](../results/20261001T150239Z_native_probe_970e20cd) — [summary.json](../results/20261001T150239Z_native_probe_970e20cd/summary.json), [status.json](../results/20261001T150239Z_native_probe_970e20cd/status.json)
- [20261001T150302Z_native_probe_d11400dc](../results/20261001T150302Z_native_probe_d11400dc) — [summary.json](../results/20261001T150302Z_native_probe_d11400dc/summary.json), [status.json](../results/20261001T150302Z_native_probe_d11400dc/status.json)
- [20261001T150317Z_native_probe_8cfa3419](../results/20261001T150317Z_native_probe_8cfa3419) — [summary.json](../results/20261001T150317Z_native_probe_8cfa3419/summary.json), [status.json](../results/20261001T150317Z_native_probe_8cfa3419/status.json)
- [20261001T150331Z_native_probe_af7e2af7](../results/20261001T150331Z_native_probe_af7e2af7) — [summary.json](../results/20261001T150331Z_native_probe_af7e2af7/summary.json), [status.json](../results/20261001T150331Z_native_probe_af7e2af7/status.json)
- [20261001T150341Z_native_probe_10e08982](../results/20261001T150341Z_native_probe_10e08982) — [summary.json](../results/20261001T150341Z_native_probe_10e08982/summary.json), [status.json](../results/20261001T150341Z_native_probe_10e08982/status.json)
- [20261001T150400Z_native_probe_e23cbe94](../results/20261001T150400Z_native_probe_e23cbe94) — [summary.json](../results/20261001T150400Z_native_probe_e23cbe94/summary.json), [status.json](../results/20261001T150400Z_native_probe_e23cbe94/status.json)
- [20261001T150424Z_native_probe_b154b53c](../results/20261001T150424Z_native_probe_b154b53c) — [status.json](../results/20261001T150424Z_native_probe_b154b53c/status.json)
- [20261001T150437Z_native_probe_8b18c7e6](../results/20261001T150437Z_native_probe_8b18c7e6) — [summary.json](../results/20261001T150437Z_native_probe_8b18c7e6/summary.json), [status.json](../results/20261001T150437Z_native_probe_8b18c7e6/status.json)
- [20261001T150558Z_native_probe_a1104d2b](../results/20261001T150558Z_native_probe_a1104d2b) — [summary.json](../results/20261001T150558Z_native_probe_a1104d2b/summary.json), [status.json](../results/20261001T150558Z_native_probe_a1104d2b/status.json)
- [20261001T150625Z_native_probe_9e26f067](../results/20261001T150625Z_native_probe_9e26f067) — [summary.json](../results/20261001T150625Z_native_probe_9e26f067/summary.json), [status.json](../results/20261001T150625Z_native_probe_9e26f067/status.json)
- [20261001T150638Z_native_probe_d05e89b7](../results/20261001T150638Z_native_probe_d05e89b7) — [summary.json](../results/20261001T150638Z_native_probe_d05e89b7/summary.json), [status.json](../results/20261001T150638Z_native_probe_d05e89b7/status.json)
- [20261001T150655Z_native_probe_7bec9c24](../results/20261001T150655Z_native_probe_7bec9c24) — [summary.json](../results/20261001T150655Z_native_probe_7bec9c24/summary.json), [status.json](../results/20261001T150655Z_native_probe_7bec9c24/status.json)
- [20261001T150714Z_native_probe_be0e346e](../results/20261001T150714Z_native_probe_be0e346e) — [summary.json](../results/20261001T150714Z_native_probe_be0e346e/summary.json), [status.json](../results/20261001T150714Z_native_probe_be0e346e/status.json)
- [20261001T150728Z_native_probe_82b9993d](../results/20261001T150728Z_native_probe_82b9993d) — [summary.json](../results/20261001T150728Z_native_probe_82b9993d/summary.json), [status.json](../results/20261001T150728Z_native_probe_82b9993d/status.json)
- [20261001T150739Z_native_probe_b6bdbc5a](../results/20261001T150739Z_native_probe_b6bdbc5a) — [summary.json](../results/20261001T150739Z_native_probe_b6bdbc5a/summary.json), [status.json](../results/20261001T150739Z_native_probe_b6bdbc5a/status.json)
- [20261001T150754Z_native_probe_96bfa262](../results/20261001T150754Z_native_probe_96bfa262) — [summary.json](../results/20261001T150754Z_native_probe_96bfa262/summary.json), [status.json](../results/20261001T150754Z_native_probe_96bfa262/status.json)
- [20261001T150811Z_native_probe_76633ec0](../results/20261001T150811Z_native_probe_76633ec0) — [summary.json](../results/20261001T150811Z_native_probe_76633ec0/summary.json), [status.json](../results/20261001T150811Z_native_probe_76633ec0/status.json)
- [20261001T150834Z_native_probe_035ff2ec](../results/20261001T150834Z_native_probe_035ff2ec) — [summary.json](../results/20261001T150834Z_native_probe_035ff2ec/summary.json), [status.json](../results/20261001T150834Z_native_probe_035ff2ec/status.json)
- [20261001T150850Z_native_probe_9f1e61e3](../results/20261001T150850Z_native_probe_9f1e61e3) — [summary.json](../results/20261001T150850Z_native_probe_9f1e61e3/summary.json), [status.json](../results/20261001T150850Z_native_probe_9f1e61e3/status.json)
- [20261001T150904Z_native_probe_1d30fc17](../results/20261001T150904Z_native_probe_1d30fc17) — [summary.json](../results/20261001T150904Z_native_probe_1d30fc17/summary.json), [status.json](../results/20261001T150904Z_native_probe_1d30fc17/status.json)
- [20261001T150914Z_native_probe_637740cc](../results/20261001T150914Z_native_probe_637740cc) — [summary.json](../results/20261001T150914Z_native_probe_637740cc/summary.json), [status.json](../results/20261001T150914Z_native_probe_637740cc/status.json)
- [20261001T150933Z_native_probe_f354f0af](../results/20261001T150933Z_native_probe_f354f0af) — [summary.json](../results/20261001T150933Z_native_probe_f354f0af/summary.json), [status.json](../results/20261001T150933Z_native_probe_f354f0af/status.json)
- [20261001T150957Z_native_probe_4bc12276](../results/20261001T150957Z_native_probe_4bc12276) — [summary.json](../results/20261001T150957Z_native_probe_4bc12276/summary.json), [status.json](../results/20261001T150957Z_native_probe_4bc12276/status.json)
- [20261001T151010Z_native_probe_f3c98111](../results/20261001T151010Z_native_probe_f3c98111) — [summary.json](../results/20261001T151010Z_native_probe_f3c98111/summary.json), [status.json](../results/20261001T151010Z_native_probe_f3c98111/status.json)
- [20261001T151028Z_native_lifecycle_4da6a30b](../results/20261001T151028Z_native_lifecycle_4da6a30b) — [summary.json](../results/20261001T151028Z_native_lifecycle_4da6a30b/summary.json), [status.json](../results/20261001T151028Z_native_lifecycle_4da6a30b/status.json)
- [20261001T151230Z_native_lifecycle_916282f8](../results/20261001T151230Z_native_lifecycle_916282f8) — [summary.json](../results/20261001T151230Z_native_lifecycle_916282f8/summary.json), [status.json](../results/20261001T151230Z_native_lifecycle_916282f8/status.json)
- [20261001T151251Z_native_probe_3e0e392c](../results/20261001T151251Z_native_probe_3e0e392c) — [status.json](../results/20261001T151251Z_native_probe_3e0e392c/status.json)
- [20261001T151318Z_native_probe_8223a454](../results/20261001T151318Z_native_probe_8223a454) — [summary.json](../results/20261001T151318Z_native_probe_8223a454/summary.json), [status.json](../results/20261001T151318Z_native_probe_8223a454/status.json)
- [chunk_decode_matrix_20261005](../results/chunk_decode_matrix_20261005) — [report.json](../results/chunk_decode_matrix_20261005/report.json)
- [load_18db4c9f550619a1_8f08a7e78f2ac02c0054a24dadb611f4](../results/load_18db4c9f550619a1_8f08a7e78f2ac02c0054a24dadb611f4) — [summary.json](../results/load_18db4c9f550619a1_8f08a7e78f2ac02c0054a24dadb611f4/summary.json), [status.json](../results/load_18db4c9f550619a1_8f08a7e78f2ac02c0054a24dadb611f4/status.json)
- [load_18db5a4f2c8590d6_915465b69549a9c0ea94898c03e40a01](../results/load_18db5a4f2c8590d6_915465b69549a9c0ea94898c03e40a01) — [summary.json](../results/load_18db5a4f2c8590d6_915465b69549a9c0ea94898c03e40a01/summary.json), [status.json](../results/load_18db5a4f2c8590d6_915465b69549a9c0ea94898c03e40a01/status.json)
- [load_18db5a4f999ec105_0df6ae8ba335f6883aa42f2c41d00495](../results/load_18db5a4f999ec105_0df6ae8ba335f6883aa42f2c41d00495) — [summary.json](../results/load_18db5a4f999ec105_0df6ae8ba335f6883aa42f2c41d00495/summary.json), [status.json](../results/load_18db5a4f999ec105_0df6ae8ba335f6883aa42f2c41d00495/status.json)
- [load_18db5a6201308720_b1c92b15076683f60117a007c11b6de8](../results/load_18db5a6201308720_b1c92b15076683f60117a007c11b6de8) — [summary.json](../results/load_18db5a6201308720_b1c92b15076683f60117a007c11b6de8/summary.json), [status.json](../results/load_18db5a6201308720_b1c92b15076683f60117a007c11b6de8/status.json)
- [load_18db5a626e8387c4_7af009cf1c42a4c42e048a142fa7c25a](../results/load_18db5a626e8387c4_7af009cf1c42a4c42e048a142fa7c25a) — [summary.json](../results/load_18db5a626e8387c4_7af009cf1c42a4c42e048a142fa7c25a/summary.json), [status.json](../results/load_18db5a626e8387c4_7af009cf1c42a4c42e048a142fa7c25a/status.json)
- [load_18db5aac01d00904_e0cf7803f85d1f6dc77e6b27b0187b13](../results/load_18db5aac01d00904_e0cf7803f85d1f6dc77e6b27b0187b13) — [summary.json](../results/load_18db5aac01d00904_e0cf7803f85d1f6dc77e6b27b0187b13/summary.json), [status.json](../results/load_18db5aac01d00904_e0cf7803f85d1f6dc77e6b27b0187b13/status.json)
- [load_18db5ad4a30c32da_774ed4ce6a7e47cceb7e996394a0b68c](../results/load_18db5ad4a30c32da_774ed4ce6a7e47cceb7e996394a0b68c) — [summary.json](../results/load_18db5ad4a30c32da_774ed4ce6a7e47cceb7e996394a0b68c/summary.json), [status.json](../results/load_18db5ad4a30c32da_774ed4ce6a7e47cceb7e996394a0b68c/status.json)
- [load_18db5ad54166ab6e_9125717e7b3914fcf500eb7306901d26](../results/load_18db5ad54166ab6e_9125717e7b3914fcf500eb7306901d26) — [summary.json](../results/load_18db5ad54166ab6e_9125717e7b3914fcf500eb7306901d26/summary.json), [status.json](../results/load_18db5ad54166ab6e_9125717e7b3914fcf500eb7306901d26/status.json)
- [load_18db5adbd45e7bde_8cba9a0244ab0b87c7cb4c5be1d14225](../results/load_18db5adbd45e7bde_8cba9a0244ab0b87c7cb4c5be1d14225) — [summary.json](../results/load_18db5adbd45e7bde_8cba9a0244ab0b87c7cb4c5be1d14225/summary.json), [status.json](../results/load_18db5adbd45e7bde_8cba9a0244ab0b87c7cb4c5be1d14225/status.json)
- [load_18db5ae2749b5e7d_3d9fea9e8c363781e0bc9685e26ed773](../results/load_18db5ae2749b5e7d_3d9fea9e8c363781e0bc9685e26ed773) — [summary.json](../results/load_18db5ae2749b5e7d_3d9fea9e8c363781e0bc9685e26ed773/summary.json), [status.json](../results/load_18db5ae2749b5e7d_3d9fea9e8c363781e0bc9685e26ed773/status.json)
- [load_18db5ae879797f1c_1c767df247165cadfcfbcee09db56789](../results/load_18db5ae879797f1c_1c767df247165cadfcfbcee09db56789) — [summary.json](../results/load_18db5ae879797f1c_1c767df247165cadfcfbcee09db56789/summary.json), [status.json](../results/load_18db5ae879797f1c_1c767df247165cadfcfbcee09db56789/status.json)
- [load_18db5b40813995d3_df056f6b28f9cc4d0b48be6e2b0207cf](../results/load_18db5b40813995d3_df056f6b28f9cc4d0b48be6e2b0207cf) — [summary.json](../results/load_18db5b40813995d3_df056f6b28f9cc4d0b48be6e2b0207cf/summary.json), [status.json](../results/load_18db5b40813995d3_df056f6b28f9cc4d0b48be6e2b0207cf/status.json)
- [load_18db5b40ee8268d4_5c28ae010f695ccf19503c4824ffd6fd](../results/load_18db5b40ee8268d4_5c28ae010f695ccf19503c4824ffd6fd) — [summary.json](../results/load_18db5b40ee8268d4_5c28ae010f695ccf19503c4824ffd6fd/summary.json), [status.json](../results/load_18db5b40ee8268d4_5c28ae010f695ccf19503c4824ffd6fd/status.json)
- [m0-fixtures](../results/m0-fixtures)
- [m0-initial](../results/m0-initial)
- [m10_capacity_screen_20261005](../results/m10_capacity_screen_20261005)
- [m11_clean_native_smoke_20261005](../results/m11_clean_native_smoke_20261005)
- [m11_three_language_demo_20261005](../results/m11_three_language_demo_20261005)
- [m11_three_language_demo_20261005_long](../results/m11_three_language_demo_20261005_long)
- [m11_three_language_demo_20261005_v2](../results/m11_three_language_demo_20261005_v2)
- [m4_measured_20261002](../results/m4_measured_20261002) — [summary.json](../results/m4_measured_20261002/summary.json), [status.json](../results/m4_measured_20261002/status.json)
- [m4_measured_threads_fixed_20261002](../results/m4_measured_threads_fixed_20261002) — [summary.json](../results/m4_measured_threads_fixed_20261002/summary.json), [status.json](../results/m4_measured_threads_fixed_20261002/status.json)
- [m5_native_single](../results/m5_native_single)
- [m6_mock_smoke](../results/m6_mock_smoke)
- [m6_native_single](../results/m6_native_single)
- [m6_network_mock](../results/m6_network_mock)
- [m6_network_native](../results/m6_network_native)
- [m6_network_sweep_mock](../results/m6_network_sweep_mock)
- [m6_sweep_mock](../results/m6_sweep_mock)
- [m7_native_api](../results/m7_native_api)
- [m9_fleurs_holdout5_20261004](../results/m9_fleurs_holdout5_20261004) — [comparison.json](../results/m9_fleurs_holdout5_20261004/comparison.json), [status.json](../results/m9_fleurs_holdout5_20261004/status.json)
- [m9_fleurs_pairs_20261004_screen](../results/m9_fleurs_pairs_20261004_screen) — [status.json](../results/m9_fleurs_pairs_20261004_screen/status.json)
- [m9_fleurs_pairs_20261004_screen_v2](../results/m9_fleurs_pairs_20261004_screen_v2) — [comparison.json](../results/m9_fleurs_pairs_20261004_screen_v2/comparison.json), [status.json](../results/m9_fleurs_pairs_20261004_screen_v2/status.json)
- [m9_fleurs_pairs_20261004_tuning3](../results/m9_fleurs_pairs_20261004_tuning3) — [comparison.json](../results/m9_fleurs_pairs_20261004_tuning3/comparison.json), [status.json](../results/m9_fleurs_pairs_20261004_tuning3/status.json)
- [m9_qwen_threads2_tuning3](../results/m9_qwen_threads2_tuning3) — [summary.json](../results/m9_qwen_threads2_tuning3/summary.json), [status.json](../results/m9_qwen_threads2_tuning3/status.json)
- [m9_silence_native_20261004](../results/m9_silence_native_20261004)
- [m9_stress_20261004](../results/m9_stress_20261004)
- [m9_stress_90s_20261004](../results/m9_stress_90s_20261004)
- [m9_stress_native_20261004](../results/m9_stress_native_20261004) — [audit.json](../results/m9_stress_native_20261004/audit.json)
- [m9_stress_native_timeout150s_20261004](../results/m9_stress_native_timeout150s_20261004) — [audit.json](../results/m9_stress_native_timeout150s_20261004/audit.json)
- [m9_two_worker_recovery_20261004](../results/m9_two_worker_recovery_20261004)
- [mock_18da73a2f154de75_f0d1ff8290b9d5c9519c3eac39ac1ba6](../results/mock_18da73a2f154de75_f0d1ff8290b9d5c9519c3eac39ac1ba6) — [summary.json](../results/mock_18da73a2f154de75_f0d1ff8290b9d5c9519c3eac39ac1ba6/summary.json), [status.json](../results/mock_18da73a2f154de75_f0d1ff8290b9d5c9519c3eac39ac1ba6/status.json)
- [mock_18da73a2f177c001_6ae34d2c6dd2b62d9c205bdbfdb01072](../results/mock_18da73a2f177c001_6ae34d2c6dd2b62d9c205bdbfdb01072) — [summary.json](../results/mock_18da73a2f177c001_6ae34d2c6dd2b62d9c205bdbfdb01072/summary.json), [status.json](../results/mock_18da73a2f177c001_6ae34d2c6dd2b62d9c205bdbfdb01072/status.json)
- [mock_18da7df339af5744_11b5796cfed0b65269777f414ee2bfc6](../results/mock_18da7df339af5744_11b5796cfed0b65269777f414ee2bfc6) — [summary.json](../results/mock_18da7df339af5744_11b5796cfed0b65269777f414ee2bfc6/summary.json), [status.json](../results/mock_18da7df339af5744_11b5796cfed0b65269777f414ee2bfc6/status.json)
- [mock_18da7e17412eb179_4e55dc2079aeb850078caec9cc9f6aa6](../results/mock_18da7e17412eb179_4e55dc2079aeb850078caec9cc9f6aa6) — [summary.json](../results/mock_18da7e17412eb179_4e55dc2079aeb850078caec9cc9f6aa6/summary.json), [status.json](../results/mock_18da7e17412eb179_4e55dc2079aeb850078caec9cc9f6aa6/status.json)
- [mock_18da7e57c93e38cf_546e548e323e873daa469fa26ab64ae5](../results/mock_18da7e57c93e38cf_546e548e323e873daa469fa26ab64ae5) — [summary.json](../results/mock_18da7e57c93e38cf_546e548e323e873daa469fa26ab64ae5/summary.json), [status.json](../results/mock_18da7e57c93e38cf_546e548e323e873daa469fa26ab64ae5/status.json)
- [native_18da7ee6e55acee7_4b115abbe11f963c4aad30d03738fd4e](../results/native_18da7ee6e55acee7_4b115abbe11f963c4aad30d03738fd4e) — [summary.json](../results/native_18da7ee6e55acee7_4b115abbe11f963c4aad30d03738fd4e/summary.json), [status.json](../results/native_18da7ee6e55acee7_4b115abbe11f963c4aad30d03738fd4e/status.json)
- [native_18da7ef22cb36762_65b83ddf84e820d545d776bb4f261823](../results/native_18da7ef22cb36762_65b83ddf84e820d545d776bb4f261823) — [summary.json](../results/native_18da7ef22cb36762_65b83ddf84e820d545d776bb4f261823/summary.json), [status.json](../results/native_18da7ef22cb36762_65b83ddf84e820d545d776bb4f261823/status.json)
- [native_18da7f012e5f3e98_a23954294ff81f19cae95855ea76f6d2](../results/native_18da7f012e5f3e98_a23954294ff81f19cae95855ea76f6d2) — [summary.json](../results/native_18da7f012e5f3e98_a23954294ff81f19cae95855ea76f6d2/summary.json), [status.json](../results/native_18da7f012e5f3e98_a23954294ff81f19cae95855ea76f6d2/status.json)
- [native_18da7f3456226ebf_d34c0c7a963e6d880b2ce3c0f0ed55f5](../results/native_18da7f3456226ebf_d34c0c7a963e6d880b2ce3c0f0ed55f5) — [summary.json](../results/native_18da7f3456226ebf_d34c0c7a963e6d880b2ce3c0f0ed55f5/summary.json), [status.json](../results/native_18da7f3456226ebf_d34c0c7a963e6d880b2ce3c0f0ed55f5/status.json)
- [native_18da7f37fcfb35af_4accac5730886bd346e16e01c1ee9474](../results/native_18da7f37fcfb35af_4accac5730886bd346e16e01c1ee9474) — [summary.json](../results/native_18da7f37fcfb35af_4accac5730886bd346e16e01c1ee9474/summary.json), [status.json](../results/native_18da7f37fcfb35af_4accac5730886bd346e16e01c1ee9474/status.json)
- [native_18da7f3c02fedbb3_d66922459404ac9b4406ee23fb2a72f7](../results/native_18da7f3c02fedbb3_d66922459404ac9b4406ee23fb2a72f7) — [summary.json](../results/native_18da7f3c02fedbb3_d66922459404ac9b4406ee23fb2a72f7/summary.json), [status.json](../results/native_18da7f3c02fedbb3_d66922459404ac9b4406ee23fb2a72f7/status.json)
- [native_18da7f409488c11c_1c1968d3f4099617cb45fd2a3d5b9371](../results/native_18da7f409488c11c_1c1968d3f4099617cb45fd2a3d5b9371) — [summary.json](../results/native_18da7f409488c11c_1c1968d3f4099617cb45fd2a3d5b9371/summary.json), [status.json](../results/native_18da7f409488c11c_1c1968d3f4099617cb45fd2a3d5b9371/status.json)
- [native_18da7f43de6cbe3c_1f0a6c847af871080d90a085477f2657](../results/native_18da7f43de6cbe3c_1f0a6c847af871080d90a085477f2657) — [summary.json](../results/native_18da7f43de6cbe3c_1f0a6c847af871080d90a085477f2657/summary.json), [status.json](../results/native_18da7f43de6cbe3c_1f0a6c847af871080d90a085477f2657/status.json)
- [native_18da7f462e47fd4b_b9d2da6216b0f88a733383566f2b3090](../results/native_18da7f462e47fd4b_b9d2da6216b0f88a733383566f2b3090) — [summary.json](../results/native_18da7f462e47fd4b_b9d2da6216b0f88a733383566f2b3090/summary.json), [status.json](../results/native_18da7f462e47fd4b_b9d2da6216b0f88a733383566f2b3090/status.json)
- [native_18da7f49bb332e11_31f75aefc3beaddc95e9770157069d2c](../results/native_18da7f49bb332e11_31f75aefc3beaddc95e9770157069d2c) — [summary.json](../results/native_18da7f49bb332e11_31f75aefc3beaddc95e9770157069d2c/summary.json), [status.json](../results/native_18da7f49bb332e11_31f75aefc3beaddc95e9770157069d2c/status.json)
- [native_18da7f4dd99d9e38_98ed74bb0c58df60f4781186bdb4b8dc](../results/native_18da7f4dd99d9e38_98ed74bb0c58df60f4781186bdb4b8dc) — [summary.json](../results/native_18da7f4dd99d9e38_98ed74bb0c58df60f4781186bdb4b8dc/summary.json), [status.json](../results/native_18da7f4dd99d9e38_98ed74bb0c58df60f4781186bdb4b8dc/status.json)
- [native_18da7f5330b87a8d_6070365e557c72c9097b3c8565f3713e](../results/native_18da7f5330b87a8d_6070365e557c72c9097b3c8565f3713e) — [summary.json](../results/native_18da7f5330b87a8d_6070365e557c72c9097b3c8565f3713e/summary.json), [status.json](../results/native_18da7f5330b87a8d_6070365e557c72c9097b3c8565f3713e/status.json)
- [native_18da7f56e8bc5e13_f17ec541e60bccb69e8431c57e3c765a](../results/native_18da7f56e8bc5e13_f17ec541e60bccb69e8431c57e3c765a) — [summary.json](../results/native_18da7f56e8bc5e13_f17ec541e60bccb69e8431c57e3c765a/summary.json), [status.json](../results/native_18da7f56e8bc5e13_f17ec541e60bccb69e8431c57e3c765a/status.json)
- [native_18da7f5a4a28d4b8_8aed6d871c7e04ad5d0a84f60cc0c627](../results/native_18da7f5a4a28d4b8_8aed6d871c7e04ad5d0a84f60cc0c627) — [summary.json](../results/native_18da7f5a4a28d4b8_8aed6d871c7e04ad5d0a84f60cc0c627/summary.json), [status.json](../results/native_18da7f5a4a28d4b8_8aed6d871c7e04ad5d0a84f60cc0c627/status.json)
- [native_18da7f5d928360e4_d6ff639fc2c3a82e8c74311dab6519f8](../results/native_18da7f5d928360e4_d6ff639fc2c3a82e8c74311dab6519f8) — [summary.json](../results/native_18da7f5d928360e4_d6ff639fc2c3a82e8c74311dab6519f8/summary.json), [status.json](../results/native_18da7f5d928360e4_d6ff639fc2c3a82e8c74311dab6519f8/status.json)
- [native_18da7f62a0a313ed_2913235edfababb6223ad75df523ed71](../results/native_18da7f62a0a313ed_2913235edfababb6223ad75df523ed71) — [summary.json](../results/native_18da7f62a0a313ed_2913235edfababb6223ad75df523ed71/summary.json), [status.json](../results/native_18da7f62a0a313ed_2913235edfababb6223ad75df523ed71/status.json)
- [native_18da7f68766267d1_04af1007034d877ac51c8272450e16b2](../results/native_18da7f68766267d1_04af1007034d877ac51c8272450e16b2) — [summary.json](../results/native_18da7f68766267d1_04af1007034d877ac51c8272450e16b2/summary.json), [status.json](../results/native_18da7f68766267d1_04af1007034d877ac51c8272450e16b2/status.json)
- [native_18da7f6b8bbb097f_07e9f33d673b1b7ad51c45c78fd37fa0](../results/native_18da7f6b8bbb097f_07e9f33d673b1b7ad51c45c78fd37fa0) — [summary.json](../results/native_18da7f6b8bbb097f_07e9f33d673b1b7ad51c45c78fd37fa0/summary.json), [status.json](../results/native_18da7f6b8bbb097f_07e9f33d673b1b7ad51c45c78fd37fa0/status.json)
- [native_18da7f77de09886d_9f95eb2a7ffdf0d34edb50d0d2ea0653](../results/native_18da7f77de09886d_9f95eb2a7ffdf0d34edb50d0d2ea0653) — [summary.json](../results/native_18da7f77de09886d_9f95eb2a7ffdf0d34edb50d0d2ea0653/summary.json), [status.json](../results/native_18da7f77de09886d_9f95eb2a7ffdf0d34edb50d0d2ea0653/status.json)
- [native_18da7f7fa25dc3e8_9bdf3774ec34d6576e5739ad65d44930](../results/native_18da7f7fa25dc3e8_9bdf3774ec34d6576e5739ad65d44930) — [summary.json](../results/native_18da7f7fa25dc3e8_9bdf3774ec34d6576e5739ad65d44930/summary.json), [status.json](../results/native_18da7f7fa25dc3e8_9bdf3774ec34d6576e5739ad65d44930/status.json)
- [native_18da7fb681e74597_be07ebdcf059aa4592fec532beb5b9bb](../results/native_18da7fb681e74597_be07ebdcf059aa4592fec532beb5b9bb) — [summary.json](../results/native_18da7fb681e74597_be07ebdcf059aa4592fec532beb5b9bb/summary.json), [status.json](../results/native_18da7fb681e74597_be07ebdcf059aa4592fec532beb5b9bb/status.json)
- [reports](../results/reports)
- [sweep_18db5f797a5352ae_fd8a3e0c99d6e6ebf6977230c15b859e](../results/sweep_18db5f797a5352ae_fd8a3e0c99d6e6ebf6977230c15b859e) — [summary.json](../results/sweep_18db5f797a5352ae_fd8a3e0c99d6e6ebf6977230c15b859e/summary.json)
- [sweep_18db5fc511f1f0c4_01ee626edc817719c8317cecff24588a](../results/sweep_18db5fc511f1f0c4_01ee626edc817719c8317cecff24588a) — [summary.json](../results/sweep_18db5fc511f1f0c4_01ee626edc817719c8317cecff24588a/summary.json)
- [time_multiplexing](../results/time_multiplexing)

## The proposed 10-WAV-per-language comparison

Use 10 **unique heldout recordings** in each language (30 source recordings total), with the exact same set in every layout. Report each language separately, with per-WAV duration, transcripts, WER/CER, first text, final time, EOF delay, queue/decode timings, CPU/RSS/PSS, completion/failure and throughput. Use corpus WER/CER (total edits / total reference units), rather than the mean of per-WAV error percentages.

Keep clean and telephone audio in separate condition tables. Keep single-session and multi-session policies explicit; retain the shared single-session control. For overlapping calls, use distinct WAVs as well as the existing same-WAV routing check. Repetitions can measure run variability but must not be counted as additional unique recordings. Ten recordings per language is a broader diagnostic sample, and still does not establish reliable p95/p99 or production capacity.

This experiment has not been run. The current comparison script selects one recording per language; increasing `--calls-per-wav` repeats those recordings and does not select 10 different WAVs. The new shared-pool corpus runner supports dataset selection and multi-input scheduling; see the linked guide. The historical script remains a same-WAV diagnostic.
