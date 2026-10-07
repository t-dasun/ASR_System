# CPU capacity stress report

Main C++ service and WAV simulator; Python orchestrates and scores. No latency pass/fail target.

Same 10 unique WAVs per language, 3 measured repetitions per curve point. Larger targets replay full balanced cohorts to fill concurrency. Languages are separate; failures are retained.

Resumable per-call encoder/decoder state shares model weights. Each worker runs one ready decode step at a time and requeues the call. No multi-call tensor batching. Decode step 2 seconds, zero initially withheld chunks, final whole-audio refinement disabled. Four compute threads per worker. Model weights are unchanged.

Runtime: `qwen_stream`. Config: `configs/qwen_stream_shared.yaml`.

## Worker admission and idle cost

| Workers | Slots/worker | Status | Idle RSS GiB | Idle PSS GiB | Reason |
|---:|---:|---|---:|---:|---|
| 1 | 1 | MEASURED | 1.736 | 1.269 |  |
| 1 | 2 | MEASURED | 1.737 | 1.269 |  |
| 1 | 3 | MEASURED | 1.737 | 1.269 |  |
| 1 | 4 | MEASURED | 1.738 | 1.269 |  |
| 1 | 5 | MEASURED | 1.737 | 1.269 |  |
| 1 | 6 | MEASURED | 1.737 | 1.269 |  |
| 1 | 7 | MEASURED | 1.737 | 1.271 |  |
| 1 | 8 | MEASURED | 1.736 | 1.271 |  |
| 2 | 1 | MEASURED | 3.465 | 2.413 |  |
| 2 | 2 | MEASURED | 3.464 | 2.413 |  |
| 2 | 4 | MEASURED | 3.465 | 2.413 |  |
| 2 | 8 | MEASURED | 3.463 | 2.413 |  |

## en

| Workers | Slots/worker | Concurrency | Complete | Failure % | First text mean/p95 s | EOF mean/p95 s | Audio s/wall s | CPU mean/peak cores | Peak PSS GiB | Completed WER/CER % | Failure-inclusive WER/CER % |
|---:|---:|---:|---:|---:|---|---|---:|---|---:|---:|---:|
| 1 | 1 | 1 | 30/30 | 0.0 | 2.678 / 2.821 | 1.033 / 1.556 | 0.885 | 1.915 / 4.688 | 1.692 | 10.244 | 10.244 |
| 1 | 2 | 2 | 30/30 | 0.0 | 2.830 / 3.384 | 1.971 / 3.558 | 1.537 | 3.230 / 4.657 | 1.783 | 10.244 | 10.244 |
| 1 | 3 | 3 | 30/30 | 0.0 | 3.285 / 4.602 | 4.741 / 8.956 | 1.812 | 3.770 / 4.690 | 1.869 | 10.244 | 10.244 |
| 1 | 4 | 4 | 30/30 | 0.0 | 3.628 / 4.928 | 8.069 / 11.316 | 1.888 | 3.915 / 4.642 | 1.938 | 10.244 | 10.244 |
| 1 | 5 | 5 | 30/30 | 0.0 | 4.834 / 6.108 | 11.225 / 15.955 | 1.918 | 3.951 / 4.749 | 1.982 | 10.244 | 10.244 |
| 1 | 6 | 6 | 30/30 | 0.0 | 5.312 / 7.333 | 14.383 / 20.917 | 1.907 | 3.956 / 4.703 | 2.047 | 10.244 | 10.244 |
| 1 | 7 | 7 | 30/30 | 0.0 | 4.853 / 7.191 | 11.589 / 17.259 | 2.497 | 3.877 / 4.627 | 2.104 | 10.244 | 10.244 |
| 1 | 8 | 8 | 30/30 | 0.0 | 5.178 / 10.102 | 15.469 / 21.489 | 2.487 | 3.869 / 4.683 | 2.176 | 10.244 | 10.244 |
| 2 | 1 | 2 | 30/30 | 0.0 | 2.765 / 3.061 | 1.282 / 1.966 | 1.647 | 4.164 / 9.240 | 3.122 | 10.244 | 10.244 |
| 2 | 2 | 4 | 30/30 | 0.0 | 3.289 / 4.371 | 4.119 / 6.509 | 2.449 | 6.871 / 9.353 | 3.262 | 10.244 | 10.244 |
| 2 | 4 | 8 | 30/30 | 0.0 | 5.127 / 7.610 | 14.611 / 21.636 | 2.563 | 7.477 / 8.948 | 3.531 | 10.244 | 10.244 |
| 2 | 8 | 16 | 60/60 | 0.0 | 8.835 / 17.935 | 33.447 / 46.556 | 2.594 | 7.622 / 9.230 | 4.056 | 10.244 | 10.244 |

