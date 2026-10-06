#include <asr/audio/wav.hpp>
#include <asr/benchmark/load.hpp>
#include <asr/observability/metrics.hpp>
#include <asr/observability/system_sampler.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <latch>
#include <limits>
#include <openssl/evp.h>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>

namespace asr {
namespace {
using Json = nlohmann::json;
void write_json(const std::filesystem::path &path, const Json &value) {
    std::ofstream stream(path);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << value.dump(2) << '\n';
}
std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
Json call_json(const LoadCall &call) {
    return {{"id", call.id},
            {"language", call.language},
            {"input_index", call.input_index},
            {"recording_id", call.recording_id},
            {"ordinal", call.ordinal},
            {"wave", call.wave},
            {"offset_ms", call.offset_ms},
            {"warmup", call.warmup},
            {"repetition", call.repetition}};
}
} // namespace
std::vector<LoadWavInput> prepare_load_manifest(const std::filesystem::path &path) {
    std::ifstream stream(path);
    if (!stream)
        throw std::invalid_argument("cannot open WAV manifest");
    std::vector<LoadWavInput> inputs;
    std::set<std::string> ids;
    std::size_t total_bytes = 0;
    std::string line;
    SincResampler resampler;
    while (std::getline(stream, line)) {
        if (line.empty())
            continue;
        if (inputs.size() >= 200 || line.size() > 65536)
            throw std::invalid_argument("WAV manifest bound exceeded");
        const auto row = Json::parse(line);
        LoadWavInput input;
        input.id = row.at("id");
        input.language = row.at("language");
        input.path = row.at("file").get<std::string>();
        if (input.id.empty() || !ids.insert(input.id).second ||
            (input.language != "en" && input.language != "id" && input.language != "zh"))
            throw std::invalid_argument("invalid or duplicate WAV manifest identity/language");
        if (input.path.is_relative() && !std::filesystem::exists(input.path))
            input.path = path.parent_path() / input.path;
        input.path = std::filesystem::absolute(input.path).lexically_normal();
        if (std::filesystem::file_size(input.path) > 128ULL * 1024 * 1024)
            throw std::invalid_argument("manifest WAV exceeds 128 MiB input bound");
        std::ifstream bytes(input.path, std::ios::binary);
        if (!bytes)
            throw std::invalid_argument("manifest WAV missing: " + input.path.string());
        auto context =
            std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("SHA256 initialization failed");
        char buffer[65536];
        while (bytes.read(buffer, sizeof(buffer)) || bytes.gcount())
            if (EVP_DigestUpdate(context.get(), buffer, bytes.gcount()) != 1)
                throw std::runtime_error("SHA256 update failed");
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned size = 0;
        if (bytes.bad() || EVP_DigestFinal_ex(context.get(), digest, &size) != 1)
            throw std::runtime_error("WAV hash failed");
        std::ostringstream hash;
        hash << std::hex << std::setfill('0');
        for (unsigned i = 0; i < size; ++i)
            hash << std::setw(2) << unsigned(digest[i]);
        if (row.contains("sha256") && row.at("sha256") != hash.str())
            throw std::invalid_argument("manifest WAV SHA256 mismatch: " + input.id);
        auto prepared = prepare_wav(input.path, ChannelMix::reject, resampler);
        input.pcm = prepared.pcm;
        if (row.contains("num_samples") && row.at("num_samples") != input.pcm->size())
            throw std::invalid_argument("manifest prepared sample count mismatch: " + input.id);
        if (input.pcm->empty())
            throw std::invalid_argument("empty manifest WAV");
        total_bytes += input.pcm->size() * sizeof(std::int16_t);
        if (total_bytes > 256ULL * 1024 * 1024)
            throw std::invalid_argument("prepared manifest exceeds 256 MiB PCM bound");
        input.metadata = {{"source", "wav"},
                          {"recording_id", input.id},
                          {"path", input.path.string()},
                          {"sha256", hash.str()},
                          {"reference", row.value("reference", std::string{})},
                          {"output_samples", input.pcm->size()},
                          {"input_rate_hz", prepared.metadata.input_rate},
                          {"input_channels", prepared.metadata.input_channels},
                          {"resampler", prepared.metadata.resampler}};
        inputs.push_back(std::move(input));
    }
    if (inputs.empty())
        throw std::invalid_argument("empty WAV manifest");
    return inputs;
}
Json LoadPlan::json() const {
    Json inputs = Json::array();
    for (auto &input : spec.inputs)
        inputs.push_back(input.metadata);
    Json items = Json::array();
    for (const auto &call : calls)
        items.push_back(call_json(call));
    return {
        {"schema_version", 1},
        {"inputs", inputs},
        {"allowed", allowed},
        {"spec",
         {{"calls", spec.calls},
          {"concurrency", spec.concurrency},
          {"warmups", spec.warmups},
          {"repetitions", spec.repetitions},
          {"stagger_ms", spec.stagger_ms},
          {"seed", spec.seed},
          {"languages", spec.languages},
          {"mode", spec.mode},
          {"max_failure_rate", spec.max_failure_rate ? Json(*spec.max_failure_rate) : Json(nullptr)},
          {"max_p95_final_ms", spec.max_p95_final_ms ? Json(*spec.max_p95_final_ms) : Json(nullptr)},
          {"max_p95_send_lag_ms", spec.max_p95_send_lag_ms ? Json(*spec.max_p95_send_lag_ms) : Json(nullptr)},
          {"min_audio_throughput",
           spec.min_audio_throughput ? Json(*spec.min_audio_throughput) : Json(nullptr)},
          {"max_peak_rss_bytes", spec.max_peak_rss_bytes ? Json(*spec.max_peak_rss_bytes) : Json(nullptr)}}},
        {"preflight", preflight},
        {"calls", items}};
}
std::uint64_t linux_available_memory_bytes() {
    std::ifstream stream("/proc/meminfo");
    std::string key, unit;
    std::uint64_t kib = 0;
    while (stream >> key >> kib >> unit)
        if (key == "MemAvailable:") {
            const auto host_available = kib * 1024;
            std::ifstream limit_file("/sys/fs/cgroup/memory.max");
            std::ifstream current_file("/sys/fs/cgroup/memory.current");
            std::string limit_text;
            std::uint64_t current = 0;
            if (limit_file >> limit_text && current_file >> current && limit_text != "max") {
                try {
                    const auto limit = std::stoull(limit_text);
                    return std::min<std::uint64_t>(host_available,
                                                   limit > current ? limit - current : std::uint64_t{0});
                } catch (const std::exception &) {
                    throw std::runtime_error("invalid cgroup memory limit");
                }
            }
            return host_available;
        }
    throw std::runtime_error("cannot read MemAvailable from /proc/meminfo");
}
LoadPlan plan_load(const RunConfig &config, const LoadSpec &spec, std::int64_t samples,
                   std::uint64_t available_memory_bytes) {
    std::vector<int> eligible;
    std::uint64_t corpus_bytes = 0;
    for (std::size_t i = 0; i < spec.inputs.size(); ++i) {
        const auto &input = spec.inputs[i];
        if (!input.pcm || input.pcm->empty())
            throw std::invalid_argument("unprepared manifest input");
        corpus_bytes += input.pcm->size() * sizeof(std::int16_t);
        if (std::find(spec.languages.begin(), spec.languages.end(), input.language) != spec.languages.end()) {
            eligible.push_back(static_cast<int>(i));
            samples = std::max(samples, static_cast<std::int64_t>(input.pcm->size()));
        }
    }
    if (!spec.inputs.empty() && eligible.empty())
        throw std::invalid_argument("no manifest WAVs match requested languages");
    const auto valid_nonnegative = [](const std::optional<double> &value) {
        return !value || (std::isfinite(*value) && *value >= 0);
    };
    if (spec.calls < 1 || spec.calls > 1000 || spec.concurrency < 1 || spec.concurrency > 16 ||
        spec.warmups < 0 || spec.warmups > 20 || spec.repetitions < 1 || spec.repetitions > 20 ||
        spec.stagger_ms < 0 || spec.stagger_ms > 60000 || spec.languages.empty() ||
        spec.languages.size() > 3 || samples < 0 || samples > 16000LL * 3600 ||
        !valid_nonnegative(spec.max_failure_rate) || (spec.max_failure_rate && *spec.max_failure_rate > 1) ||
        !valid_nonnegative(spec.max_p95_final_ms) || !valid_nonnegative(spec.max_p95_send_lag_ms) ||
        !valid_nonnegative(spec.min_audio_throughput) || (spec.mode != "direct" && spec.mode != "network"))
        throw std::invalid_argument("load specification exceeds bounded M6 limits");
    for (const auto &language : spec.languages)
        if (language != "en" && language != "id" && language != "zh")
            throw std::invalid_argument("load language must be en, id, or zh");
    LoadPlan plan;
    plan.spec = spec;
    std::vector<std::string> reasons;
    const bool prefix = config.runtime == "qwen_prefix";
    const int slots = config.worker_processes * config.max_sessions_per_process;
    if (spec.concurrency > slots)
        reasons.emplace_back("target concurrency exceeds configured active call slots");
    if (!config.realtime)
        reasons.emplace_back("load measurement requires host steady-clock pacing");
    if (config.runtime != "mock" && config.audio_source != "wav")
        reasons.emplace_back("native load requires real WAV audio");
    if (prefix && samples > 16000LL * 60)
        reasons.emplace_back("qwen_prefix call exceeds 60-second audio bound");
    const auto total_calls = std::uint64_t(spec.calls) * (spec.warmups + spec.repetitions);
    const auto chunks =
        (samples + std::int64_t(config.chunk_ms) * 16 - 1) / (std::int64_t(config.chunk_ms) * 16);
    const auto estimated_disk = total_calls * (262144ULL + std::uint64_t(chunks) * 2048ULL);
    const auto required_memory =
        corpus_bytes +
        (prefix ? (2ULL + (config.shared_model_loaded ? 0ULL : 3ULL * config.worker_processes)) * 1024 *
                          1024 * 1024 +
                      std::uint64_t(spec.concurrency) * 16ULL * 1024 * 1024
         : config.runtime == "qwen_native"
             ? 2ULL * 1024 * 1024 * 1024 + std::uint64_t(spec.concurrency) * 3ULL * 1024 * 1024 * 1024
             : 256ULL * 1024 * 1024 + std::uint64_t(spec.concurrency) * 64ULL * 1024 * 1024);
    if (available_memory_bytes < required_memory)
        reasons.emplace_back("available memory below conservative worker-plus-reserve estimate");
    if (total_calls > 2000)
        reasons.emplace_back("run count exceeds 2000-call safety bound");
    std::filesystem::path probe = config.output_directory;
    while (!probe.empty() && !std::filesystem::exists(probe))
        probe = probe.parent_path();
    std::uint64_t available_disk = 0;
    if (!probe.empty()) {
        std::error_code error;
        const auto capacity = std::filesystem::space(probe, error);
        if (!error)
            available_disk = capacity.available;
    }
    if (available_disk == 0)
        reasons.emplace_back("output filesystem free space unavailable");
    else if (available_disk < estimated_disk + 100ULL * 1024 * 1024)
        reasons.emplace_back("output filesystem lacks estimated artifacts plus 100 MiB reserve");
    const double audio_seconds = double(samples) / 16000.0;
    const auto waves = (spec.calls + spec.concurrency - 1) / spec.concurrency;
    const auto estimated_seconds =
        double(waves) * (spec.warmups + spec.repetitions) *
        (audio_seconds + (config.runtime == "mock" ? 0.1 : 5.0 * (prefix ? spec.concurrency : 1)));
    if (estimated_seconds > 7200)
        reasons.emplace_back("estimated run duration exceeds two-hour safety bound");
    std::mt19937 rng(spec.seed);
    std::uniform_int_distribution<std::size_t> choose(0, spec.languages.size() - 1);
    for (int phase = 0; phase < spec.warmups + spec.repetitions; ++phase)
        for (int ordinal = 0; ordinal < spec.calls; ++ordinal) {
            LoadCall call;
            call.warmup = phase < spec.warmups;
            call.repetition = call.warmup ? phase : phase - spec.warmups;
            call.ordinal = ordinal;
            call.wave = ordinal / spec.concurrency;
            call.offset_ms = ordinal < spec.concurrency ? ordinal * spec.stagger_ms : 0;
            if (eligible.empty())
                call.language = spec.languages[choose(rng)];
            else {
                call.input_index = eligible[ordinal % eligible.size()];
                call.language = spec.inputs[call.input_index].language;
                call.recording_id = spec.inputs[call.input_index].id;
            }
            call.id = std::string(call.warmup ? "warmup_" : "repeat_") + std::to_string(call.repetition) +
                      "_call_" + std::to_string(ordinal);
            plan.calls.push_back(std::move(call));
        }
    double total_audio_seconds = 0;
    for (const auto &call : plan.calls)
        total_audio_seconds += call.input_index < 0
                                   ? audio_seconds
                                   : double(spec.inputs[call.input_index].pcm->size()) / 16000.0;
    plan.allowed = reasons.empty();
    plan.preflight = {{"skip_reasons", reasons},
                      {"total_calls", total_calls},
                      {"total_audio_seconds", total_audio_seconds},
                      {"estimated_duration_seconds", estimated_seconds},
                      {"estimated_disk_bytes", estimated_disk},
                      {"available_disk_bytes", available_disk},
                      {"available_memory_bytes", available_memory_bytes},
                      {"required_memory_bytes", required_memory},
                      {"worker_processes", config.worker_processes},
                      {"active_call_slots", slots},
                      {"shared_model_already_loaded", prefix && config.shared_model_loaded},
                      {"memory_estimate_kind", prefix ? "shared_model_scratch_plus_reserve_and_call_buffers"
                                                      : "isolated_workers_plus_reserve"},
                      {"per_call_chunks", chunks},
                      {"manifest_unique_inputs", eligible.size()},
                      {"manifest_pcm_bytes", corpus_bytes},
                      {"mode", spec.mode},
                      {"network_mode", "loopback_websocket_v1"}};
    return plan;
}
Json evaluate_load_slo(const LoadSpec &spec, const Json &suite) {
    const auto &metrics = suite.at("metrics");
    Json violations = Json::array();
    const bool declared = spec.max_failure_rate || spec.max_p95_final_ms || spec.max_p95_send_lag_ms ||
                          spec.min_audio_throughput || spec.max_peak_rss_bytes;
    if (spec.max_failure_rate && metrics["failure_rate"].get<double>() > *spec.max_failure_rate)
        violations.push_back("failure_rate");
    const auto final_p95 = metrics["final_result_ms"]["p95"];
    if (spec.max_p95_final_ms && (final_p95.is_null() || final_p95.get<double>() > *spec.max_p95_final_ms))
        violations.push_back("p95_final_ms");
    const auto lag_p95 = metrics["call_max_send_lag_ms"]["p95"];
    if (spec.max_p95_send_lag_ms && (lag_p95.is_null() || lag_p95.get<double>() > *spec.max_p95_send_lag_ms))
        violations.push_back("p95_send_lag_ms");
    if (spec.min_audio_throughput &&
        metrics["audio_seconds_per_wall_second"].get<double>() < *spec.min_audio_throughput)
        violations.push_back("audio_throughput");
    if (spec.max_peak_rss_bytes &&
        (metrics["sampled_peak_tree_rss_bytes"].is_null() ||
         metrics["sampled_peak_tree_rss_bytes"].get<std::uint64_t>() > *spec.max_peak_rss_bytes))
        violations.push_back("peak_rss_bytes");
    const auto offered = metrics["offered_calls"].get<int>();
    return {{"profile_declared", declared},
            {"violations", violations},
            {"thresholds_met", declared && violations.empty()},
            {"tail_population_sufficient", offered >= 20},
            {"qualified", declared && violations.empty() && offered >= 20 &&
                              suite.value("status", "FAILED") == "COMPLETE" && !suite.value("is_mock", true)},
            {"note", offered < 20 ? "Fewer than 20 measured calls: tail estimate is screening only"
                                  : "Measured-call population; warmups excluded"}};
}
Json run_load(const RunConfig &config, IASREngine &engine, const LoadPlan &plan, std::int64_t samples,
              std::shared_ptr<const std::vector<std::int16_t>> pcm, const Json &audio_metadata,
              const std::atomic<bool> *cancel_requested) {
    if (!plan.allowed)
        throw std::invalid_argument("preflight rejected load plan");
    if (plan.spec.inputs.empty() && config.audio_source == "wav" &&
        (!pcm || std::int64_t(pcm->size()) != samples))
        throw std::invalid_argument("load WAV buffer/sample count mismatch");
    const auto suite_id = new_run_id("load");
    const auto suite_dir = config.output_directory / suite_id;
    std::filesystem::create_directories(config.output_directory);
    if (!std::filesystem::is_directory(config.output_directory)) {
        throw std::runtime_error("cannot create load output directory");
    }
    if (!std::filesystem::create_directory(suite_dir))
        throw std::runtime_error("load suite directory already exists");
    write_json(suite_dir / "plan.json", plan.json());
    write_json(suite_dir / "status.json", {{"status", "RUNNING"}, {"suite_id", suite_id}});
    try {
        Json phases = Json::array();
        int completed = 0, failed = 0, measurement_failures = 0;
        for (int phase = 0; phase < plan.spec.warmups + plan.spec.repetitions; ++phase) {
            std::vector<const LoadCall *> phase_calls;
            for (const auto &call : plan.calls)
                if ((call.warmup && phase == call.repetition) ||
                    (!call.warmup && phase == plan.spec.warmups + call.repetition))
                    phase_calls.push_back(&call);
            const int parallel = std::min<int>(plan.spec.concurrency, phase_calls.size());
            std::vector<Json> results(phase_calls.size());
            std::atomic<std::size_t> next{0};
            std::latch ready(parallel), start(1);
            std::vector<std::thread> threads;
            threads.reserve(parallel);
            std::int64_t phase_start_ns = 0;
            std::unique_ptr<ResourceMonitor> monitor;
            if (config.resource_sampling)
                monitor = std::make_unique<ResourceMonitor>(std::make_unique<LinuxSystemSampler>(),
                                                            config.sample_interval_ms);
            for (int t = 0; t < parallel; ++t)
                threads.emplace_back([&, t] {
                    asr::SteadyClock clock;
                    ready.count_down();
                    start.wait();
                    while (true) {
                        const auto index = next.fetch_add(1);
                        if (index >= phase_calls.size())
                            break;
                        const auto &call = *phase_calls[index];
                        if (cancel_requested && cancel_requested->load()) {
                            results[index] = {{"call", call_json(call)}, {"status", "STOPPED"}};
                            continue;
                        }
                        if (index < std::size_t(parallel) && call.offset_ms > 0)
                            std::this_thread::sleep_until(
                                std::chrono::steady_clock::time_point(std::chrono::nanoseconds(
                                    phase_start_ns + std::int64_t(call.offset_ms) * 1000000)));
                        auto effective = config;
                        effective.language = call.language;
                        effective.seed = config.seed + std::uint32_t(phase * plan.spec.calls + call.ordinal);
                        effective.resource_sampling = false; // Shared process tree is sampled once per phase.
                        effective.resolved["dataset"]["language"] = call.language;
                        effective.resolved["experiment"]["seed"] = effective.seed;
                        effective.resolved["metrics"]["resource_sampling"] = false;
                        effective.resolved["metrics"]["suite_level_sampling"] = config.resource_sampling;
                        effective.resolved["load"]["mode"] = plan.spec.mode;
                        const auto run_id = suite_id + "_" + call.id;
                        FileResultRepository repository(suite_dir);
                        try {
                            std::unique_ptr<IAudioSource> source;
                            auto call_pcm = pcm;
                            auto call_samples = samples;
                            auto call_metadata = audio_metadata;
                            if (call.input_index >= 0) {
                                const auto &input = plan.spec.inputs.at(call.input_index);
                                call_pcm = input.pcm;
                                call_samples = input.pcm->size();
                                call_metadata = input.metadata;
                                effective.wav_path = input.path;
                                effective.audio_source = "wav";
                                effective.resolved["audio"]["source"] = "wav";
                                effective.resolved["audio"]["path"] = input.path.string();
                                effective.resolved["audio"]["recording_id"] = input.id;
                            }
                            if (call_pcm)
                                source = std::make_unique<BufferPcmSource>(call_pcm);
                            else
                                source = std::make_unique<SyntheticPcmSource>(call_samples, effective.seed);
                            auto summary = run_baseline(effective, engine, *source, clock, repository, run_id,
                                                        call_samples, call_metadata);
                            results[index] = {{"call", call_json(call)},
                                              {"run_id", run_id},
                                              {"directory", repository.directory().string()},
                                              {"status", summary.value("status", "FAILED")},
                                              {"summary", summary}};
                        } catch (const std::exception &error) {
                            results[index] = {{"call", call_json(call)},
                                              {"run_id", run_id},
                                              {"directory", repository.directory().string()},
                                              {"status", "FAILED"},
                                              {"error", error.what()}};
                        }
                    }
                });
            ready.wait();
            phase_start_ns = now_ns();
            start.count_down();
            for (auto &thread : threads)
                thread.join();
            Json phase_json = {{"phase", phase},
                               {"warmup", phase < plan.spec.warmups},
                               {"measurement_start_ns", phase_start_ns},
                               {"measurement_end_ns", now_ns()},
                               {"calls", results}};
            if (monitor) {
                monitor->stop();
                phase_json["resources"] = monitor->summary();
                const auto raw_path =
                    suite_dir / ("phase_" + std::to_string(phase) + "_system_metrics.jsonl");
                std::ofstream raw(raw_path);
                raw.exceptions(std::ios::badbit | std::ios::failbit);
                for (const auto &sample : monitor->samples())
                    raw << sample.dump() << '\n';
                raw.close();
                phase_json["resource_samples_path"] = raw_path.filename().string();
                if (!phase_json["resources"].value("measurement_valid", false))
                    ++measurement_failures;
            }
            for (const auto &result : results)
                if (result.value("status", "FAILED") == "COMPLETE")
                    ++completed;
                else
                    ++failed;
            phases.push_back(std::move(phase_json));
        }
        int measured_offered = 0, measured_completed = 0;
        double measured_audio_seconds = 0, measured_wall_seconds = 0;
        std::vector<double> final_ms, first_ms, lag_ms, rtf;
        std::uint64_t peak_rss = 0;
        bool rss_measured = false;
        for (const auto &phase : phases) {
            if (phase["warmup"].get<bool>())
                continue;
            measured_wall_seconds += (phase["measurement_end_ns"].get<std::int64_t>() -
                                      phase["measurement_start_ns"].get<std::int64_t>()) /
                                     1e9;
            if (phase.contains("resources") && phase["resources"].value("measurement_valid", false)) {
                rss_measured = true;
                peak_rss = std::max(peak_rss,
                                    phase["resources"]["sampled_peak_tree_rss_bytes"].get<std::uint64_t>());
            }
            for (const auto &result : phase["calls"]) {
                ++measured_offered;
                if (result.value("status", "FAILED") != "COMPLETE")
                    continue;
                ++measured_completed;
                const auto &summary = result["summary"];
                measured_audio_seconds += summary.value("audio_seconds", 0.0);
                const auto &m = summary["measurements"];
                if (!m["final_result_ns"].is_null())
                    final_ms.push_back(m["final_result_ns"].get<double>() / 1e6);
                if (!m["first_usable_transcript_ns"].is_null())
                    first_ms.push_back(m["first_usable_transcript_ns"].get<double>() / 1e6);
                if (!m["effective_rtf"].is_null())
                    rtf.push_back(m["effective_rtf"].get<double>());
                lag_ms.push_back(summary["max_send_lag_ns"].get<double>() / 1e6);
            }
        }
        Json metrics = {
            {"offered_calls", measured_offered},
            {"completed_calls", measured_completed},
            {"failure_rate",
             measured_offered ? double(measured_offered - measured_completed) / measured_offered : 0.0},
            {"completed_audio_seconds", measured_audio_seconds},
            {"measurement_wall_seconds", measured_wall_seconds},
            {"audio_seconds_per_wall_second",
             measured_wall_seconds > 0 ? measured_audio_seconds / measured_wall_seconds : 0.0},
            {"completed_calls_per_minute",
             measured_wall_seconds > 0 ? measured_completed * 60.0 / measured_wall_seconds : 0.0},
            {"final_result_ms", distribution(final_ms, "completed calls", "ms")},
            {"first_usable_ms", distribution(first_ms, "completed calls with text", "ms")},
            {"call_max_send_lag_ms", distribution(lag_ms, "completed-call maxima", "ms")},
            {"effective_rtf", distribution(rtf, "completed calls", "ratio")},
            {"sampled_peak_tree_rss_bytes", rss_measured ? Json(peak_rss) : Json(nullptr)}};
        Json suite = {{"schema_version", 1},
                      {"suite_id", suite_id},
                      {"mode", plan.spec.mode},
                      {"is_mock", engine.capabilities().is_mock},
                      {"status", cancel_requested && cancel_requested->load() ? "STOPPED"
                                 : failed || measurement_failures             ? "FAILED"
                                                                              : "COMPLETE"},
                      {"completed_calls", completed},
                      {"failed_calls", failed},
                      {"measurement_failures", measurement_failures},
                      {"metrics", metrics},
                      {"phases", phases},
                      {"preflight", plan.preflight}};
        suite["slo"] = evaluate_load_slo(plan.spec, suite);
        write_json(suite_dir / "summary.json", suite);
        write_json(suite_dir / "status.next.json", {{"status", suite["status"]}, {"suite_id", suite_id}});
        std::filesystem::rename(suite_dir / "status.next.json", suite_dir / "status.json");
        suite["directory"] = suite_dir.string();
        return suite;
    } catch (const std::exception &error) {
        try {
            write_json(suite_dir / "orchestration_error.json", {{"message", error.what()}});
            write_json(suite_dir / "status.next.json",
                       {{"status", "FAILED"}, {"suite_id", suite_id}, {"reason", "orchestration exception"}});
            std::filesystem::rename(suite_dir / "status.next.json", suite_dir / "status.json");
        } catch (...) {
        }
        throw;
    }
}
} // namespace asr
