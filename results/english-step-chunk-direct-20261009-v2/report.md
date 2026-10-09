# English shared-runtime decode-step / chunk-size experiment

Main C/C++ `qwen_stream`, direct mode. One worker, one active call at a time; eight session slots remain available in the shared runtime. No Python inference.

Same five distinct English WAVs, 3 measured repetitions per setting. Four model threads, 32 new tokens/step, zero initially withheld chunks, no full-audio final refinement. Each round permutes setting order using seed 42. Warmup is separate.

First text is first nonempty server result, not proof of semantic correctness. EOF delay is worker EOF receipt to final publication. Browser rendering/network impairment are not measured. WER uses completed calls only with failed calls reported separately.

| Decode step ms | PCM chunk ms | Complete | First text mean / p95 s | EOF mean / p95 s | Final mean s | Corpus WER % |
|---:|---:|---:|---|---|---:|---:|
| 2000 | 200 | 15/15 | 3.035 / 3.468 | 1.627 / 2.118 | 9.179 | 4.854 |
| 1000 | 200 | 15/15 | 1.655 / 1.809 | 2.316 / 3.871 | 9.869 | 5.825 |
| 2000 | 100 | 15/15 | 2.967 / 3.122 | 1.609 / 2.311 | 9.161 | 4.854 |
| 1000 | 100 | 15/15 | 1.661 / 1.831 | 2.231 / 3.516 | 9.783 | 5.825 |
| 500 | 100 | 15/15 | 1.038 / 1.129 | 7.651 / 12.074 | 15.203 | 12.621 |

## Per-WAV means

Each cell: **WER % / first-text seconds / EOF-to-final seconds**. Mean of completed repetitions; corpus WER above weights by reference words.

| Recording | 2000 ms / 200 ms | 1000 ms / 200 ms | 2000 ms / 100 ms | 1000 ms / 100 ms | 500 ms / 100 ms |
|---|---|---|---|---|---|
| fleurs_en_us_validation_1523_142 | 0.000 / 2.782 / 1.686 (3/3) | 0.000 / 1.567 / 2.370 (3/3) | 0.000 / 2.777 / 1.897 (3/3) | 0.000 / 1.588 / 2.551 (3/3) | 15.789 / 1.121 / 10.380 (3/3) |
| fleurs_en_us_validation_1626_141 | 15.152 / 3.083 / 2.068 (3/3) | 18.182 / 1.669 / 3.862 (3/3) | 15.152 / 3.059 / 2.133 (3/3) | 18.182 / 1.686 / 3.510 (3/3) | 21.212 / 1.060 / 11.783 (3/3) |
| fleurs_en_us_validation_1654_60 | 0.000 / 3.212 / 1.989 (3/3) | 0.000 / 1.692 / 2.716 (3/3) | 0.000 / 3.115 / 1.806 (3/3) | 0.000 / 1.656 / 2.451 (3/3) | 0.000 / 1.005 / 8.399 (3/3) |
| fleurs_en_us_validation_1607_28 | 0.000 / 3.279 / 1.128 (3/3) | 0.000 / 1.794 / 0.905 (3/3) | 0.000 / 3.115 / 0.998 (3/3) | 0.000 / 1.826 / 0.878 (3/3) | 8.333 / 1.030 / 2.783 (3/3) |
| fleurs_en_us_validation_1521_51 | 0.000 / 2.821 / 1.262 (3/3) | 0.000 / 1.555 / 1.728 (3/3) | 0.000 / 2.768 / 1.209 (3/3) | 0.000 / 1.551 / 1.766 (3/3) | 13.333 / 0.975 / 4.911 (3/3) |

## Observed fastest first text

Lowest measured mean first text: **500 ms decode / 100 ms chunks**, 1.038 s, completed WER 12.621%. This is a nonempty-output latency result; inspect final quality and EOF delay before choosing a setting.

## Scope and artifacts

Short read-speech sample: five selected files, not the full ten-file cohort or domain audio. Subsecond audio steps may increase encoder recomputation and provisional mistakes. No stable-word latency, batching gain or continuous-call capacity is claimed. Failed calls remain in calls.json and job artifacts; first/EOF/final distributions describe completions.

Raw C++ suite directories contain per-call configuration, transcript/events, audio/runtime timings and resource samples. Plan records WAV/model/binary/config/driver identities. summary.json retains timing means and p50/p95/p99; per_wav.csv provides individual file means.
