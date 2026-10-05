# Matched C++ topology comparison

See [available results by language](AVAILABLE_RESULTS_BY_LANGUAGE.md) for separate English, Indonesian and Mandarin tables, individual call readings, earlier clean/telephone results and a complete result-directory index. The aggregate latency tables below combine the three language clips; they do not describe language-specific performance.

Evidence: [full JSON matrix and 320 artifact hashes](../tools/multiplexing/matched_topology_comparison_20261005.json), [metric/percentile CSV](../tools/multiplexing/matched_topology_metrics_20261005.csv), [per-call CSV](../tools/multiplexing/matched_topology_calls_20261005.csv), and [original C++ suites](../results/time_multiplexing/matched_topologies_20261005/comparison.json). Layout names use `w` for workers and `s` for active sessions per worker. The shared single-session row is a control for the changed decode policy.

Inputs are the **same three held-out FLEURS WAVs in every layout**, verified against manifest SHA-256 values: English `fleurs_en_us_validation_1605_16` (12.58 s), Indonesian `fleurs_id_id_validation_1520_4` (15.48 s), and Mandarin `fleurs_cmn_hans_cn_validation_1559_6` (14.28 s). Each is offered twice. The main C++ runner uses 200 ms availability-based pacing, direct ingress, four threads per worker/library, BF16 weights, disabled BF16 conversion cache, 200 ms resource sampling, and EOF refinement. Native live decoding uses a 2 s step; shared prefix decoding emits one 4 s preview. Thus the control row is needed to distinguish policy changes from sharing contention.

3 unique held-out recordings, 2 repetitions each per layout; paired same-utterance loads, not sustained mixed-input capacity or robust tail estimates

All inference, paced PCM, event artifacts and timings are produced by the main C++ `asr-cli load` runner. Python only invokes commands, checks hashes, scores text and formats the report.

| Layout | Complete | Pre-EOF text | Startup mean | First text mean / p95 | Final mean / p95 | EOF delay mean | Peak RSS | Audio s / wall s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| native_1w_1s | 6/6 | 6/6 | 358.020 ms | 7.134 s / 7.200 s | 17.991 s / 19.976 s | 3.877 s | 2.67 GiB | 0.769 |
| shared_1w_1s_control | 6/6 | 6/6 | 0.005 ms | 5.016 s / 5.298 s | 16.660 s / 18.787 s | 2.546 s | 2.66 GiB | 0.847 |
| shared_1w_2s | 6/6 | 6/6 | 0.006 ms | 5.529 s / 6.463 s | 17.905 s / 21.255 s | 3.792 s | 2.66 GiB | 1.472 |
| native_2w_1s | 6/6 | 6/6 | 337.100 ms | 7.534 s / 7.800 s | 19.621 s / 21.937 s | 5.508 s | 5.32 GiB | 1.414 |

Each recording is offered 2 times per layout. Single-session cases run sequentially; concurrency-two cases overlap calls. Shared models are reused within a WAV suite; native contexts reload per call. First-text timing starts after readiness, so startup and shared context load are reported separately. Four threads are configured per worker; the two-native-worker layout therefore has eight configured compute threads in total.

| Layout | EN WER | ID WER | ZH CER | Exact final match to native single |
|---|---:|---:|---:|---:|
| native_1w_1s | 7.69% | 8.33% | 0.00% | 6/6 |
| shared_1w_1s_control | 7.69% | 8.33% | 0.00% | 6/6 |
| shared_1w_2s | 7.69% | 8.33% | 0.00% | 6/6 |
| native_2w_1s | 7.69% | 8.33% | 0.00% | 6/6 |

Accuracy has one unique utterance per language; repeating it does not increase independent accuracy coverage. WER and CER are never pooled.

Full per-call startup, first partial, final, EOF delay, scheduled final lag, queue/submit/publication distributions, CPU, RSS, transcript and accuracy matrices are in the [JSON matrix](../tools/multiplexing/matched_topology_comparison_20261005.json) and original C++ suites. Its group distributions use explicit unit fields (milliseconds for timing); raw per-call `*_ns` fields remain nanoseconds. Shared prefix/EOF queue waits and offline invocation wall time are measured explicitly. Native internal queue/active compute and stable-word timing remain unavailable. Shared model-load timing is one reused-context load, not a load repeated for each call. Native live invocation wall time includes pacing and is not comparable to offline decode wall time. Direct ingress excludes WebSocket/browser delay.

| Layout | Configured threads across workers | Sampled mean CPU cores | Sampled peak CPU cores | Peak PSS |
|---|---:|---:|---:|---:|
| native_1w_1s | 4 | 2.23 | 4.64 | 2.66 GiB |
| shared_1w_1s_control | 4 | 0.92 | 4.57 | 2.66 GiB |
| shared_1w_2s | 4 | 1.52 | 4.53 | 2.66 GiB |
| native_2w_1s | 8 | 5.67 | 9.35 | 3.85 GiB |

CPU means integrate available process-tree samples. Short-lived-process usage and between-sample peaks may be missed; per-call native worker CPU is recorded separately. The configuration is a per-library compute-thread budget, not an OS CPU quota; samples include runtime/transport threads and sampling granularity.

