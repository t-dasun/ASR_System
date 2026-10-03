# Engine adapters

Public engine/session/sink contracts live in `interfaces/include/asr/engines/engine.hpp`. The independent `mock` adapter is implemented and tested in M1. M3 adds the process-isolated `native` Qwen adapter and worker, built only in the CPU-native preset. Vendor headers remain in the worker source; core/session public headers contain none. Alternative adapters remain pending. M0's research probe stays separate.
