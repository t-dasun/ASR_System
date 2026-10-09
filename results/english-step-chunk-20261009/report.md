# English shared-runtime decode-step / chunk-size experiment

Main C/C++ `qwen_stream` over loopback WebSocket. One worker, one active call at a time; eight session slots remain available in the shared runtime. No Python inference.

Same five distinct English WAVs, 3 measured repetitions per setting. Four model threads, 32 new tokens/step, zero initially withheld chunks, no full-audio final refinement. Each round permutes setting order using seed 42. Warmup is separate.

First text is first nonempty server result, not proof of semantic correctness. EOF delay is worker EOF receipt to final publication. Browser rendering/network impairment are not measured. WER uses completed calls only with failed calls reported separately.

| Decode step ms | PCM chunk ms | Complete | First text mean / p95 s | EOF mean / p95 s | Final mean s | Corpus WER % |
|---:|---:|---:|---|---|---:|---:|
| 2000 | 200 | 15/15 | 3.089 / 3.385 | 1.630 / 2.308 | 9.299 | 4.854 |
| 1000 | 200 | 15/15 | 1.727 / 1.864 | 2.524 / 4.214 | 10.186 | 5.825 |
| 2000 | 100 | 15/15 | 3.137 / 3.568 | 1.547 / 2.017 | 9.265 | 4.854 |
| 1000 | 100 | 15/15 | 1.731 / 1.873 | 2.425 / 4.049 | 10.131 | 5.825 |
| 500 | 100 | 15/15 | 1.101 / 1.162 | 8.042 / 12.221 | 15.751 | 12.621 |

## Per-WAV means

Each cell: **WER % / first-text seconds / EOF-to-final seconds**. Mean of completed repetitions; corpus WER above weights by reference words.

| Recording | 2000 ms / 200 ms | 1000 ms / 200 ms | 2000 ms / 100 ms | 1000 ms / 100 ms | 500 ms / 100 ms |
|---|---|---|---|---|---|
| fleurs_en_us_validation_1523_142 | 0.000 / 2.876 / 1.859 (3/3) | 0.000 / 1.661 / 2.602 (3/3) | 0.000 / 2.858 / 1.504 (3/3) | 0.000 / 1.634 / 2.630 (3/3) | 15.789 / 1.155 / 10.125 (3/3) |
| fleurs_en_us_validation_1626_141 | 15.152 / 3.297 / 2.130 (3/3) | 18.182 / 1.727 / 4.174 (3/3) | 15.152 / 3.176 / 2.016 (3/3) | 18.182 / 1.765 / 4.029 (3/3) | 21.212 / 1.134 / 12.163 (3/3) |
| fleurs_en_us_validation_1654_60 | 0.000 / 3.272 / 1.802 (3/3) | 0.000 / 1.722 / 2.867 (3/3) | 0.000 / 3.269 / 1.772 (3/3) | 0.000 / 1.770 / 2.786 (3/3) | 0.000 / 1.059 / 9.103 (3/3) |
| fleurs_en_us_validation_1607_28 | 0.000 / 3.148 / 1.085 (3/3) | 0.000 / 1.863 / 0.904 (3/3) | 0.000 / 3.532 / 1.186 (3/3) | 0.000 / 1.870 / 0.915 (3/3) | 8.333 / 1.112 / 3.644 (3/3) |
| fleurs_en_us_validation_1521_51 | 0.000 / 2.854 / 1.272 (3/3) | 0.000 / 1.664 / 2.074 (3/3) | 0.000 / 2.852 / 1.257 (3/3) | 0.000 / 1.617 / 1.765 (3/3) | 13.333 / 1.047 / 5.174 (3/3) |

## Observed fastest first text

Lowest measured mean first text: **500 ms decode / 100 ms chunks**, 1.101 s, completed WER 12.621%. This is a nonempty-output latency result; inspect final quality and EOF delay before choosing a setting.

## Scope and artifacts

Short read-speech sample: five selected files, not the full ten-file cohort or domain audio. Subsecond audio steps may increase encoder recomputation and provisional mistakes. No stable-word latency, batching gain or continuous-call capacity is claimed. Failed calls remain in calls.json and job artifacts; first/EOF/final distributions describe completions.

Raw C++ suite directories contain per-call configuration, transcript/events, audio/runtime timings and resource samples. Plan records WAV/model/binary/config/driver identities. summary.json retains timing means and p50/p95/p99; per_wav.csv provides individual file means.
