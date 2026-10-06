# Executable entry points

- `asr_cli/`: `asr-cli`, with argument/run handling in `main.cpp`, engine construction in `engine_runtime.cpp`, and service wiring in `service.cpp`.
- `asr_native_worker/`: child helper for the one-call native runtime.
- `asr_prefix_worker/`: persistent child helper for the shared-prefix worker pool.

Use `asr-cli`; its selected runtime launches workers automatically. See [run commands](../docs/CLI_QUICKSTART.md).
