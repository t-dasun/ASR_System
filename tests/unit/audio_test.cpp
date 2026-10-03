#include <asr/audio/delivery.hpp>
#include <asr/audio/wav.hpp>
#include <asr/engines/mock_engine.hpp>
#include <chrono>
#include <fstream>
#include <iostream>

using namespace asr;
namespace {
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> void rejects(F function, const std::string &message) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, message);
}
struct Fixture {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("asr_m2_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { std::filesystem::create_directory(path); }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    static void put(std::vector<unsigned char> &bytes, std::uint32_t value, int width) {
        for (int i = 0; i < width; ++i)
            bytes.push_back(static_cast<unsigned char>(value >> (8 * i)));
    }
    std::filesystem::path wav(const std::string &name, int format, int rate, int channels, int bits,
                              const std::vector<std::uint32_t> &samples) {
        std::vector<unsigned char> bytes;
        auto label = [&](const char *value) {
            for (int i = 0; i < 4; ++i)
                bytes.push_back(value[i]);
        };
        const auto data_size = samples.size() * (bits / 8);
        label("RIFF");
        put(bytes, 36 + data_size + (data_size & 1U), 4);
        label("WAVE");
        label("fmt ");
        put(bytes, 16, 4);
        put(bytes, format, 2);
        put(bytes, channels, 2);
        put(bytes, rate, 4);
        put(bytes, rate * channels * bits / 8, 4);
        put(bytes, channels * bits / 8, 2);
        put(bytes, bits, 2);
        label("data");
        put(bytes, data_size, 4);
        for (auto sample : samples)
            put(bytes, sample, bits / 8);
        if (data_size & 1U)
            bytes.push_back(0);
        const auto file = path / name;
        std::ofstream output(file, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return file;
    }
};
void wav_conversion(Fixture &fixture) {
    SincResampler sinc;
    auto stereo = fixture.wav("stereo.wav", 1, 16000, 2, 16, {1000, 3000, 32767, 32767, 0, 0});
    rejects([&] { (void)prepare_wav(stereo, ChannelMix::reject, sinc); },
            "stereo accepted without mixing choice");
    auto averaged = prepare_wav(stereo, ChannelMix::average, sinc);
    auto left = prepare_wav(stereo, ChannelMix::left, sinc);
    check(averaged.pcm->size() == 3 && (*averaged.pcm)[0] == 2000 && (*left.pcm)[0] == 1000,
          "explicit channel mixing incorrect");
    check(averaged.metadata.resampler == "identity" && averaged.metadata.input_channels == 2,
          "identity or metadata incorrect");
    auto pcm8 = fixture.wav("pcm8.wav", 1, 16000, 1, 8, {0, 128, 255});
    auto converted = prepare_wav(pcm8, ChannelMix::reject, sinc);
    check(*converted.pcm == std::vector<std::int16_t>({-32768, 0, 32512}),
          "unsigned PCM8 conversion incorrect");
    auto pcm24 = fixture.wav("pcm24.wav", 1, 16000, 1, 24, {0x800000, 0, 0x7fffff});
    check(prepare_wav(pcm24, ChannelMix::reject, sinc).pcm->front() == -32768, "signed PCM24 incorrect");
    auto float_wav = fixture.wav("float.wav", 3, 16000, 1, 32,
                                 {std::bit_cast<std::uint32_t>(1.5F), std::bit_cast<std::uint32_t>(-0.5F)});
    auto floats = prepare_wav(float_wav, ChannelMix::reject, sinc);
    check((*floats.pcm)[0] == 32767 && (*floats.pcm)[1] == -16384 && floats.metadata.clipped_samples == 1,
          "float clip/conversion incorrect");
    const auto rate48 = fixture.wav("rate48.wav", 1, 48000, 1, 16, std::vector<std::uint32_t>(480, 8192));
    auto reduced = prepare_wav(rate48, ChannelMix::reject, sinc);
    check(reduced.pcm->size() == 160 && reduced.metadata.resampler == sinc.id(), "48k to 16k length wrong");
    for (auto sample : *reduced.pcm)
        check(std::abs(sample - 8192) <= 2, "resampler failed DC preservation");
    BufferPcmSource source(reduced.pcm);
    check(source.read(100)->size() == 100 && source.read(100)->size() == 60 && source.read(1)->empty(),
          "prepared PCM source tail/EOF incorrect");
    auto bad = fixture.wav("bad.wav", 1, 16000, 1, 16, {7});
    std::filesystem::resize_file(bad, std::filesystem::file_size(bad) - 1);
    rejects([&] { (void)prepare_wav(bad, ChannelMix::reject, sinc); }, "truncated WAV accepted");
    auto nan = fixture.wav("nan.wav", 3, 16000, 1, 32, {0x7fc00000});
    rejects([&] { (void)prepare_wav(nan, ChannelMix::reject, sinc); }, "nonfinite WAV accepted");
}
struct WithheldSource : IAudioSource {
    FakeClock &clock;
    std::int64_t read_at = -1;
    explicit WithheldSource(FakeClock &value) : clock(value) {}
    PcmBuffer read(std::size_t count) override {
        read_at = clock.now_ns();
        return std::make_shared<const std::vector<std::int16_t>>(count, 1);
    }
};
struct DelayedSink : IRecognitionSink {
    FakeClock &clock;
    explicit DelayedSink(FakeClock &value) : clock(value) {}
    void on_event(const RecognitionEvent &event) override {
        if (event.kind == EventKind::partial)
            clock.sleep_until_ns(clock.now_ns() + 250000000);
    }
};
void pacing_and_controls() {
    FakeClock clock;
    WithheldSource source(clock);
    PacedAudioStream stream(source, clock, "run", "call1", 8000, 200);
    check(stream.next_deadline_ns() == 200000000, "first chunk deadline must follow duration");
    check(static_cast<bool>(stream.produce_ready()) && source.read_at == -1,
          "future audio read before its deadline");
    clock.sleep_until_ns(199999999);
    check(static_cast<bool>(stream.produce_ready()) && source.read_at == -1,
          "future audio leaked one nanosecond early");
    clock.sleep_until_ns(200000000);
    check(static_cast<bool>(stream.produce_ready()) && source.read_at == 200000000,
          "first chunk withheld too long");
    check(stream.finish_input(1, 8000).code == ErrorCode::invalid_input, "early EOF accepted");
    auto first = stream.pop();
    check(first && first.value && first.value->timing.first_sample == 0 &&
              first.value->timing.sample_count == 3200 && first.value->timing.lag_ns == 0,
          "first chunk timing incorrect");
    MockEngine engine;
    DelayedSink sink(clock);
    SessionConfig settings;
    settings.run_id = "run";
    settings.call_id = "call1";
    settings.partial_every_ms = 200;
    auto session = engine.create_session(settings, sink, clock);
    check(session && session.value->submit(std::move(first.value->chunk)), "delayed mock submission failed");
    check(clock.now_ns() == 450000000, "delayed mock did not consume time");
    check(static_cast<bool>(stream.produce_ready()), "second chunk was not produced after delay");
    auto second = stream.pop();
    check(second && second.value && second.value->timing.sequence == 1 &&
              second.value->timing.first_sample == 3200 && second.value->timing.lag_ns == 50000000 &&
              stream.stats().late_chunks == 1,
          "late chunk or sequence accounting incorrect");
    stream.cancel();
    check(stream.state() == DeliveryState::stopped && !stream.pop().value,
          "immediate cancellation did not stop delivery");
    WithheldSource replacement(clock);
    rejects([&] { stream.reset(replacement, "call1", 1600); }, "reset reused call ID");
    stream.reset(replacement, "call2", 1600);
    check(stream.next_deadline_ns() == 550000000 && stream.stats().delivered_chunks == 0,
          "reset failed to replace timeline/state");
    stream.graceful_stop();
    check(stream.state() == DeliveryState::completed && stream.graceful(), "graceful stop did not drain");
    FakeClock drain_clock;
    WithheldSource drain_source(drain_clock);
    PacedAudioStream drain(drain_source, drain_clock, "run", "drain", 3200, 200);
    drain_clock.sleep_until_ns(200000000);
    check(static_cast<bool>(drain.produce_ready()), "drain source failed");
    drain.graceful_stop();
    check(drain.state() == DeliveryState::draining && drain.pop().value.has_value() &&
              drain.state() == DeliveryState::completed && drain.graceful(),
          "graceful stop lost queued audio");
    check(drain.finish_input(1, 3200).code == ErrorCode::invalid_state,
          "graceful stop incorrectly reported normal EOF");
    FakeClock eof_clock;
    WithheldSource eof_source(eof_clock);
    PacedAudioStream eof(eof_source, eof_clock, "run", "eof", 3200, 200);
    eof_clock.sleep_until_ns(200000000);
    check(static_cast<bool>(eof.produce_ready()) &&
              eof.finish_input(0, 3200).code == ErrorCode::invalid_input &&
              static_cast<bool>(eof.finish_input(1, 3200)) && static_cast<bool>(eof.finish_input(1, 3200)) &&
              eof.pop().value.has_value() && eof.state() == DeliveryState::completed,
          "normal EOF sequence, idempotence, or drain failed");
    FakeClock congested_clock;
    WithheldSource congested_source(congested_clock);
    DeliveryLimits tight{2, 400, 1000, 5};
    PacedAudioStream congested(congested_source, congested_clock, "run", "congested", 16000, 200, tight);
    congested_clock.sleep_until_ns(600000000);
    check(congested.produce_ready().code == ErrorCode::resource_exhausted &&
              congested.stats().overflows == 1 && congested.stats().discarded_samples == 6400,
          "bounded queue did not fail explicitly");
    FakeClock byte_clock;
    WithheldSource byte_source(byte_clock);
    PacedAudioStream byte_bound(byte_source, byte_clock, "run", "bytes", 6400, 200,
                                DeliveryLimits{8, 1000, 1000, 5, 6400});
    byte_clock.sleep_until_ns(400000000);
    check(byte_bound.produce_ready().code == ErrorCode::resource_exhausted &&
              byte_bound.stats().peak_bytes == 6400,
          "byte bound did not constrain queued PCM");
    FakeClock overdue_clock;
    WithheldSource overdue_source(overdue_clock);
    PacedAudioStream overdue(overdue_source, overdue_clock, "run", "overdue", 3200, 200,
                             DeliveryLimits{2, 400, 100, 5});
    overdue_clock.sleep_until_ns(400000000);
    check(overdue.produce_ready().code == ErrorCode::deadline_expired && overdue_source.read_at == -1,
          "overdue source read was not rejected/accounted");
}
} // namespace
int main() {
    try {
        Fixture fixture;
        wav_conversion(fixture);
        pacing_and_controls();
        std::cout << "WAV conversion, causal delivery, lag, queue bounds, stop/reset passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
