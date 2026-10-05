#include <asr/audio/wav.hpp>
#include <asr/backend/api_service.hpp>
#include <asr/backend/session_manager.hpp>
#include <asr/backend/websocket.hpp>
#include <asr/benchmark/baseline.hpp>
#include <asr/benchmark/load.hpp>
#include <asr/benchmark/sweep.hpp>
#include <asr/engines/mock_engine.hpp>
#include <asr/observability/system_sampler.hpp>
#include <chrono>
#include <cmath>
#include <csignal>
#ifdef ASR_HAS_QWEN_NATIVE
#include <asr/engines/native_engine.hpp>
#include <asr/engines/prefix_engine.hpp>
#endif
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stop_server = 0;
void on_stop_signal(int) { stop_server = 1; }
const char *state_name(asr::SessionState state) {
    switch (state) {
    case asr::SessionState::creating:
        return "creating";
    case asr::SessionState::ready:
        return "ready";
    case asr::SessionState::streaming:
        return "streaming";
    case asr::SessionState::finalizing:
        return "finalizing";
    case asr::SessionState::completed:
        return "completed";
    case asr::SessionState::stopped:
        return "stopped";
    case asr::SessionState::failed:
        return "failed";
    }
    return "unknown";
}
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
std::unique_ptr<asr::IASREngine> make_engine(const asr::RunConfig &config) {
#ifdef ASR_HAS_QWEN_NATIVE
    if (config.runtime == "qwen_prefix")
        return std::make_unique<asr::PrefixMultiplexEngine>(
            config.model_path.string(), config.max_sessions_per_process,
            config.prefix_preview_ms, config.native_threads, config.idle_timeout_ms,
            config.total_timeout_ms, config.timeout_ms);
#endif
    std::unique_ptr<asr::IWorkerExecutor> executor;
    if (config.runtime == "mock")
        executor = std::make_unique<asr::FactoryWorkerExecutor>(
            "in_process", [] { return std::make_unique<asr::MockEngine>(); });
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
        options.cpu_cores = config.cpu_cores;
        executor = std::make_unique<asr::FactoryWorkerExecutor>(
            "process", [options] { return std::make_unique<asr::NativeQwenEngine>(options); });
    }
#else
    else
        throw std::invalid_argument("this build has no native Qwen worker; use release-cpu preset");
