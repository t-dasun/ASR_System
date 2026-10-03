#pragma once
#include <asr/audio/source.hpp>
#include <filesystem>
#include <span>

namespace asr {
enum class ChannelMix { reject, average, left };
class IResampler {
  public:
    virtual ~IResampler() = default;
    virtual std::string id() const = 0;
    virtual std::vector<float> convert(std::span<const float> mono, int input_rate,
                                       int output_rate) const = 0;
};
// Offline preparation adapter: windowed-sinc low-pass, not a streaming inference component.
class SincResampler final : public IResampler {
  public:
    std::string id() const override { return "windowed_sinc_hann32_v1"; }
    std::vector<float> convert(std::span<const float> mono, int input_rate, int output_rate) const override;
};
struct WavMetadata {
    int input_rate = 0, input_channels = 0, input_bits = 0, format_code = 0;
    std::int64_t input_frames = 0, output_samples = 0, clipped_samples = 0;
    std::string channel_mix, resampler;
};
struct PreparedAudio {
    PcmBuffer pcm;
    WavMetadata metadata;
};
// Classic little-endian RIFF/WAVE PCM8/16/24/32 or IEEE float32.
// Preparation is bounded to 128 MiB input and 600 seconds. No loudness normalization.
PreparedAudio prepare_wav(const std::filesystem::path &path, ChannelMix mix, const IResampler &resampler);
class BufferPcmSource final : public IAudioSource {
    PcmBuffer pcm_;
    std::size_t position_ = 0;

  public:
    explicit BufferPcmSource(PcmBuffer pcm);
    PcmBuffer read(std::size_t max_samples) override;
};
} // namespace asr
