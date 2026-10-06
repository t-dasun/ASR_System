#include <asr/audio/wav.hpp>
#include <bit>
#include <cmath>
#include <fstream>
#include <numbers>

namespace asr {
namespace {
std::uint32_t le(const std::vector<unsigned char> &bytes, std::size_t offset, unsigned width) {
    if (offset > bytes.size() || width > bytes.size() - offset)
        throw std::invalid_argument("truncated WAV field");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= std::uint32_t(bytes[offset + i]) << (8 * i);
    return value;
}
bool tag(const std::vector<unsigned char> &bytes, std::size_t offset, const char *name) {
    return offset <= bytes.size() && bytes.size() - offset >= 4 &&
           std::equal(bytes.begin() + offset, bytes.begin() + offset + 4, name);
}
} // namespace
std::vector<float> SincResampler::convert(std::span<const float> mono, int input_rate,
                                          int output_rate) const {
    if (input_rate < 8000 || input_rate > 192000 || output_rate != 16000 ||
        mono.size() > static_cast<std::size_t>(input_rate) * 600)
        throw std::invalid_argument("unsupported resampling bounds");
    for (float value : mono)
        if (!std::isfinite(value))
            throw std::invalid_argument("nonfinite PCM");
    if (input_rate == output_rate)
        return {mono.begin(), mono.end()};
    const auto count = (std::uint64_t(mono.size()) * output_rate + input_rate / 2) / input_rate;
    std::vector<float> result(count);
    const double ratio = std::min(1.0, double(output_rate) / input_rate);
    const double cutoff = 0.95 * ratio;
    const double radius = 32.0 / ratio;
    for (std::size_t i = 0; i < count; ++i) {
        const double position = double(i) * input_rate / output_rate;
        const auto begin = std::max<std::int64_t>(0, std::int64_t(std::ceil(position - radius)));
        const auto end = std::min<std::int64_t>(mono.size() - 1, std::int64_t(std::floor(position + radius)));
        double sum = 0, weight_sum = 0;
        for (auto j = begin; j <= end; ++j) {
            const double distance = position - double(j);
            const double x = std::numbers::pi * cutoff * distance;
            const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(x) / x;
            const double window = 0.5 * (1.0 + std::cos(std::numbers::pi * distance / radius));
            const double weight = cutoff * sinc * window;
            sum += mono[j] * weight;
            weight_sum += weight;
        }
        result[i] = weight_sum == 0 ? 0.0F : static_cast<float>(sum / weight_sum);
    }
    return result;
}
PreparedAudio prepare_wav(const std::filesystem::path &path, ChannelMix mix, const IResampler &resampler) {
    const auto size = std::filesystem::file_size(path);
    if (size < 12 || size > 128 * 1024 * 1024)
        throw std::invalid_argument("WAV size outside 12 bytes..128 MiB");
    std::vector<unsigned char> bytes(size);
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("cannot read complete WAV");
    if (!tag(bytes, 0, "RIFF") || !tag(bytes, 8, "WAVE") || le(bytes, 4, 4) != size - 8)
        throw std::invalid_argument("expected exact little-endian RIFF/WAVE extent");
    std::size_t fmt = 0, data = 0, data_size = 0;
    for (std::size_t pos = 12; pos < bytes.size();) {
        if (bytes.size() - pos < 8)
            throw std::invalid_argument("truncated WAV chunk header");
        const auto length = le(bytes, pos + 4, 4);
        const auto start = pos + 8;
        if (length > bytes.size() - start)
            throw std::invalid_argument("truncated WAV chunk");
        if (tag(bytes, pos, "fmt ")) {
            if (fmt || length < 16)
                throw std::invalid_argument("duplicate or short WAV format");
            fmt = start;
        } else if (tag(bytes, pos, "data")) {
            if (data)
                throw std::invalid_argument("duplicate WAV data");
            data = start;
            data_size = length;
        }
        pos = start + length + (length & 1U);
        if (pos > bytes.size())
            throw std::invalid_argument("missing WAV padding");
    }
    if (!fmt || !data)
        throw std::invalid_argument("WAV requires fmt and data chunks");
    WavMetadata meta;
    meta.format_code = le(bytes, fmt, 2);
    meta.input_channels = le(bytes, fmt + 2, 2);
    meta.input_rate = le(bytes, fmt + 4, 4);
    meta.input_bits = le(bytes, fmt + 14, 2);
    const bool integer = meta.format_code == 1;
    if ((!integer && meta.format_code != 3) ||
        (integer && meta.input_bits != 8 && meta.input_bits != 16 && meta.input_bits != 24 &&
         meta.input_bits != 32) ||
        (!integer && meta.input_bits != 32))
        throw std::invalid_argument("unsupported WAV encoding");
    if (meta.input_channels < 1 || meta.input_channels > 8 || meta.input_rate < 8000 ||
        meta.input_rate > 192000)
        throw std::invalid_argument("unsupported WAV rate/channels");
    const auto width = meta.input_bits / 8;
    const auto alignment = width * meta.input_channels;
    if (le(bytes, fmt + 12, 2) != static_cast<unsigned>(alignment) ||
        le(bytes, fmt + 8, 4) != static_cast<unsigned>(alignment * meta.input_rate) || data_size % alignment)
        throw std::invalid_argument("inconsistent WAV block alignment, byte rate, or partial frame");
    meta.input_frames = data_size / alignment;
    if (meta.input_frames > std::int64_t(meta.input_rate) * 600)
        throw std::invalid_argument("WAV exceeds 600 seconds");
    if (mix == ChannelMix::reject && meta.input_channels != 1)
        throw std::invalid_argument("multichannel WAV requires explicit average or left mixing");
    meta.channel_mix = mix == ChannelMix::reject ? "reject" : mix == ChannelMix::average ? "average" : "left";
    std::vector<float> mono(meta.input_frames);
    for (std::int64_t frame = 0; frame < meta.input_frames; ++frame) {
        double sum = 0;
        for (int channel = 0; channel < meta.input_channels; ++channel) {
            const auto raw = le(bytes, data + frame * alignment + channel * width, width);
            double value;
            if (!integer)
                value = std::bit_cast<float>(raw);
            else if (width == 1)
                value = (static_cast<int>(raw) - 128) / 128.0;
            else {
                const std::int64_t sign = std::int64_t(1) << (meta.input_bits - 1);
                const auto signed_value = std::int64_t(raw) >= sign ? std::int64_t(raw) - 2 * sign : raw;
                value = double(signed_value) / double(sign);
            }
            if (!std::isfinite(value))
                throw std::invalid_argument("WAV contains nonfinite float samples");
            if (mix == ChannelMix::average)
                sum += value / meta.input_channels;
            else if (channel == 0)
                sum = value;
        }
        mono[frame] = static_cast<float>(sum);
    }
    auto normalized = resampler.convert(mono, meta.input_rate, 16000);
    const auto expected = (meta.input_frames * 16000 + meta.input_rate / 2) / meta.input_rate;
    if (normalized.size() != static_cast<std::size_t>(expected))
        throw std::runtime_error("resampler returned wrong sample count");
    auto pcm = std::make_shared<std::vector<std::int16_t>>();
    pcm->reserve(normalized.size());
    for (const float value : normalized) {
        if (!std::isfinite(value))
            throw std::runtime_error("resampler returned nonfinite samples");
        if (value < -1 || value > 32767.0 / 32768.0)
            ++meta.clipped_samples;
        pcm->push_back(static_cast<std::int16_t>(
            std::lround(std::clamp(double(value), -1.0, 32767.0 / 32768.0) * 32768)));
    }
    meta.output_samples = pcm->size();
    meta.resampler = meta.input_rate == 16000 ? "identity" : resampler.id();
    return {pcm, meta};
}
BufferPcmSource::BufferPcmSource(PcmBuffer pcm) : pcm_(std::move(pcm)) {
    if (!pcm_ || pcm_->size() > 16000 * 600)
        throw std::invalid_argument("invalid prepared PCM buffer");
}
PcmBuffer BufferPcmSource::read(std::size_t max_samples) {
    if (max_samples == 0 || max_samples > 16000)
        throw std::invalid_argument("invalid source read size");
    const auto count = std::min(max_samples, pcm_->size() - position_);
    auto result = std::make_shared<const std::vector<std::int16_t>>(pcm_->begin() + position_,
                                                                    pcm_->begin() + position_ + count);
    position_ += count;
    return result;
}
} // namespace asr
