# Runnable configurations

- `mock_baseline.yaml`: deterministic C++ smoke/tests, no model.
- `qwen_native_single.yaml`: native CPU streaming, one active call per worker.
- `qwen_prefix_shared.yaml`: persistent shared-model contexts, multiple sessions per worker.

Both real-model presets use the committed 1.2-second smoke WAV. Add a manifest or override `audio.path` for longer recordings. `--set section.key=value` overrides strict YAML configuration; file paths resolve relative to the YAML directory. See [worker/call selection](../docs/CLI_QUICKSTART.md).
