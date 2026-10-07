#include "service.hpp"
#include "engine_runtime.hpp"
#include <asr/audio/wav.hpp>
#include <asr/backend/api_service.hpp>
#include <asr/backend/websocket.hpp>
#include <asr/benchmark/sweep.hpp>
#include <asr/observability/system_sampler.hpp>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

namespace asr::app {
namespace {
volatile std::sig_atomic_t stop_server = 0;
void on_stop_signal(int) { stop_server = 1; }
} // namespace

int run_service(const RunConfig &config, const std::filesystem::path &config_path,
                const std::vector<std::string> &overrides, const LoadSpec &load_spec, int serve_port) {
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
         {"session_decode_controls", true},
         {"shared_model", config.runtime == "qwen_prefix" || config.runtime == "qwen_stream"},
         {"stream_unfixed_chunks",
          config.runtime == "qwen_stream" ? nlohmann::json(config.stream_unfixed_chunks)
          : config.runtime == "qwen_native" ? nlohmann::json(2)
                                            : nlohmann::json(nullptr)},
         {"refine_final", config.refine_final},
         {"decode_step_ms", config.decode_step_ms},
         {"prefix_preview_ms", config.prefix_preview_ms},
         {"chunk_ms", config.chunk_ms},
         {"max_websocket_frame_bytes", 65536},
         {"worker_processes", config.worker_processes},
         {"max_sessions_per_process", config.max_sessions_per_process},
         {"manifest_inputs", load_spec.inputs.size()},
         {"worker_routing", config.scheduler},
         {"decode_scheduling",
          (config.runtime == "qwen_prefix" || config.runtime == "qwen_stream")
              ? (config.runtime == "qwen_stream" ? "oldest_ready_stream_step_requeue"
                                                 : "oldest_ready_preview_priority_max_2_then_eof")
              : "native_live"},
         {"inference_slots_per_process", 1},
         {"max_load_concurrency", asr::max_load_concurrency},
         {"process_isolated", config.runtime == "qwen_native" ||
                                  ((config.runtime == "qwen_prefix" || config.runtime == "qwen_stream") &&
                                   config.worker_processes > 1)},
         {"hard_decode_watchdog", config.runtime == "qwen_native"},
         {"cooperative_cancellation", manager->capabilities().cooperative_cancellation},
         {"routes",
          {"/v1/capabilities", "/v1/config/resolve", "/v1/runtime", "/v1/jobs", "/v1/history",
           "/v1/artifacts/{id}/{file}", "/v1/suites/dry-run", "/v1/suites", "/v1/jobs/{id}",
           "/v1/jobs/{id}/stop", "/v1/asr", "/v1/observe"}}},
        [config_path, overrides, config, load_spec, &manager](const nlohmann::json &body, bool dry_run,
                                                              const std::atomic<bool> &cancelled) {
            if (!body.is_object())
                throw std::invalid_argument("suite request must be a JSON object");
            auto extra = body.value("overrides", std::vector<std::string>{});
            auto all = overrides;
            all.insert(all.end(), extra.begin(), extra.end());
            auto effective = asr::resolve_config(config_path, all);
            validate_shared_suite(config, effective);
            effective.shared_model_loaded =
                (effective.runtime == "qwen_prefix" || effective.runtime == "qwen_stream");
            auto spec = asr::LoadSpec{};
            spec.inputs = load_spec.inputs;
            spec.calls = body.value("calls", 2);
            spec.concurrency = body.value("concurrency", 1);
            spec.warmups = body.value("warmups", 0);
            spec.repetitions = body.value("repetitions", 1);
            spec.stagger_ms = body.value("stagger_ms", 0);
            spec.languages = body.value("languages", std::vector<std::string>{effective.language});
            spec.mode = body.value("mode", "direct");
            spec.seed = effective.seed;
            if (!spec.inputs.empty() && body.value("kind", "load") != "load")
                throw std::invalid_argument(
                    "manifest service supports load suites; use single-WAV service for sweeps");
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
            if (effective.audio_source == "wav" && spec.inputs.empty()) {
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
                auto run_manager = (effective.runtime == "qwen_prefix" || effective.runtime == "qwen_stream")
                                       ? std::unique_ptr<asr::IASREngine>{}
                                       : make_engine(effective);
                std::unique_ptr<asr::WebSocketServer> server;
                std::unique_ptr<asr::WebSocketEngine> network;
                asr::IASREngine *ingress = run_manager ? run_manager.get() : manager.get();
                if (spec.mode == "network") {
                    server = std::make_unique<asr::WebSocketServer>(*ingress);
                    network = std::make_unique<asr::WebSocketEngine>(server->port(), ingress->capabilities());
                    ingress = network.get();
                }
                return asr::run_load(effective, *ingress, plan, sample_count, pcm, metadata, &cancelled);
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
                if ((effective.runtime == "qwen_prefix" || effective.runtime == "qwen_stream"))
                    for (auto &item : plan.cases)
                        if (item.config) {
                            validate_shared_suite(config, *item.config);
                            item.config->shared_model_loaded = true;
                            // Recompute memory admission against the already loaded model.
                            const auto fresh = asr::plan_load(
                                *item.config, item.load ? item.load->spec : spec, sample_count, memory);
                            if (item.load &&
                                item.skip_reasons ==
                                    item.load->preflight["skip_reasons"].get<std::vector<std::string>>())
                                item.skip_reasons =
                                    fresh.preflight["skip_reasons"].get<std::vector<std::string>>();
                            item.load = fresh;
                        }
                if (dry_run)
                    return plan.json();
                return asr::run_sweep(
                    plan, effective.output_directory,
                    [&](const asr::RunConfig &selected, const asr::LoadPlan &load) {
                        validate_shared_suite(config, selected);
                        auto run_manager =
                            (selected.runtime == "qwen_prefix" || selected.runtime == "qwen_stream")
                                ? std::unique_ptr<asr::IASREngine>{}
                                : make_engine(selected);
                        std::unique_ptr<asr::WebSocketServer> server;
                        std::unique_ptr<asr::WebSocketEngine> network;
                        asr::IASREngine *ingress = run_manager ? run_manager.get() : manager.get();
                        if (load.spec.mode == "network") {
                            server = std::make_unique<asr::WebSocketServer>(*ingress);
                            network = std::make_unique<asr::WebSocketEngine>(server->port(),
                                                                             ingress->capabilities());
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
                  {"host_memory_available_bytes", sample["host_memory_bytes"].value("MemAvailable", 0LL)},
                  {"host_memory_total_bytes", sample["host_memory_bytes"].value("MemTotal", 0LL)},
                  {"sampled_process_tree_rss_bytes", rss},
                  {"queue_depth_available",
                   (config.runtime == "qwen_prefix" || config.runtime == "qwen_stream")}}}};
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
} // namespace asr::app