| Timing mean, ms | native_1w_1s | shared_1w_1s_control | shared_1w_2s | native_2w_1s |
|---|---:|---:|---:|---:|
| startup_ns | 358.020 | 0.005 | 0.006 | 337.100 |
| first_partial_ns | 7133.585 | 5015.856 | 5529.227 | 7533.621 |
| first_usable_transcript_ns | 7133.585 | 5015.856 | 5529.227 | 7533.621 |
| final_result_ns | 17990.768 | 16659.919 | 17905.419 | 19621.341 |
| finalization_ns | 3877.053 | 2546.287 | 3791.905 | 5507.723 |
| scheduled_final_lag_ns | 3877.434 | 2546.586 | 3792.086 | 5508.008 |
| model_load_ns | 352.950 | unavailable | unavailable | 332.151 |
| shared_model_load_ns | unavailable | 339.957 | 338.025 | unavailable |
| live_invocation_wall_ns | 15398.546 | unavailable | unavailable | 16009.448 |
| prefix_decode_wall_ns | unavailable | 1015.482 | 1009.118 | unavailable |
| eof_refinement_wall_ns | 2498.833 | 2546.086 | 2530.212 | 3525.568 |
| prefix_decode_queue_wait_ns | unavailable | 0.180 | 519.895 | unavailable |
| eof_decode_queue_wait_ns | unavailable | 0.137 | 1261.631 | unavailable |
| runtime_queue_wait_ns | unavailable | 0.318 | 1781.526 | unavailable |
| offline_decode_wall_ns | unavailable | 3561.567 | 3539.329 | unavailable |
| worker_cpu_ns | 38604.403 | unavailable | unavailable | 53999.718 |
| first_stable_transcript_ns | unavailable | unavailable | unavailable | unavailable |
| first_inference_compute_ns | unavailable | unavailable | unavailable | unavailable |
| partial_service_lag_ns | unavailable | unavailable | unavailable | unavailable |

| Chunk/event delay p95, ms | native_1w_1s | shared_1w_1s_control | shared_1w_2s | native_2w_1s |
|---|---:|---:|---:|---:|
| send_lag | 0.337 | 0.329 | 0.141 | 0.196 |
| controller_queue_wait | 0.002 | 0.002 | 0.002 | 0.003 |
| submit_wall | 0.321 | 0.073 | 0.097 | 0.354 |
| publication_delay | 183.831 | 0.071 | 0.072 | 190.435 |

Chunk/event distributions pool raw durations, never average per-call percentiles. Publication delay uses controller receipt before artifact append and includes IPC for native workers. The native path emits more revisions than the one-preview shared policy, so event counts and live-invocation wall time have different populations/semantics.

## Interpretation and reproduction

All **24/24 calls** completed, passed hash/sample/worker-topology/isolation checks, and delivered useful text before their own EOF. Every final matched the native single-session reference for the same WAV. Accuracy parity on three utterances is a functional screen, not a population WER/CER result. Six calls per layout cannot support robust p95/p99 or sustained capacity conclusions; the JSON/CSV retain those descriptive percentiles with counts.

Sharing two sessions rather than one in the same prefix engine added about **0.52 s mean preview queue wait** and **1.26 s mean EOF queue wait**. The offline decode work remained about 3.54 s per call. The shared single-session control had near-zero ready-job wait. The earlier first text relative to native live decoding is partly a policy difference; the shared path gives one provisional preview, while native live decoding emits many more revisions.

Two native workers doubled sampled model memory (5.32 GiB RSS versus 2.67 GiB for one native worker) and used a higher total thread budget. Shared two-session RSS remained 2.66 GiB. Compare CPU/PSS and finalization as well as first text. Phase throughput excludes shared context creation before the phase; the JSON also records whole-CLI wall time and corresponding throughput, including that setup. OS cache/thermal state were not controlled; execution order was shuffled with seed 42.

A **four-native-worker** case was RAM-preflight rejected: 14.0 GiB required versus approximately 8.18 GiB available. Its plan is included in the JSON matrix. No four-worker inference latency or accuracy is claimed.

Reproduce the same screen from the repository root, with other model services stopped:

```bash
cmake --build --preset release-cpu
python3 tools/multiplexing/compare_topologies.py \
  --output results/time_multiplexing/my_matched_topologies --calls-per-wav 2
```

The command creates `plan.json`, `jobs.json`, `comparison.json`, `comparison.md`, `metrics.csv`, `calls.csv`, and per-layout/per-WAV original C++ suite directories. To regenerate derivative matrices from existing results:

```bash
python3 tools/multiplexing/compare_topologies.py \
  --output results/time_multiplexing/my_matched_topologies --report-only
```

## RTF matrix

| Layout | Effective RTF mean / p95 | Offline decode wall RTF mean |
|---|---:|---:|
| native_1w_1s | 1.275 / 1.290 | unavailable |
| shared_1w_1s_control | 1.180 / 1.214 | 0.252 |
| shared_1w_2s | 1.268 / 1.413 | 0.250 |
| native_2w_1s | 1.391 / 1.431 | unavailable |

Effective RTF includes pacing and finalization. Offline decode wall RTF sums the shared preview and EOF invocation durations and divides by unique input duration; it excludes ready-job queue wait and pacing, but is still wall time rather than vendor active compute. Native active compute RTF remains unavailable.

Validation: the final native build passed 16/16 CTest cases, including timing-definition tests. The 320 source artifacts matched the recorded hashes, and the tested CLI matched its sealed binary hash. All work remains uncommitted.
