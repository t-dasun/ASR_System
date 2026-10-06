#pragma once
#include <algorithm>
#include <asr/core/types.hpp>
#include <stdexcept>

namespace asr {
class IAudioSource {
  public:
    virtual ~IAudioSource() = default;
    virtual PcmBuffer read(std::size_t max_samples) = 0;
};
class SyntheticPcmSource final : public IAudioSource {
    std::int64_t remaining_;
    std::uint32_t state_;

  public:
    SyntheticPcmSource(std::int64_t samples, std::uint32_t seed) : remaining_(samples), state_(seed) {
        if (samples < 0 || samples > 16000 * 600)
            throw std::invalid_argument("synthetic length outside 0..600 seconds");
    }
    PcmBuffer read(std::size_t max_samples) override {
        if (max_samples == 0 || max_samples > 16000)
            throw std::invalid_argument("invalid source read size");
        const auto count = std::min(max_samples, static_cast<std::size_t>(remaining_));
        auto samples = std::make_shared<std::vector<std::int16_t>>();
        samples->reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            state_ = state_ * 1664525U + 1013904223U;
            samples->push_back(static_cast<std::int16_t>(static_cast<int>(state_ >> 20) - 2048));
        }
        remaining_ -= static_cast<std::int64_t>(count);
        return samples;
    }
};
} // namespace asr