### Resumable decoding (completed-call means)

| Workers | Slots/worker | Stream wall ms | Stream queue ms | Steps | Reused prefill tokens | Stream invocation RTF |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 4133.247 | 0.499 | 4.600 | 125.100 | 0.496 |
| 1 | 2 | 4116.371 | 1510.376 | 4.600 | 125.100 | 0.495 |
| 1 | 3 | 4214.927 | 5086.634 | 4.600 | 125.100 | 0.506 |
| 1 | 4 | 4107.011 | 8926.991 | 4.600 | 125.100 | 0.493 |
| 1 | 5 | 4060.944 | 11705.618 | 4.600 | 125.100 | 0.488 |
| 1 | 6 | 4084.438 | 14628.641 | 4.600 | 125.100 | 0.492 |
| 1 | 7 | 3073.378 | 12761.556 | 4.600 | 125.100 | 0.370 |
| 1 | 8 | 3085.780 | 16554.608 | 4.600 | 125.100 | 0.371 |
| 2 | 1 | 4883.592 | 0.531 | 4.600 | 125.100 | 0.581 |
| 2 | 2 | 5537.927 | 3201.091 | 4.600 | 125.100 | 0.664 |
| 2 | 4 | 5849.465 | 13499.056 | 4.600 | 125.100 | 0.707 |
| 2 | 8 | 5853.686 | 30761.941 | 4.600 | 125.100 | 0.705 |

## id

| Workers | Slots/worker | Concurrency | Complete | Failure % | First text mean/p95 s | EOF mean/p95 s | Audio s/wall s | CPU mean/peak cores | Peak PSS GiB | Completed WER/CER % | Failure-inclusive WER/CER % |
|---:|---:|---:|---:|---:|---|---|---:|---|---:|---:|---:|
| 1 | 1 | 1 | 30/30 | 0.0 | 2.638 / 2.756 | 1.172 / 2.189 | 0.898 | 2.266 / 4.760 | 1.725 | 35.196 | 35.196 |
| 1 | 2 | 2 | 30/30 | 0.0 | 2.801 / 3.422 | 3.792 / 5.230 | 1.461 | 3.604 / 4.724 | 1.804 | 35.196 | 35.196 |
| 1 | 3 | 3 | 30/30 | 0.0 | 3.575 / 5.312 | 8.637 / 11.912 | 1.545 | 3.811 / 4.851 | 1.892 | 35.196 | 35.196 |
| 1 | 4 | 4 | 30/30 | 0.0 | 4.482 / 6.249 | 14.066 / 18.896 | 1.626 | 3.996 / 4.787 | 1.937 | 35.196 | 35.196 |
| 1 | 5 | 5 | 30/30 | 0.0 | 4.832 / 7.205 | 19.651 / 22.865 | 1.626 | 4.009 / 4.791 | 2.002 | 35.196 | 35.196 |
| 1 | 6 | 6 | 30/30 | 0.0 | 5.382 / 9.465 | 22.924 / 29.975 | 1.632 | 4.012 / 4.815 | 2.061 | 35.196 | 35.196 |
| 1 | 7 | 7 | 30/30 | 0.0 | 4.610 / 6.731 | 20.485 / 28.889 | 2.011 | 3.962 / 4.822 | 2.160 | 35.196 | 35.196 |
| 1 | 8 | 8 | 30/30 | 0.0 | 4.876 / 7.834 | 25.539 / 34.201 | 1.988 | 3.960 / 4.708 | 2.233 | 35.196 | 35.196 |
| 2 | 1 | 2 | 30/30 | 0.0 | 2.838 / 3.060 | 1.994 / 3.427 | 1.672 | 5.626 / 9.323 | 3.111 | 35.196 | 35.196 |
| 2 | 2 | 4 | 30/30 | 0.0 | 3.416 / 4.144 | 8.092 / 11.430 | 2.153 | 7.492 / 9.212 | 3.286 | 35.196 | 35.196 |
| 2 | 4 | 8 | 30/30 | 0.0 | 4.837 / 7.493 | 23.145 / 31.012 | 2.072 | 7.172 / 9.287 | 3.581 | 35.196 | 35.196 |
| 2 | 8 | 16 | 60/60 | 0.0 | 8.988 / 21.665 | 53.679 / 73.217 | 2.089 | 7.586 / 9.374 | 4.157 | 35.196 | 35.196 |

### Resumable decoding (completed-call means)

