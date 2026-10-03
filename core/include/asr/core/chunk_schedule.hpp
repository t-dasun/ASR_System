#pragma once
#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace asr {
// Sample-based offsets avoid cumulative rounding or sleep drift.
inline std::chrono::nanoseconds audio_offset(std::int64_t samples, int sample_rate) {
    if (samples < 0 || samples > 1'000'000'000LL || sample_rate <= 0) {
        throw std::invalid_argument("invalid sample count or sample rate");
    }
    return std::chrono::nanoseconds(samples * 1'000'000'000LL / sample_rate);
}
inline int chunk_samples(int chunk_ms, int sample_rate) {
    if (chunk_ms < 1 || chunk_ms > 1000 || sample_rate < 1 || sample_rate > 192000) {
        throw std::invalid_argument("chunk must be 1..1000 ms at 1..192000 Hz");
    }
    const int samples = sample_rate * chunk_ms / 1000;
    if (samples == 0 || sample_rate * chunk_ms % 1000 != 0) {
        throw std::invalid_argument("chunk duration must represent whole samples");
    }
    return samples;
}
} // namespace asr
