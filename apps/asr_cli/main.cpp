#include <asr/audio/wav.hpp>
#include <asr/benchmark/baseline.hpp>
#include <asr/engines/mock_engine.hpp>
#ifdef ASR_HAS_QWEN_NATIVE
#include <asr/engines/native_engine.hpp>
#endif
#include <iostream>

namespace {
void usage() {
    std::cout << "Usage: asr-cli <validate|dry-run|run> [--config FILE] [--set section.key=value ...]\n"
                 "CPU mock or pinned Qwen native engine; synthetic or WAV audio.\n"
                 "validate and dry-run do not create run artifacts.\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
            usage();
            return 0;
        }
        if (argc < 2) {
            usage();
            return 2;
        }
        const std::string action = argv[1];
        if (action != "validate" && action != "dry-run" && action != "run")
            throw std::invalid_argument("unknown command: " + action);
        std::filesystem::path config_path;
        std::vector<std::string> overrides;
        for (int i = 2; i < argc; ++i) {
            const std::string option = argv[i];
            if ((option != "--config" && option != "--set") || i + 1 >= argc)
                throw std::invalid_argument("expected --config FILE or --set section.key=value");
            if (option == "--config") {
                if (!config_path.empty())
                    throw std::invalid_argument("duplicate --config");
                config_path = argv[++i];
            } else
                overrides.emplace_back(argv[++i]);
        }
        auto config = asr::resolve_config(config_path, overrides);
#ifndef ASR_HAS_QWEN_NATIVE
        if (config.runtime == "qwen_native")
            throw std::invalid_argument("this build has no native Qwen worker; use release-cpu preset");
#endif
        if (action == "validate") {
            std::cout << nlohmann::json({{"valid", true},
                                         {"is_mock", config.runtime == "mock"},
                                         {"resolved", config.resolved}})
                             .dump(2)
                      << '\n';
            return 0;
        }
        std::optional<asr::PreparedAudio> prepared;
        std::int64_t samples = static_cast<std::int64_t>(config.duration_ms) * 16;
        nlohmann::json audio_metadata = {{"source", "synthetic"}, {"seed", config.seed}};
        if (config.audio_source == "wav") {
            asr::SincResampler resampler;
            const auto mix = config.channel_mix == "average" ? asr::ChannelMix::average
                             : config.channel_mix == "left"  ? asr::ChannelMix::left
                                                             : asr::ChannelMix::reject;
            prepared = asr::prepare_wav(config.wav_path, mix, resampler);
            samples = prepared->pcm->size();
            audio_metadata = {{"source", "wav"},
                              {"path", config.wav_path.string()},
                              {"input_rate_hz", prepared->metadata.input_rate},
                              {"input_channels", prepared->metadata.input_channels},
                              {"input_bits", prepared->metadata.input_bits},
                              {"input_format_code", prepared->metadata.format_code},
                              {"input_frames", prepared->metadata.input_frames},
                              {"output_samples", prepared->metadata.output_samples},
                              {"clipped_samples", prepared->metadata.clipped_samples},
                              {"channel_mix", prepared->metadata.channel_mix},
                              {"resampler", prepared->metadata.resampler}};
        }
        if (action == "dry-run") {
            const auto chunk = config.chunk_ms * 16;
            std::cout << nlohmann::json({{"dry_run", true},
                                         {"is_mock", config.runtime == "mock"},
                                         {"calls", 1},
                                         {"chunks", (samples + chunk - 1) / chunk},
                                         {"samples", samples},
                                         {"clock", config.realtime ? "host_steady" : "simulated"},
                                         {"resolved", config.resolved}})
                             .dump(2)
                      << '\n';
            return 0;
        }
        config.resolved["audio"]["effective_samples"] = samples;
        config.resolved["audio"]["effective_duration_seconds"] = static_cast<double>(samples) / 16000;
        std::unique_ptr<asr::IASREngine> engine;
        if (config.runtime == "mock")
            engine = std::make_unique<asr::MockEngine>();
#ifdef ASR_HAS_QWEN_NATIVE
        else {
            asr::NativeQwenOptions options;
            options.worker_executable =
                std::filesystem::read_symlink("/proc/self/exe").parent_path() / "asr-native-worker";
            options.model_directory = config.model_path;
            options.threads = config.native_threads;
            options.decode_step_ms = config.decode_step_ms;
            options.max_new_tokens = config.max_new_tokens;
            options.timeout_ms = config.timeout_ms;
            options.refine_final = config.refine_final;
            engine = std::make_unique<asr::NativeQwenEngine>(std::move(options));
        }
#endif
        std::unique_ptr<asr::IAudioSource> source;
        if (prepared)
            source = std::make_unique<asr::BufferPcmSource>(prepared->pcm);
        else
            source = std::make_unique<asr::SyntheticPcmSource>(samples, config.seed);
        std::unique_ptr<asr::IClock> clock;
        if (config.realtime)
            clock = std::make_unique<asr::SteadyClock>();
        else
            clock = std::make_unique<asr::FakeClock>();
        asr::FileResultRepository repository(config.output_directory);
        nlohmann::json summary;
        try {
            summary = asr::run_baseline(config, *engine, *source, *clock, repository,
                                        asr::new_run_id(config.runtime == "mock" ? "mock" : "native"),
                                        samples, audio_metadata);
        } catch (...) {
            if (!repository.directory().empty())
                std::cout << nlohmann::json(
                                 {{"run_directory", repository.directory().string()}, {"status", "FAILED"}})
                                 .dump()
                          << '\n';
            throw;
        }
        std::cout << nlohmann::json(
                         {{"run_directory", repository.directory().string()}, {"summary", summary}})
                         .dump(2)
                  << '\n';
        return summary.value("status", "FAILED") == "COMPLETE" ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "asr-cli: " << error.what() << '\n';
        return 1;
    }
}
