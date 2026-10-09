# Direct English sweep: v2 versus v4 power-mode comparison

V4 is the selected comparison run, replacing v3 for this analysis. Both v2 and v4 completed 75/75 calls with all saved contract checks passing. WAVs, references, executable/config identities, thread and worker settings, repetitions and schedule match. All per-setting final transcript sets match. The precise power-profile names and controlled charger/background conditions were not saved, so the timing improvement is observational evidence consistent with the reported power-mode change.

| Step ms | Chunk ms | v2 step wall s | v4 step wall s | Time reduction % | v4 first text mean s | v4 EOF mean s | WER both % |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 2000 | 200 | 1.247 | 0.674 | 45.9 | 2.543 | 0.826 | 4.85 |
| 1000 | 200 | 1.021 | 0.582 | 43.0 | 1.361 | 0.783 | 5.83 |
| 2000 | 100 | 1.211 | 0.670 | 44.6 | 2.537 | 0.825 | 4.85 |
| 1000 | 100 | 1.013 | 0.581 | 42.6 | 1.378 | 0.787 | 5.83 |
| 500 | 100 | 0.929 | 0.511 | 45.0 | 0.787 | 1.633 | 12.62 |

Recorded whole-host mean CPU frequency increased from about 1,594 to 2,220 MHz; this averages reported logical-core clocks including idle cores, not isolated active ASR-core frequency. CPU Tctl mean/maximum rose from 54.7/57.1 °C to 81.3/93.1 °C. These observations support higher CPU performance allowance but do not establish the exact cause or prove throttling.

The 500 ms setting now averages 0.511 s compute per quantum, close to but still above 0.5 s of arriving audio. It produces earlier nonempty text, but WER remains 12.62% and EOF delay is 1.633 s. The 1,000 ms settings provide mean first text around 1.36–1.38 s, EOF around 0.78–0.79 s and 5.83% WER. Two-second settings retain the lower 4.85% WER. Latency changed substantially; accuracy did not.

The raw v2/v3/v4 bundles remain separate. No previous measurements are overwritten and no inference is rerun by this comparison.