| Workers | Slots/worker | Stream wall ms | Stream queue ms | Steps | Reused prefill tokens | Stream invocation RTF |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 6301.724 | 0.632 | 5.700 | 114.700 | 0.607 |
| 1 | 2 | 6289.766 | 3820.114 | 5.700 | 114.700 | 0.607 |
| 1 | 3 | 6374.069 | 9081.485 | 5.700 | 114.700 | 0.615 |
| 1 | 4 | 6274.286 | 14580.863 | 5.700 | 114.700 | 0.606 |
| 1 | 5 | 6275.585 | 20190.711 | 5.700 | 114.700 | 0.606 |
| 1 | 6 | 6252.674 | 23626.994 | 5.700 | 114.700 | 0.603 |
| 1 | 7 | 5036.513 | 22025.686 | 5.700 | 114.700 | 0.488 |
| 1 | 8 | 5098.490 | 26508.115 | 5.700 | 114.700 | 0.495 |
| 2 | 1 | 8557.329 | 0.809 | 5.700 | 114.700 | 0.828 |
| 2 | 2 | 8920.062 | 6324.285 | 5.700 | 114.700 | 0.861 |
| 2 | 4 | 8865.585 | 20707.122 | 5.700 | 114.700 | 0.856 |
| 2 | 8 | 9351.419 | 49339.527 | 5.700 | 114.700 | 0.911 |

## zh

| Workers | Slots/worker | Concurrency | Complete | Failure % | First text mean/p95 s | EOF mean/p95 s | Audio s/wall s | CPU mean/peak cores | Peak PSS GiB | Completed WER/CER % | Failure-inclusive WER/CER % |
|---:|---:|---:|---:|---:|---|---|---:|---|---:|---:|---:|
| 1 | 1 | 1 | 30/30 | 0.0 | 2.546 / 2.643 | 0.892 / 1.076 | 0.926 | 1.864 / 4.890 | 1.749 | 12.426 | 12.426 |
| 1 | 2 | 2 | 30/30 | 0.0 | 2.706 / 3.010 | 1.294 / 2.200 | 1.694 | 3.269 / 4.825 | 1.810 | 12.426 | 12.426 |
| 1 | 3 | 3 | 30/30 | 0.0 | 3.133 / 4.325 | 4.240 / 6.300 | 2.060 | 3.920 / 4.809 | 1.880 | 12.426 | 12.426 |
| 1 | 4 | 4 | 30/30 | 0.0 | 4.182 / 5.499 | 8.977 / 12.514 | 2.121 | 4.021 / 4.891 | 1.935 | 12.426 | 12.426 |
| 1 | 5 | 5 | 30/30 | 0.0 | 4.544 / 5.949 | 12.772 / 17.877 | 2.134 | 4.060 / 4.820 | 2.001 | 12.426 | 12.426 |
| 1 | 6 | 6 | 30/30 | 0.0 | 4.923 / 7.197 | 16.925 / 25.974 | 2.123 | 4.058 / 4.869 | 2.093 | 12.426 | 12.426 |
| 1 | 7 | 7 | 30/30 | 0.0 | 4.090 / 5.881 | 11.803 / 18.650 | 2.895 | 3.960 / 4.733 | 2.109 | 12.426 | 12.426 |
| 1 | 8 | 8 | 30/30 | 0.0 | 4.232 / 7.162 | 16.311 / 21.962 | 2.817 | 3.923 / 4.864 | 2.226 | 12.426 | 12.426 |
| 2 | 1 | 2 | 30/30 | 0.0 | 2.659 / 2.818 | 1.070 / 1.488 | 1.711 | 3.983 / 9.183 | 3.124 | 12.426 | 12.426 |
| 2 | 2 | 4 | 30/30 | 0.0 | 3.270 / 3.851 | 3.503 / 4.843 | 2.949 | 7.487 / 9.401 | 3.287 | 12.426 | 12.426 |
| 2 | 4 | 8 | 30/30 | 0.0 | 4.206 / 5.864 | 14.108 / 20.802 | 3.052 | 7.641 / 9.558 | 3.545 | 12.426 | 12.426 |
| 2 | 8 | 16 | 60/60 | 0.0 | 6.889 / 12.714 | 36.093 / 52.753 | 2.924 | 7.539 / 9.923 | 4.110 | 12.426 | 12.426 |

### Resumable decoding (completed-call means)

