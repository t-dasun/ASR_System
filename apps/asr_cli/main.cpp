#include "engine_runtime.hpp"
#include "service.hpp"
#include <asr/audio/wav.hpp>
#include <asr/backend/websocket.hpp>
#include <asr/benchmark/baseline.hpp>
#include <asr/benchmark/load.hpp>
#include <asr/benchmark/sweep.hpp>
#include <cmath>
#include <iostream>

using asr::app::make_engine;

namespace {
std::vector<std::string> split(const std::string &value, char separator) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(separator, begin);
        result.push_back(value.substr(begin, end - begin));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return result;
}
int strict_int(const std::string &value) {
    std::size_t used = 0;
    const auto result = std::stoi(value, &used);
    if (used != value.size())
        throw std::invalid_argument("invalid integer: " + value);
    return result;
}
double strict_double(const std::string &value) {
    std::size_t used = 0;
    const auto result = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(result))
        throw std::invalid_argument("invalid finite number: " + value);
    return result;
}
std::uint64_t strict_u64(const std::string &value) {
    std::size_t used = 0;
    const auto result = std::stoull(value, &used);
    if (used != value.size() || value.starts_with('-'))
        throw std::invalid_argument("invalid nonnegative integer: " + value);
    return result;
}
void usage() {
    std::cout << "Usage: asr-cli "
                 "<validate|dry-run|run|load-dry-run|load|sweep-dry-run|sweep|serve> [--config "
                 "FILE] [--set "
                 "section.key=value ...]\n"
                 "CPU mock or pinned Qwen native engine; synthetic or WAV audio.\n"
                 "Load: --calls N --concurrency N --warmups N --repetitions N --stagger-ms N --languages "
                 "en,id,zh. Load/serve: --manifest JSONL for distinct WAV calls.\n"
                 "Sweep: --strategy baseline|selected|oat|matrix|scale --axis key=v1,v2 [--case "
                 "key=value;key=value].\n"
                 "SLO: --max-failure-rate N --max-p95-final-ms N --max-p95-send-lag-ms N "
                 "--min-audio-throughput N --max-peak-rss-bytes N.\n"
                 "validate and dry-run commands do not create run artifacts.\n";
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
        if (action != "validate" && action != "dry-run" && action != "run" && action != "load-dry-run" &&
            action != "load" && action != "sweep-dry-run" && action != "sweep" && action != "serve")
            throw std::invalid_argument("unknown command: " + action);
        std::filesystem::path config_path;
        std::vector<std::string> overrides;
        asr::LoadSpec load_spec;
        asr::SweepSpec sweep_spec;
        bool concurrency_explicit = false, calls_explicit = false;
        std::filesystem::path wav_manifest;
        std::string preset;
        int endurance_seconds = 1800;
        int serve_port = 0;
        for (int i = 2; i < argc; ++i) {
            const std::string option = argv[i];
            if (i + 1 >= argc)
                throw std::invalid_argument("option needs a value: " + option);
            if (option == "--config") {
                if (!config_path.empty())
                    throw std::invalid_argument("duplicate --config");
                config_path = argv[++i];
            } else if (option == "--set")
                overrides.emplace_back(argv[++i]);
            else if (option == "--port" && (action == "serve"))
                serve_port = strict_int(argv[++i]);
            else if (option == "--manifest" &&
                     (action == "load" || action == "load-dry-run" || action == "serve"))
                wav_manifest = argv[++i];
            else if (action == "load" || action == "load-dry-run" || action == "sweep" ||
                     action == "sweep-dry-run") {
                const std::string value = argv[++i];
                if (option == "--calls") {
                    load_spec.calls = strict_int(value);
                    calls_explicit = true;
                } else if (option == "--concurrency") {
                    load_spec.concurrency = strict_int(value);
                    concurrency_explicit = true;
                } else if (option == "--warmups")
                    load_spec.warmups = strict_int(value);
                else if (option == "--repetitions")
                    load_spec.repetitions = strict_int(value);
                else if (option == "--stagger-ms")
                    load_spec.stagger_ms = strict_int(value);
                else if (option == "--languages") {
                    load_spec.languages = split(value, ',');
                } else if (option == "--mode")
                    load_spec.mode = value;
                else if (option == "--max-failure-rate")
                    load_spec.max_failure_rate = strict_double(value);
                else if (option == "--max-p95-final-ms")
                    load_spec.max_p95_final_ms = strict_double(value);
                else if (option == "--max-p95-send-lag-ms")
                    load_spec.max_p95_send_lag_ms = strict_double(value);
                else if (option == "--min-audio-throughput")
                    load_spec.min_audio_throughput = strict_double(value);
                else if (option == "--max-peak-rss-bytes")
                    load_spec.max_peak_rss_bytes = strict_u64(value);
                else if (option == "--strategy" && (action == "sweep" || action == "sweep-dry-run"))
                    sweep_spec.strategy = value;
                else if (option == "--preset" && (action == "sweep" || action == "sweep-dry-run"))
                    preset = value;
                else if (option == "--endurance-seconds" && (action == "sweep" || action == "sweep-dry-run"))
                    endurance_seconds = strict_int(value);
                else if (option == "--axis" && (action == "sweep" || action == "sweep-dry-run")) {
                    const auto equal = value.find('=');
                    if (equal == std::string::npos)
                        throw std::invalid_argument("axis needs key=comma-separated-values");
                    sweep_spec.axes.push_back({value.substr(0, equal), split(value.substr(equal + 1), ',')});
                } else if (option == "--case" && (action == "sweep" || action == "sweep-dry-run"))
                    sweep_spec.selected.push_back(split(value, ';'));
                else
                    throw std::invalid_argument("unknown option: " + option);
            } else
                throw std::invalid_argument("unknown option: " + option);
        }
        auto config = asr::resolve_config(config_path, overrides);
        if (!wav_manifest.empty()) {
            load_spec.inputs = asr::prepare_load_manifest(wav_manifest);
            if (!calls_explicit)
                load_spec.calls = static_cast<int>(load_spec.inputs.size());
            if (load_spec.languages.empty())
                load_spec.languages = {"en", "id", "zh"};
        }
        if (load_spec.languages.empty())
            load_spec.languages.push_back(config.language);
        load_spec.seed = config.seed;
        sweep_spec.load = load_spec;
#ifndef ASR_HAS_QWEN_NATIVE
        if (config.runtime != "mock")
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
        if (action == "serve")
            return asr::app::run_service(config, config_path, overrides, load_spec, serve_port);
        std::optional<asr::PreparedAudio> prepared;
        std::int64_t samples = static_cast<std::int64_t>(config.duration_ms) * 16;
        nlohmann::json audio_metadata = {{"source", "synthetic"}, {"seed", config.seed}};
        if (config.audio_source == "wav" && load_spec.inputs.empty()) {
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
                                         {"worker_processes", config.worker_processes},
                                         {"inference_slots_per_process", 1},
                                         {"max_sessions_per_process", config.max_sessions_per_process},
                                         {"chunks", (samples + chunk - 1) / chunk},
                                         {"samples", samples},
                                         {"clock", config.realtime ? "host_steady" : "simulated"},
                                         {"resolved", config.resolved}})
                             .dump(2)
                      << '\n';
            return 0;
        }
        if (action == "load-dry-run" || action == "load") {
            auto plan = asr::plan_load(config, load_spec, samples, asr::linux_available_memory_bytes());
            if (action == "load-dry-run") {
                std::cout << plan.json().dump(2) << '\n';
                return 0;
            }
            if (!plan.allowed) {
                std::cerr << "asr-cli: load preflight rejected run: " << plan.preflight["skip_reasons"].dump()
                          << '\n';
                return 1;
            }
        }
        if (action == "sweep-dry-run" || action == "sweep") {
            if (!preset.empty()) {
                if (!sweep_spec.axes.empty() || !sweep_spec.selected.empty() ||
                    sweep_spec.strategy != "baseline")
                    throw std::invalid_argument("--preset cannot be combined with strategy, axis, or case");
                if (preset == "baseline" || preset == "cold-start")
                    sweep_spec.strategy = "baseline";
                else if (preset == "concurrency" || preset == "processes") {
                    sweep_spec.strategy = "scale";
                    sweep_spec.axes = {{"concurrency", {"1", "2", "4"}}};
                } else if (preset == "threads") {
                    sweep_spec.strategy = "oat";
                    sweep_spec.axes = {{"model.threads", {"1", "2", "4", "8", "16"}}};
                } else if (preset == "chunks") {
                    sweep_spec.strategy = "oat";
                    sweep_spec.axes = {{"audio.chunk_ms", {"100", "200", "500", "1000"}}};
                } else if (preset == "native-config") {
                    sweep_spec.strategy = "matrix";
                    sweep_spec.axes = {{"model.threads", {"2", "4"}},
                                       {"model.decode_step_ms", {"1000", "2000"}}};
                } else if (preset == "endurance") {
                    if (endurance_seconds < 60 || endurance_seconds > 7200 || samples <= 0)
                        throw std::invalid_argument("endurance duration must be 60..7200 seconds");
                    sweep_spec.strategy = "baseline";
                    sweep_spec.load.calls =
                        std::max(sweep_spec.load.calls,
                                 int((std::int64_t(endurance_seconds) * 16000 + samples - 1) / samples));
                } else
                    throw std::invalid_argument("unknown sweep preset: " + preset);
                if (preset == "cold-start")
                    sweep_spec.load.warmups = 0;
            }
            if (!concurrency_explicit && sweep_spec.strategy != "scale")
                sweep_spec.load.concurrency = 1;
            auto plan = asr::plan_sweep(config_path, overrides, sweep_spec, samples,
                                        asr::linux_available_memory_bytes());
            if (action == "sweep-dry-run") {
                std::cout << plan.json().dump(2) << '\n';
                return 0;
            }
            auto summary =
                asr::run_sweep(plan, config.output_directory,
                               [&](const asr::RunConfig &effective, const asr::LoadPlan &load) {
                                   auto manager = make_engine(effective);
                                   std::unique_ptr<asr::WebSocketServer> server;
                                   std::unique_ptr<asr::WebSocketEngine> network;
                                   asr::IASREngine *ingress = manager.get();
                                   if (load.spec.mode == "network") {
                                       server = std::make_unique<asr::WebSocketServer>(*manager);
                                       network = std::make_unique<asr::WebSocketEngine>(
                                           server->port(), manager->capabilities());
                                       ingress = network.get();
                                   }
                                   return asr::run_load(effective, *ingress, load, samples,
                                                        prepared ? prepared->pcm : nullptr, audio_metadata);
                               });
            std::cout << summary.dump(2) << '\n';
            return summary.value("status", "FAILED") == "COMPLETE" ? 0 : 1;
        }
        config.resolved["audio"]["effective_samples"] = samples;
        config.resolved["audio"]["effective_duration_seconds"] = static_cast<double>(samples) / 16000;
        std::unique_ptr<asr::IASREngine> engine = make_engine(config);
        config.shared_model_loaded = config.runtime == "qwen_prefix";
        if (action == "load") {
            auto plan = asr::plan_load(config, load_spec, samples, asr::linux_available_memory_bytes());
            std::unique_ptr<asr::WebSocketServer> server;
            std::unique_ptr<asr::WebSocketEngine> network;
            asr::IASREngine *ingress = engine.get();
            if (load_spec.mode == "network") {
                server = std::make_unique<asr::WebSocketServer>(*engine);
                network = std::make_unique<asr::WebSocketEngine>(server->port(), engine->capabilities());
                ingress = network.get();
            }
            auto summary = asr::run_load(config, *ingress, plan, samples, prepared ? prepared->pcm : nullptr,
                                         audio_metadata);
            std::cout << summary.dump(2) << '\n';
            return summary.value("status", "FAILED") == "COMPLETE" ? 0 : 1;
        }
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
