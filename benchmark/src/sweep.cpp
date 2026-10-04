#include <algorithm>
#include <asr/benchmark/sweep.hpp>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>

namespace asr {
namespace {
using Json = nlohmann::json;
void write_json(const std::filesystem::path &path, const Json &value) {
    std::ofstream stream(path);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << value.dump(2) << '\n';
}
void checkpoint(const std::filesystem::path &directory, const Json &summary) {
    write_json(directory / "summary.next.json", summary);
    std::filesystem::rename(directory / "summary.next.json", directory / "summary.json");
}
bool valid_axis(const std::string &key) {
    static const std::set<std::string> keys{"model.threads",
                                            "workers.processes",
                                            "audio.chunk_ms",
                                            "model.decode_step_ms",
                                            "model.max_new_tokens",
                                            "workers.scheduler",
                                            "metrics.sample_interval_ms",
                                            "metrics.resource_sampling"};
    return keys.contains(key);
}
SweepCase make_case(const SweepPlan &plan, std::string id, std::vector<std::string> overrides,
                    int concurrency) {
    SweepCase result;
    result.id = std::move(id);
    result.overrides = overrides;
    result.concurrency = concurrency;
    auto all = plan.base_overrides;
    all.insert(all.end(), overrides.begin(), overrides.end());
    try {
        auto config = resolve_config(plan.config_path, all);
        if (config.runtime == "mock")
            for (const auto &override : overrides)
                if (override.starts_with("model.threads=") || override.starts_with("model.decode_step_ms=") ||
                    override.starts_with("model.max_new_tokens="))
                    result.skip_reasons.push_back("native model axis has no effect on mock engine");
        auto load = plan.spec.load;
        if (concurrency > 0) {
            load.concurrency = concurrency;
            load.calls = std::max(load.calls, concurrency * 2);
        }
        auto load_plan = plan_load(config, load, plan.samples, plan.available_memory_bytes);
        if (!load_plan.allowed)
            for (const auto &reason : load_plan.preflight["skip_reasons"])
                result.skip_reasons.push_back(reason.get<std::string>());
        result.config = std::move(config);
        result.load = std::move(load_plan);
    } catch (const std::exception &error) {
        result.skip_reasons.push_back(error.what());
    }
    return result;
}
Json run_case(const SweepCase &item, std::int64_t samples, const std::filesystem::path &directory,
              const SweepExecutor &executor) {
    if (!item.skip_reasons.empty() || !item.config || !item.load)
        return {{"id", item.id},
                {"concurrency", item.concurrency},
                {"status", "SKIPPED"},
                {"reasons", item.skip_reasons}};
    auto config = *item.config;
    config.output_directory = directory;
    config.resolved["output"]["directory"] = directory.string();
    try {
        const auto fresh = plan_load(config, item.load->spec, samples, linux_available_memory_bytes());
        if (!fresh.allowed)
            return {{"id", item.id},
                    {"concurrency", item.concurrency},
                    {"status", "SKIPPED"},
                    {"reasons", fresh.preflight["skip_reasons"]},
                    {"note", "resource preflight changed before execution"}};
        const auto summary = executor(config, fresh);
        return {{"id", item.id},
                {"concurrency", item.concurrency},
                {"runtime", config.runtime},
                {"status", summary.value("status", "FAILED")},
                {"suite_directory", summary.value("directory", "")},
                {"metrics", summary.value("metrics", Json::object())},
                {"slo", summary.value("slo", Json::object())}};
    } catch (const std::exception &error) {
        return {{"id", item.id},
                {"concurrency", item.concurrency},
                {"status", "FAILED"},
                {"error", error.what()}};
    }
}
bool passes(const Json &result) {
    if (result.value("status", "FAILED") != "COMPLETE")
        return false;
    const auto &slo = result.value("slo", Json::object());
    return !slo.value("profile_declared", false) || slo.value("thresholds_met", false);
}
} // namespace
Json SweepCase::json() const {
    return {{"id", id},
            {"concurrency", concurrency},
            {"overrides", overrides},
            {"skip_reasons", skip_reasons},
            {"effective_config", config ? config->resolved : Json(nullptr)},
            {"load", load ? load->json() : Json(nullptr)}};
}
Json SweepPlan::json() const {
    Json items = Json::array(), axes = Json::array();
    for (const auto &item : cases)
        items.push_back(item.json());
    for (const auto &axis : spec.axes)
        axes.push_back({{"key", axis.key}, {"values", axis.values}});
    return {{"schema_version", 1},
            {"strategy", spec.strategy},
            {"config_path", config_path.string()},
            {"base_overrides", base_overrides},
            {"axes", axes},
            {"selected", spec.selected},
            {"seed", spec.load.seed},
            {"cases", items},
            {"case_count", items.size()},
            {"network_mode", "loopback_websocket_v1"}};
}
SweepPlan plan_sweep(const std::filesystem::path &config_path, const std::vector<std::string> &base_overrides,
                     const SweepSpec &spec, std::int64_t samples, std::uint64_t available_memory_bytes) {
    if (spec.strategy != "baseline" && spec.strategy != "selected" && spec.strategy != "oat" &&
        spec.strategy != "matrix" && spec.strategy != "scale")
        throw std::invalid_argument("sweep strategy must be baseline, selected, oat, matrix, or scale");
    if (spec.axes.size() > 4 || spec.selected.size() > 64)
        throw std::invalid_argument("sweep has too many axes or cases");
    SweepPlan plan{spec, config_path, base_overrides, samples, available_memory_bytes, {}};
    if (spec.strategy == "baseline")
        plan.cases.push_back(make_case(plan, "baseline", {}, 0));
    else if (spec.strategy == "selected") {
        if (spec.selected.empty())
            throw std::invalid_argument("selected strategy needs --case");
        for (std::size_t i = 0; i < spec.selected.size(); ++i)
            plan.cases.push_back(make_case(plan, "selected_" + std::to_string(i), spec.selected[i], 0));
    } else if (spec.strategy == "scale") {
        if (spec.axes.size() != 1 || spec.axes[0].key != "concurrency")
            throw std::invalid_argument("scale strategy needs --axis concurrency=1,2,...");
        std::set<int> levels;
        for (const auto &text : spec.axes[0].values) {
            const auto level = std::stoi(text);
            if (level < 1 || level > 16 || !levels.insert(level).second)
                throw std::invalid_argument("scale levels must be unique in 1..16");
        }
        for (const auto level : levels)
            plan.cases.push_back(make_case(plan, "scale_" + std::to_string(level),
                                           {"workers.processes=" + std::to_string(level)}, level));
    } else {
        if (spec.axes.empty())
            throw std::invalid_argument("oat/matrix strategy needs at least one --axis");
        for (const auto &axis : spec.axes)
            if (!valid_axis(axis.key) || axis.values.empty() || axis.values.size() > 8)
                throw std::invalid_argument("unsupported or oversized sweep axis: " + axis.key);
        if (spec.strategy == "oat") {
            plan.cases.push_back(make_case(plan, "baseline", {}, 0));
            for (const auto &axis : spec.axes)
                for (const auto &value : axis.values) {
                    auto item = make_case(plan, "oat_" + std::to_string(plan.cases.size()),
                                          {axis.key + "=" + value}, 0);
                    if (item.config && plan.cases[0].config &&
                        item.config->resolved == plan.cases[0].config->resolved)
                        item.skip_reasons.push_back("no effective change from baseline");
                    plan.cases.push_back(std::move(item));
                }
        } else {
            std::vector<std::vector<std::string>> combinations{{}};
            for (const auto &axis : spec.axes) {
                std::vector<std::vector<std::string>> expanded;
                for (const auto &existing : combinations)
                    for (const auto &value : axis.values) {
                        auto next = existing;
                        next.push_back(axis.key + "=" + value);
                        expanded.push_back(std::move(next));
                        if (expanded.size() > 64)
                            throw std::invalid_argument("matrix exceeds 64 cases");
                    }
                combinations = std::move(expanded);
            }
            for (const auto &combination : combinations)
                plan.cases.push_back(
                    make_case(plan, "matrix_" + std::to_string(plan.cases.size()), combination, 0));
        }
    }
    if (plan.cases.size() > 64)
        throw std::invalid_argument("sweep exceeds 64 cases");
    if (spec.strategy == "oat" || spec.strategy == "matrix") {
        const auto begin = plan.cases.begin() + (spec.strategy == "oat" ? 1 : 0);
        std::mt19937 rng(spec.load.seed);
        std::shuffle(begin, plan.cases.end(), rng);
    }
    return plan;
}
Json run_sweep(const SweepPlan &plan, const std::filesystem::path &output_root, const SweepExecutor &executor,
               const std::atomic<bool> *cancel_requested) {
    if (!executor)
        throw std::invalid_argument("sweep executor required");
    std::filesystem::create_directories(output_root);
    const auto id = new_run_id("sweep");
    const auto directory = output_root / id;
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("sweep directory already exists");
    write_json(directory / "plan.json", plan.json());
    Json records = Json::array();
    Json result{{"schema_version", 1},
                {"sweep_id", id},
                {"strategy", plan.spec.strategy},
                {"status", "RUNNING"},
                {"cases", records}};
    checkpoint(directory, result);
    int last_pass = 0, first_fail = 0;
    bool safety_boundary = false;
    for (const auto &item : plan.cases) {
        if (cancel_requested && cancel_requested->load())
            break;
        auto row = run_case(item, plan.samples, directory, executor);
        records.push_back(row);
        result["cases"] = records;
        checkpoint(directory, result);
        if (plan.spec.strategy != "scale")
            continue;
        if (row.value("status", "FAILED") == "SKIPPED") {
            safety_boundary = true;
            break;
        }
        if (passes(row))
            last_pass = item.concurrency;
        else {
            first_fail = item.concurrency;
            break;
        }
    }
    if (plan.spec.strategy == "scale" && first_fail > 0 && last_pass > 0) {
        while (first_fail - last_pass > 1) {
            if (cancel_requested && cancel_requested->load())
                break;
            const int middle = last_pass + (first_fail - last_pass) / 2;
            auto refined = make_case(plan, "refine_" + std::to_string(middle),
                                     {"workers.processes=" + std::to_string(middle)}, middle);
            auto row = run_case(refined, plan.samples, directory, executor);
            records.push_back(row);
            result["cases"] = records;
            checkpoint(directory, result);
            if (row.value("status", "FAILED") == "SKIPPED") {
                safety_boundary = true;
                break;
            }
            if (passes(row))
                last_pass = middle;
            else
                first_fail = middle;
        }
    }
    int completed = 0, failed = 0, skipped = 0;
    for (const auto &row : records) {
        const auto status = row.value("status", "FAILED");
        completed += status == "COMPLETE";
        failed += status == "FAILED";
        skipped += status == "SKIPPED";
    }
    result["completed_cases"] = completed;
    result["failed_cases"] = failed;
    result["skipped_cases"] = skipped;
    result["status"] = cancel_requested && cancel_requested->load() ? "STOPPED"
                       : std::size_t(skipped) == records.size()     ? "SKIPPED"
                       : failed                                     ? "FAILED"
                                                                    : "COMPLETE";
    result["saturation"] = {
        {"last_passing_concurrency", last_pass ? Json(last_pass) : Json(nullptr)},
        {"first_failing_concurrency", first_fail ? Json(first_fail) : Json(nullptr)},
        {"safety_boundary", safety_boundary},
        {"qualified_capacity", false},
        {"note", "Exploratory first-failure screening; formal capacity qualification belongs to M10"}};
    checkpoint(directory, result);
    result["directory"] = directory.string();
    return result;
}
} // namespace asr
