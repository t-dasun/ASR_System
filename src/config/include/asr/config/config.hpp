#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace asr {
struct RunConfig {
    std::string name = "mock_baseline", language = "en";
    std::uint32_t seed = 42;
    int chunk_ms = 200, duration_ms = 1200, partial_every_ms = 400;
    int queue_max_chunks = 8, queue_max_ms = 1000, queue_max_bytes = 32000;
    int max_lag_ms = 1000, late_tolerance_ms = 5;
    bool realtime = false;
    std::filesystem::path output_directory, wav_path, model_path;
    std::string runtime = "mock", audio_source = "synthetic", channel_mix = "reject";
    int native_threads = 4, decode_step_ms = 2000, max_new_tokens = 32, timeout_ms = 45000;
    int stream_unfixed_chunks = 0;
    int prefix_preview_ms = 4000, max_sessions_per_process = 1;
    // Internal preflight state; never accepted from YAML or API overrides.
    bool shared_model_loaded = false;
    bool refine_final = true;
    bool resource_sampling = true;
    int sample_interval_ms = 200;
    int worker_processes = 1, idle_timeout_ms = 30000, total_timeout_ms = 600000;
    std::string scheduler = "least_active";
    bool affinity_enabled = false;
    std::vector<int> cpu_cores;
    nlohmann::json resolved;
};
// Strict configuration schema. Throws a descriptive error for unknown/duplicate/unsafe values.
RunConfig resolve_config(const std::filesystem::path &yaml_file = {},
                         const std::vector<std::string> &overrides = {});
} // namespace asr
