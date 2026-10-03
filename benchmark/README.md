# Experiment engine

M4 integrates C++ timing and replaceable Linux resource sampling into `run_baseline`. The offline `tools/evaluation/run_measured.py` launches sequential native calls, evaluates human-reference accuracy, and seals auditable experiment artifacts. It performs no inference or pacing. Concurrent scheduling/suites remain M5/M6. See [M4 code and definitions](../docs/m4-code.md).

`run_baseline` accepts injected engine, source, clock, and repository interfaces. M2 routes it through `PacedAudioStream` for synthetic or prepared WAV input and persists per-chunk timing. M3 uses the same runner with the native engine; `run_mock_baseline` remains as a strict mock-only wrapper for tests. Results distinguish `is_mock` and engine ID. The full ExperimentRunner, load generator, suite expansion, and saturation detection remain pending; current runs are single-call diagnostics.
