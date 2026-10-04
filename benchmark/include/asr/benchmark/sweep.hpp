#pragma once
#include <asr/benchmark/load.hpp>
#include <functional>
#include <optional>

namespace asr {
struct SweepAxis {
    std::string key;
    std::vector<std::string> values;
};
struct SweepSpec {
    std::string strategy = "baseline"; // baseline, selected, oat, matrix, scale
    std::vector<SweepAxis> axes;
    std::vector<std::vector<std::string>> selected;
    LoadSpec load;
};
struct SweepCase {
    std::string id;
    int concurrency = 0;
    std::vector<std::string> overrides;
    std::optional<RunConfig> config;
    std::optional<LoadPlan> load;
    std::vector<std::string> skip_reasons;
    nlohmann::json json() const;
};
struct SweepPlan {
    SweepSpec spec;
    std::filesystem::path config_path;
    std::vector<std::string> base_overrides;
    std::int64_t samples = 0;
    std::uint64_t available_memory_bytes = 0;
    std::vector<SweepCase> cases;
    nlohmann::json json() const;
};
SweepPlan plan_sweep(const std::filesystem::path &config_path, const std::vector<std::string> &base_overrides,
                     const SweepSpec &spec, std::int64_t samples, std::uint64_t available_memory_bytes);

// The caller creates a fresh manager for each case. This keeps parameters and
// worker state isolated and permits deterministic model-free injected tests.
using SweepExecutor = std::function<nlohmann::json(const RunConfig &, const LoadPlan &)>;
nlohmann::json run_sweep(const SweepPlan &plan, const std::filesystem::path &output_root,
                         const SweepExecutor &executor, const std::atomic<bool> *cancel_requested = nullptr);
} // namespace asr
