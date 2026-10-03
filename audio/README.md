# Audio module

M1 implements `IAudioSource` and seeded `SyntheticPcmSource` in `include/asr/audio/source.hpp`. M2 implements bounded classic WAV parsing, explicit channel mixing, a replaceable sinc resampler, prepared PCM source, and causal `PacedAudioStream` with queue, lag, and lifecycle controls. See `docs/m2-code.md` for formats, limits, timing semantics, and verification. The sample-deadline helper lives in `core/include/asr/core/chunk_schedule.hpp`.