| Workers | Slots/worker | Stream wall ms | Stream queue ms | Steps | Reused prefill tokens | Stream invocation RTF |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 5272.566 | 0.715 | 6.100 | 295.500 | 0.459 |
| 1 | 2 | 5213.382 | 1436.818 | 6.100 | 295.500 | 0.454 |
| 1 | 3 | 5225.888 | 6712.073 | 6.100 | 295.500 | 0.455 |
| 1 | 4 | 5195.023 | 11596.864 | 6.100 | 295.500 | 0.453 |
| 1 | 5 | 5164.539 | 15226.756 | 6.100 | 295.500 | 0.450 |
| 1 | 6 | 5191.893 | 19709.600 | 6.100 | 295.500 | 0.452 |
| 1 | 7 | 3754.888 | 15605.682 | 6.100 | 295.500 | 0.328 |
| 1 | 8 | 3865.752 | 19920.190 | 6.100 | 295.500 | 0.338 |
| 2 | 1 | 6177.434 | 0.685 | 6.100 | 295.500 | 0.535 |
| 2 | 2 | 6952.541 | 4338.879 | 6.100 | 295.500 | 0.605 |
| 2 | 4 | 6892.047 | 15481.492 | 6.100 | 295.500 | 0.603 |
| 2 | 8 | 7048.753 | 35856.353 | 6.100 | 295.500 | 0.621 |

## Observed throughput maxima (no latency acceptance target)

| Language | Workers | Concurrency | Audio s/wall s | Failure % |
|---|---:|---:|---:|---:|
| en | 2 | 16 | 2.594 | 0.0 |
| id | 2 | 4 | 2.153 | 0.0 |
| zh | 2 | 8 | 3.052 | 0.0 |

## Mixed-language network and sustained tests

| Workers | Concurrency | Mode | Language | Complete | First text mean/p95 s | EOF mean/p95 s | Completed WER/CER % | Failure-inclusive WER/CER % |
|---:|---:|---|---|---:|---|---|---:|---:|
| 1 | 8 | network | en | 10/10 | 6.365 / 8.861 | 21.560 / 30.193 | 10.244 | 10.244 |
| 1 | 8 | network | id | 10/10 | 5.770 / 7.463 | 23.174 / 29.210 | 35.196 | 35.196 |
| 1 | 8 | network | zh | 10/10 | 6.087 / 8.580 | 22.583 / 35.621 | 12.426 | 12.426 |
| 2 | 16 | network | en | 8/10 | 9.119 / 19.514 | 39.170 / 52.640 | 11.585 | 29.268 |
| 2 | 16 | network | id | 9/10 | 10.200 / 20.636 | 44.701 / 58.184 | 31.288 | 37.430 |
| 2 | 16 | network | zh | 8/10 | 9.598 / 12.834 | 45.199 / 70.970 | 12.500 | 27.515 |

These are maximum observed throughput points, not maximum usable capacity. No production latency/quality SLO was selected.

## Scope and interpretation

- First text can be partial or final; EOF delay is worker EOF receipt to final publication. Completed-call timing distributions exclude failed calls. Primary WER/CER uses completed calls only: total substitutions + deletions + insertions divided by their total reference words (WER) or characters (CER). Failures are reported separately; supplementary failure-inclusive accuracy uses empty hypotheses for failures. Completion is a protocol outcome, not an accuracy threshold. Excluding failures can make accuracy look better when difficult files fail.
- Effective RTF includes audio pacing. Stream invocation wall RTF sums resumable step wall times and optional refinement, divided by input audio duration; it excludes scheduler queue waiting. Legacy offline wall RTF is unavailable for resumable mode. Neither is vendor active compute. Ten distinct files give descriptive tails, not a production p95 guarantee.
- PSS apportions shared physical pages; summed RSS double-counts some mapped weights. Host swap is system-wide and can include unrelated programs. Resource guards reserve RAM and bound swap I/O; an aborted run is not a model accuracy failure.
- Cold CLI conservative plans remain recorded. Worker services load progressively under live resource monitoring; warm suite preflight remains enforced. A failed cold estimate is not silently labelled successful cold admission.
- Maximum configured support is bounded to eight workers and eight sessions/worker. The load runner supports 64 calls so all configured slots can be exercised. Untested higher limits are not hardware capacity claims.
- Finite paced WAV calls use completion-driven replenishment. Target concurrency differs from achieved occupancy; peaks and worker occupancy are recorded. This is not a continuous telephony arrival model.
- Raw jobs, call transcripts, resource samples, runtime snapshots, guard telemetry, plans and hashes are retained beside curve.csv/curve.json.

## Exported figures

## EN capacity curves

![en capacity curves](figures/en_capacity.png)

## ID capacity curves

![id capacity curves](figures/id_capacity.png)

## ZH capacity curves

![zh capacity curves](figures/zh_capacity.png)
