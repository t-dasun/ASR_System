# English shared-runtime decode-step / chunk-size experiment

Main C/C++ `qwen_stream`, direct mode. One worker, one active call at a time; eight session slots remain available in the shared runtime. No Python inference.

Same five distinct English WAVs, 3 measured repetitions per setting. Four model threads, 32 new tokens/step, zero initially withheld chunks, no full-audio final refinement. Each round permutes setting order using seed 42. Warmup is separate.

First text is first nonempty server result, not proof of semantic correctness. EOF delay is worker EOF receipt to final publication. Browser rendering/network impairment are not measured. WER uses completed calls only with failed calls reported separately.

| Decode step ms | PCM chunk ms | Complete | First text mean / p95 s | EOF mean / p95 s | Final mean s | Corpus WER % |
|---:|---:|---:|---|---|---:|---:|
| 2000 | 200 | 15/15 | 2.543 / 2.639 | 0.826 / 1.115 | 8.378 | 4.854 |
| 1000 | 200 | 15/15 | 1.361 / 1.457 | 0.783 / 1.037 | 8.336 | 5.825 |
| 2000 | 100 | 15/15 | 2.537 / 2.639 | 0.825 / 1.117 | 8.377 | 4.854 |
| 1000 | 100 | 15/15 | 1.378 / 1.461 | 0.787 / 1.063 | 8.340 | 5.825 |
| 500 | 100 | 15/15 | 0.787 / 0.810 | 1.633 / 2.459 | 9.186 | 12.621 |

## Per-WAV means

Each cell: **WER % / first-text seconds / EOF-to-final seconds**. Mean of completed repetitions; corpus WER above weights by reference words.

| Recording | 2000 ms / 200 ms | 1000 ms / 200 ms | 2000 ms / 100 ms | 1000 ms / 100 ms | 500 ms / 100 ms |
|---|---|---|---|---|---|
| fleurs_en_us_validation_1523_142 | 0.000 / 2.434 / 0.749 (3/3) | 0.000 / 1.304 / 0.693 (3/3) | 0.000 / 2.431 / 0.759 (3/3) | 0.000 / 1.366 / 0.714 (3/3) | 15.789 / 0.804 / 1.753 (3/3) |
| fleurs_en_us_validation_1626_141 | 15.152 / 2.599 / 1.114 (3/3) | 18.182 / 1.365 / 1.032 (3/3) | 15.152 / 2.593 / 1.108 (3/3) | 18.182 / 1.376 / 1.058 (3/3) | 21.212 / 0.797 / 2.462 (3/3) |
| fleurs_en_us_validation_1654_60 | 0.000 / 2.639 / 1.004 (3/3) | 0.000 / 1.377 / 0.970 (3/3) | 0.000 / 2.639 / 1.000 (3/3) | 0.000 / 1.390 / 0.989 (3/3) | 0.000 / 0.768 / 1.778 (3/3) |
| fleurs_en_us_validation_1607_28 | 0.000 / 2.601 / 0.584 (3/3) | 0.000 / 1.450 / 0.528 (3/3) | 0.000 / 2.587 / 0.582 (3/3) | 0.000 / 1.451 / 0.510 (3/3) | 8.333 / 0.801 / 0.600 (3/3) |
| fleurs_en_us_validation_1521_51 | 0.000 / 2.442 / 0.680 (3/3) | 0.000 / 1.309 / 0.693 (3/3) | 0.000 / 2.436 / 0.675 (3/3) | 0.000 / 1.308 / 0.666 (3/3) | 13.333 / 0.768 / 1.575 (3/3) |

## Observed fastest first text

Lowest measured mean first text: **500 ms decode / 100 ms chunks**, 0.787 s, completed WER 12.621%. This is a nonempty-output latency result; inspect final quality and EOF delay before choosing a setting.

## Scope and artifacts

Short read-speech sample: five selected files, not the full ten-file cohort or domain audio. Subsecond audio steps may increase encoder recomputation and provisional mistakes. No stable-word latency, batching gain or continuous-call capacity is claimed. Failed calls remain in calls.json and job artifacts; first/EOF/final distributions describe completions.

Raw C++ suite directories contain per-call configuration, transcript/events, audio/runtime timings and resource samples. Plan records WAV/model/binary/config/driver identities. summary.json retains timing means and p50/p95/p99; per_wav.csv provides individual file means.
