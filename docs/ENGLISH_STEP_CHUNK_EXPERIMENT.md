# English decode-step and PCM-chunk experiment

This focused experiment uses the main C/C++ `qwen_stream` shared runtime with **one worker and one active call**. The worker retains eight session slots, so the shared multi-call path is selected, but calls execute sequentially for this study.

| Decode step | PCM chunk |
|---:|---:|
| 2,000 ms | 200 ms |
| 1,000 ms | 200 ms |
| 2,000 ms | 100 ms |
| 1,000 ms | 100 ms |
| 500 ms | 100 ms |

The same first five distinct English WAVs from the pinned FLEURS validation manifest are used in every setting. Their references, identities and hashes are saved in the output manifest. Defaults: three repetitions per WAV/setting, 75 measured calls plus one separate warmup. Setting order is shuffled within each repetition round using seed 42 to reduce fixed-order bias.

Fixed inference policy: Qwen3-ASR-0.6B, CPU, four compute threads, 32 maximum new tokens per step, zero initial withheld chunks and no full-audio final refinement. `--mode network` (default) exercises loopback WebSockets; `--mode direct` sends PCM directly to the engine through C++ suite jobs. Python only orchestrates and scores.

```bash
cmake --build --preset release-cpu
python3 tools/testing/run_english_step_chunk_sweep.py \
  --output results/english-step-chunk-new --repetitions 3
```

The driver owns and stops its service on a dynamic port. A fresh output directory is required. `--plan-only` validates input selection and writes the schedule without inference. `--report-only` regenerates tables from an existing directory:

```bash
python3 tools/testing/run_english_step_chunk_sweep.py \
  --output results/english-step-chunk-20261009 --report-only
```

Outputs: `report.md` (setting-level and five-row per-WAV tables), `summary.json` (accuracy and timing mean/p50/p95/p99), `per_wav.csv`, `calls.json`, `jobs.json`, model/input/binary/config identities, per-suite configurations/transcripts/events/timings/resource samples and checksums. First nonempty server text, final publication and worker EOF-to-final delay are separate metrics. Timing summaries and primary WER use completions; failures remain visible. The first nonempty result may be a wrong filler, so faster first text alone is not a quality improvement.

The new 500 ms lower bound applies only to the resumable runtime in configuration, native-state construction, session admission and WebSocket validation. The native and legacy prefix runtime bounds remain separate. A 500 ms audio step does not promise 500 ms wall-time output. This five-file study is distinct from the completed three-language matrix in the canonical [report.md](../report.md); it must not replace that matrix's quality scores or capacity claims.

## Measured result: 9 October 2026

All **75/75 calls completed** with all recorded contract checks passing. Exactly one call was active on the shared worker. Independent rescoring reproduced every saved WER/edit count.

| Decode step ms | Chunk ms | First text mean s | EOF-to-final mean s | WER % |
|---:|---:|---:|---:|---:|
| 2000 | 200 | 3.089 | 1.630 | 4.85 |
| 1000 | 200 | 1.727 | 2.524 | 5.83 |
| 2000 | 100 | 3.137 | 1.547 | 4.85 |
| 1000 | 100 | 1.731 | 2.425 | 5.83 |
| 500 | 100 | 1.101 | 8.042 | 12.62 |

The 500 ms setting gives earliest nonempty text but higher WER and EOF delay. A 1,000 ms step is a measured compromise. The 100/200 ms chunk comparisons show similar first-text means; no statistically established chunk-size advantage is claimed. This five-WAV cohort does not replace the canonical report’s full ten-file English quality score.

[Full result and per-WAV tables](../results/english-step-chunk-20261009/report.md) · [Per-WAV CSV](../results/english-step-chunk-20261009/per_wav.csv) · [Raw summary](../results/english-step-chunk-20261009/summary.json).

## Direct versus network comparison

The completed direct sweep is [english-step-chunk-direct-20261009-v2](../results/english-step-chunk-direct-20261009-v2/report.md). Both modes completed 75/75 calls with identical inputs, references, configuration and executable identities. Direct first-text means were 3.035 / 1.655 / 2.967 / 1.661 / 1.038 seconds for the five settings listed above; WER matched the network run at every setting. Separate collection times mean the timing gaps do not isolate WebSocket overhead.

The canonical [report.md](../report.md) includes the comparison in its executive summary and Appendix G. The full multilingual reference remains **direct mode / 200 ms chunks / 2,000 ms steps**. Its ten-file language quality scores are separate from this five-file sweep.

```bash
python3 tools/testing/run_english_step_chunk_sweep.py \
  --mode direct --output results/english-step-chunk-direct-new --repetitions 3
```

## Laptop performance configuration: v4

The operator reports quiet/balanced mode for most earlier experiments and performance configuration for v4. The [v4 sweep](../results/english-step-chunk-direct-20261009-v4/report.md) completed 75/75 calls. [The v2/v4 comparison](../results/english-step-chunk-direct-20261009-v4/power_mode_comparison.md) shows 43–46% lower mean decode-step wall time, with matched WAVs, executable/config identities and unchanged transcripts/accuracy. Exact OS profile identifiers and controlled charger/background conditions were not saved. V4 is a separate performance observation, not a replacement for the earlier direct/network comparison or multilingual curves.
