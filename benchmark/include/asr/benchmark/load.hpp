#pragma once
#include <asr/backend/session_manager.hpp>
#include <asr/benchmark/baseline.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace asr {
struct LoadSpec {
    int calls = 2;
    int concurrency = 2;
    int warmups = 0;
    int repetitions = 1;
    int stagger_ms = 0;
    std::uint32_t seed = 42;
    std::vector<std::string> languages;
    std::string mode = "direct";
    // Absent thresholds are exploratory; an explicit zero is a valid strict bound.
    std::optional<double> max_failure_rate, max_p95_final_ms, max_p95_send_lag_ms;
    std::optional<double> min_audio_throughput;
    std::optional<std::uint64_t> max_peak_rss_bytes;
};
struct LoadCall {
    std::string id, language;
    int ordinal = 0, wave = 0, offset_ms = 0;
    bool warmup = false;
    int repetition = 0;
};
struct LoadPlan {
    LoadSpec spec;
    std::vector<LoadCall> calls;
    nlohmann::json preflight;
    bool allowed = false;
    nlohmann::json json() const;
};

// available_memory_bytes is supplied by the caller for deterministic tests.
LoadPlan plan_load(const RunConfig &config, const LoadSpec &spec, std::int64_t samples,
                   std::uint64_t available_memory_bytes);
std::uint64_t linux_available_memory_bytes();
nlohmann::json evaluate_load_slo(const LoadSpec &spec, const nlohmann::json &suite);

// Uses the same paced ingress, SessionManager and artifact path as single calls.
// The returned suite summary includes warmups but measures repetitions separately.
nlohmann::json run_load(const RunConfig &config, IASREngine &engine, const LoadPlan &plan,
                        std::int64_t samples, std::shared_ptr<const std::vector<std::int16_t>> pcm,
                        const nlohmann::json &audio_metadata,
                        const std::atomic<bool> *cancel_requested = nullptr);
} // namespace asr
