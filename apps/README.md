# Application entry points

`asr_cli/main.cpp` is the M1 composition root for `asr-cli validate|dry-run|run`. It currently supports only the mock CPU engine and synthetic audio. Production `asr-bench`, `asr-server`, and `asr-worker` remain reserved. Native validation probes remain separately runnable under `research/native_qwen`.