#endif
    std::unique_ptr<asr::IWorkerScheduler> scheduler;
    if (config.scheduler == "round_robin")
        scheduler = std::make_unique<asr::RoundRobinScheduler>();
    else
        scheduler = std::make_unique<asr::LeastActiveScheduler>();
    asr::WorkerLayout layout;
    layout.processes = config.worker_processes;
    layout.runtime_threads = config.native_threads;
    layout.idle_timeout_ms = config.idle_timeout_ms;
    layout.total_timeout_ms = config.total_timeout_ms;
    layout.cpu_cores = config.cpu_cores;
    return std::make_unique<asr::SessionManager>(layout, std::move(executor), std::move(scheduler));
}
void drain_engine(asr::IASREngine &engine) {
    if (auto *manager = dynamic_cast<asr::SessionManager *>(&engine))
        manager->begin_draining();
#ifdef ASR_HAS_QWEN_NATIVE
    if (auto *prefix = dynamic_cast<asr::PrefixMultiplexEngine *>(&engine))
        prefix->begin_draining();
#endif
}
nlohmann::json workers_json(asr::IASREngine &engine) {
    auto workers = nlohmann::json::array();
    if (auto *manager = dynamic_cast<asr::SessionManager *>(&engine))
        for (const auto &worker : manager->workers())
            workers.push_back({{"worker_id", worker.worker_id}, {"call_id", worker.call_id},
                               {"language", worker.language}, {"state", state_name(worker.state)},
                               {"occupied", worker.occupied}, {"healthy", worker.healthy},
                               {"active_sessions", worker.active_sessions},
                               {"runtime_threads", worker.runtime_threads},
                               {"process_id", worker.process_id}, {"failures", worker.failures},
                               {"last_error", worker.last_error}, {"server_queue_depth", nullptr}});
#ifdef ASR_HAS_QWEN_NATIVE
    if (auto *prefix = dynamic_cast<asr::PrefixMultiplexEngine *>(&engine)) {
        const auto status = prefix->worker_status();
        auto calls = nlohmann::json::array();
        bool busy = false;
        for (const auto &call : status.calls) {
            busy |= call.decoding;
            calls.push_back({{"call_id", call.call_id}, {"language", call.language},
                             {"state", state_name(call.state)}, {"buffered_samples", call.buffered_samples},
                             {"decoding", call.decoding}});
        }
        workers.push_back({{"worker_id", "prefix_shared_0"}, {"call_id", ""}, {"language", "mixed"},
                           {"state", busy ? "streaming" : "ready"}, {"occupied", !status.calls.empty()},
                           {"healthy", true}, {"active_sessions", status.calls.size()},
                           {"max_sessions", status.max_calls}, {"runtime_threads", status.runtime_threads},
                           {"blas_threads", status.blas_threads},
                           {"process_id", getpid()}, {"failures", status.failures}, {"last_error", status.last_error},
                           {"server_queue_depth", status.queued_jobs}, {"draining", status.draining},
                           {"calls", calls}});
    }
#endif
    return workers;
}
void validate_shared_suite(const asr::RunConfig &startup, const asr::RunConfig &selected) {
    if (startup.runtime != "qwen_prefix" && selected.runtime != "qwen_prefix") return;
    if (startup.runtime != selected.runtime || startup.model_path != selected.model_path ||
        startup.native_threads != selected.native_threads ||
        startup.max_sessions_per_process != selected.max_sessions_per_process ||
        startup.prefix_preview_ms != selected.prefix_preview_ms ||
        startup.idle_timeout_ms != selected.idle_timeout_ms ||
        startup.total_timeout_ms != selected.total_timeout_ms || startup.timeout_ms != selected.timeout_ms)
        throw std::invalid_argument("shared-model API suites must retain startup runtime, model, threads, slots, and preview interval; use CLI sweeps for runtime changes");
}
void usage() {
    std::cout
        << "Usage: asr-cli <validate|dry-run|run|load-dry-run|load|sweep-dry-run|sweep|serve|serve-prefix> [--config "
           "FILE] [--set "
           "section.key=value ...]\n"
           "CPU mock or pinned Qwen native engine; synthetic or WAV audio.\n"
           "Load: --calls N --concurrency N --warmups N --repetitions N --stagger-ms N --languages "
           "en,id,zh.\n"
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
            action != "load" && action != "sweep-dry-run" && action != "sweep" && action != "serve" &&
            action != "serve-prefix")
            throw std::invalid_argument("unknown command: " + action);
        std::filesystem::path config_path;
        std::vector<std::string> overrides;
        asr::LoadSpec load_spec;
        asr::SweepSpec sweep_spec;
        bool concurrency_explicit = false;
        std::string preset;
        int endurance_seconds = 1800;
        int serve_port = 0;
        int prefix_max_calls = 2;
        int prefix_preview_ms = 4000;
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
            else if (option == "--port" && (action == "serve" || action == "serve-prefix"))
                serve_port = strict_int(argv[++i]);
            else if (option == "--max-calls" && action == "serve-prefix")
                prefix_max_calls = strict_int(argv[++i]);
            else if (option == "--preview-ms" && action == "serve-prefix")
                prefix_preview_ms = strict_int(argv[++i]);
            else if (action == "load" || action == "load-dry-run" || action == "sweep" ||
                     action == "sweep-dry-run") {
                const std::string value = argv[++i];
                if (option == "--calls")
                    load_spec.calls = strict_int(value);
                else if (option == "--concurrency") {
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
        if (action == "serve-prefix") {
            overrides.emplace_back("model.runtime=qwen_prefix");
            overrides.emplace_back("workers.scheduler=round_robin");
            overrides.emplace_back("workers.max_sessions_per_process=" + std::to_string(prefix_max_calls));
            overrides.emplace_back("model.prefix_preview_ms=" + std::to_string(prefix_preview_ms));
        }
        auto config = asr::resolve_config(config_path, overrides);
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
        if (action == "serve" || action == "serve-prefix") {
            if (serve_port < 0 || serve_port > 65535)
                throw std::invalid_argument("serve port must be 0..65535");
            auto manager = make_engine(config);
            asr::ServiceAdmissionGate gate;
            asr::LinuxSystemSampler sampler;
            std::mutex sampler_mutex;
            asr::ApiService api(
                config_path, overrides, config.output_directory,
                {{"schema_version", 1},
                 {"protocol", "asr.websocket.v1"},
                 {"engine", manager->capabilities().engine_id},
                 {"engine_revision", manager->capabilities().revision},
                 {"streaming_kind", manager->capabilities().streaming_kind},
                 {"is_mock", manager->capabilities().is_mock},
                 {"device", manager->capabilities().device},
                 {"precision", manager->capabilities().precision},
                 {"languages", manager->capabilities().languages},
                 {"sample_rate_hz", 16000},
                 {"max_websocket_frame_bytes", 65536},
                 {"worker_processes", config.worker_processes},
                 {"max_sessions_per_process", config.max_sessions_per_process},
                 {"inference_slots_per_process", 1},
                 {"process_isolated", config.runtime == "qwen_native"},
                 {"hard_decode_watchdog", config.runtime == "qwen_native"},
                 {"cooperative_cancellation", manager->capabilities().cooperative_cancellation},
                 {"routes",
                  {"/v1/capabilities", "/v1/config/resolve", "/v1/runtime", "/v1/jobs", "/v1/history",
                   "/v1/artifacts/{id}/{file}", "/v1/reports", "/v1/reports/{id}", "/v1/suites/dry-run",
                   "/v1/suites", "/v1/jobs/{id}", "/v1/jobs/{id}/stop", "/v1/asr", "/v1/observe"}}},
                [config_path, overrides, config, &manager](const nlohmann::json &body, bool dry_run,
                                         const std::atomic<bool> &cancelled) {
                    if (!body.is_object())
                        throw std::invalid_argument("suite request must be a JSON object");
                    auto extra = body.value("overrides", std::vector<std::string>{});
                    auto all = overrides;
                    all.insert(all.end(), extra.begin(), extra.end());
                    auto effective = asr::resolve_config(config_path, all);
                    validate_shared_suite(config, effective);
                    effective.shared_model_loaded = effective.runtime == "qwen_prefix";
                    auto spec = asr::LoadSpec{};
                    spec.calls = body.value("calls", 2);
                    spec.concurrency = body.value("concurrency", 1);
                    spec.warmups = body.value("warmups", 0);
                    spec.repetitions = body.value("repetitions", 1);
                    spec.stagger_ms = body.value("stagger_ms", 0);
                    spec.languages = body.value("languages", std::vector<std::string>{effective.language});
                    spec.mode = body.value("mode", "direct");
                    spec.seed = effective.seed;
                    if (body.contains("max_failure_rate"))
                        spec.max_failure_rate = body.at("max_failure_rate").get<double>();
                    if (body.contains("max_p95_final_ms"))
                        spec.max_p95_final_ms = body.at("max_p95_final_ms").get<double>();
                    if (body.contains("max_p95_send_lag_ms"))
                        spec.max_p95_send_lag_ms = body.at("max_p95_send_lag_ms").get<double>();
                    if (body.contains("min_audio_throughput"))
                        spec.min_audio_throughput = body.at("min_audio_throughput").get<double>();
                    if (body.contains("max_peak_rss_bytes"))
                        spec.max_peak_rss_bytes = body.at("max_peak_rss_bytes").get<std::uint64_t>();
                    std::shared_ptr<const std::vector<std::int16_t>> pcm;
                    std::int64_t sample_count = std::int64_t(effective.duration_ms) * 16;
                    nlohmann::json metadata = {{"source", "synthetic"}, {"seed", effective.seed}};
                    if (effective.audio_source == "wav") {
                        asr::SincResampler resampler;
                        const auto mix = effective.channel_mix == "average" ? asr::ChannelMix::average
                                         : effective.channel_mix == "left"  ? asr::ChannelMix::left
                                                                            : asr::ChannelMix::reject;
                        auto prepared = asr::prepare_wav(effective.wav_path, mix, resampler);
                        pcm = prepared.pcm;
                        sample_count = pcm->size();
                        metadata = {{"source", "wav"}, {"path", effective.wav_path.string()}};
                    }
                    const auto memory = asr::linux_available_memory_bytes();
                    const auto kind = body.value("kind", "load");
                    if (kind == "load") {
                        const auto plan = asr::plan_load(effective, spec, sample_count, memory);
                        if (dry_run)
                            return plan.json();
                        if (!plan.allowed)
                            throw std::invalid_argument("load preflight rejected: " +
                                                        plan.preflight["skip_reasons"].dump());
                        auto run_manager = effective.runtime == "qwen_prefix" ?
                            std::unique_ptr<asr::IASREngine>{} : make_engine(effective);
                        std::unique_ptr<asr::WebSocketServer> server;
                        std::unique_ptr<asr::WebSocketEngine> network;
                        asr::IASREngine *ingress = run_manager ? run_manager.get() : manager.get();
                        if (spec.mode == "network") {
                            server = std::make_unique<asr::WebSocketServer>(*ingress);
                            network = std::make_unique<asr::WebSocketEngine>(server->port(),
                                                                             ingress->capabilities());
                            ingress = network.get();
                        }
                        return asr::run_load(effective, *ingress, plan, sample_count, pcm, metadata,
                                             &cancelled);
                    }
                    if (kind == "sweep") {
                        asr::SweepSpec sweep;
                        sweep.strategy = body.value("strategy", "baseline");
                        sweep.load = spec;
                        for (const auto &axis : body.value("axes", nlohmann::json::array()))
                            sweep.axes.push_back({axis.at("key").get<std::string>(),
                                                  axis.at("values").get<std::vector<std::string>>()});
                        sweep.selected = body.value("selected", std::vector<std::vector<std::string>>{});
                        auto plan = asr::plan_sweep(config_path, all, sweep, sample_count, memory);
                        if (effective.runtime == "qwen_prefix")
                            for (auto &item : plan.cases)
                                if (item.config) {
                                    validate_shared_suite(config, *item.config);
                                    item.config->shared_model_loaded = true;
                                    // Recompute memory admission against the already loaded model.
                                    const auto fresh = asr::plan_load(*item.config, item.load ? item.load->spec : spec,
                                                                      sample_count, memory);
                                    if (item.load && item.skip_reasons ==
                                        item.load->preflight["skip_reasons"].get<std::vector<std::string>>())
                                        item.skip_reasons = fresh.preflight["skip_reasons"].get<std::vector<std::string>>();
                                    item.load = fresh;
                                }
                        if (dry_run)
                            return plan.json();
                        return asr::run_sweep(
                            plan, effective.output_directory,
                            [&](const asr::RunConfig &selected, const asr::LoadPlan &load) {
                                validate_shared_suite(config, selected);
                                auto run_manager = selected.runtime == "qwen_prefix" ?
                                    std::unique_ptr<asr::IASREngine>{} : make_engine(selected);
                                std::unique_ptr<asr::WebSocketServer> server;
                                std::unique_ptr<asr::WebSocketEngine> network;
                                asr::IASREngine *ingress = run_manager ? run_manager.get() : manager.get();
                                if (load.spec.mode == "network") {
                                    server = std::make_unique<asr::WebSocketServer>(*ingress);
                                    network = std::make_unique<asr::WebSocketEngine>(
                                        server->port(), ingress->capabilities());
                                    ingress = network.get();
                                }
                                return asr::run_load(selected, *ingress, load, sample_count, pcm, metadata,
                                                     &cancelled);
                            },
                            &cancelled);
                    }
                    throw std::invalid_argument("suite kind must be load or sweep");
                },
                &gate,
                [&] {
                    nlohmann::json sample;
                    {
                        std::lock_guard lock(sampler_mutex);
                        sample = sampler.sample();
                    }
                    const auto workers = workers_json(*manager);
                    std::int64_t rss = 0;
                    for (const auto &process : sample["processes"])
                        rss += process["rss_bytes"].get<std::int64_t>();
                    return nlohmann::json{
                        {"schema_version", 1},
                        {"timestamp_ns", sample["timestamp_ns"]},
                        {"workers", workers},
                        {"system",
                         {{"host_cpu_percent", sample["cpu"]["cpu"]["utilization_percent"]},
                          {"host_memory_available_bytes",
                           sample["host_memory_bytes"].value("MemAvailable", 0LL)},
                          {"host_memory_total_bytes", sample["host_memory_bytes"].value("MemTotal", 0LL)},
                          {"sampled_process_tree_rss_bytes", rss},
                          {"queue_depth_available", config.runtime == "qwen_prefix"}}}};
                });
            asr::WebSocketServer server(*manager, std::uint16_t(serve_port), &api, &gate);
            std::signal(SIGINT, on_stop_signal);
            std::signal(SIGTERM, on_stop_signal);
            std::cout << nlohmann::json({{"protocol", "asr.websocket.v1"},
                                         {"host", "127.0.0.1"},
                                         {"port", server.port()},
                                         {"path", "/v1/asr"},
                                         {"rest", "/v1"}})
                             .dump()
                      << std::endl;
            while (!stop_server)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            drain_engine(*manager);
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
