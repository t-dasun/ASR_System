# Configuration

M4 adds `metrics.resource_sampling` (default `true`) and `metrics.sample_interval_ms` (default `200`, range `50..5000`). Real sampling is skipped for simulated clocks with an explicit reason. Native process environment applies custom/BLAS thread budgets before library initialization. See [M4 guide](../docs/m4-code.md).

`mock_baseline.yaml` is the executable mock configuration; `qwen_native_single.yaml` is the M3 native single-call example. `src/config.cpp` resolves defaults → YAML → CLI overrides, validates the supported subset, and exposes resolved JSON. Relative output, WAV, and model paths are based on the YAML directory. M2 adds WAV source and channel-mix settings; M3 adds pinned `qwen_native` runtime, threads, decode step, token cap, timeout, and EOF refinement. See `docs/m2-code.md` and `docs/m3-code.md`. Machine, engine, and suite folders remain reserved; M0 probes retain their separate arguments.
