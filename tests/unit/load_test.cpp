#include <asr/benchmark/load.hpp>
#include <asr/engines/mock_engine.hpp>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace
int main() {
    try {
        auto config = asr::resolve_config({}, {"audio.duration_ms=200", "audio.realtime_pacing=true",
                                               "workers.processes=2", "metrics.resource_sampling=false"});
        const auto root =
            std::filesystem::temp_directory_path() / ("asr_m6_load_" + std::to_string(getpid()));
        config.output_directory = root;
        asr::LoadSpec spec;
        spec.calls = 2;
        spec.concurrency = 2;
        spec.warmups = 1;
        spec.repetitions = 1;
        spec.languages = {"en", "id", "zh"};
        const auto first = asr::plan_load(config, spec, 3200, 4ULL * 1024 * 1024 * 1024);
        const auto second = asr::plan_load(config, spec, 3200, 4ULL * 1024 * 1024 * 1024);
        check(first.allowed && first.json()["calls"] == second.json()["calls"] && first.calls.size() == 4,
              "seeded load plan not reproducible");
        auto too_many = spec;
        too_many.concurrency = 3;
        const auto rejected = asr::plan_load(config, too_many, 3200, 4ULL * 1024 * 1024 * 1024);
        check(!rejected.allowed && !rejected.preflight["skip_reasons"].empty(),
              "worker capacity preflight failed");
        auto native_config = config;
        native_config.runtime = "qwen_native";
        native_config.audio_source = "wav";
        const auto memory_rejected = asr::plan_load(native_config, spec, 3200, 6ULL * 1024 * 1024 * 1024);
        check(!memory_rejected.allowed, "native memory preflight failed");
        auto network = spec;
        network.mode = "network";
        const auto network_plan = asr::plan_load(config, network, 3200, 4ULL * 1024 * 1024 * 1024);
        check(network_plan.allowed && network_plan.preflight["mode"] == "network",
              "network mode not explicitly planned");
        asr::WorkerLayout layout;
        layout.processes = 2;
        asr::SessionManager manager(layout,
                                    std::make_unique<asr::FactoryWorkerExecutor>(
                                        "in_process", [] { return std::make_unique<asr::MockEngine>(); }),
                                    std::make_unique<asr::RoundRobinScheduler>());
        const auto result = asr::run_load(config, manager, first, 3200, nullptr,
                                          {{"source", "synthetic"}, {"seed", config.seed}});
        check(result["status"] == "COMPLETE" && result["completed_calls"] == 4 &&
                  result["failed_calls"] == 0 && result["phases"].size() == 2 &&
                  result["phases"][0]["warmup"] == true && result["phases"][1]["warmup"] == false &&
                  result["metrics"]["offered_calls"] == 2 && result["metrics"]["completed_calls"] == 2 &&
                  result["slo"]["qualified"] == false,
              "mock load execution failed");
        auto strict = spec;
        strict.max_p95_final_ms = 1;
        const auto slo = asr::evaluate_load_slo(strict, result);
        check(!slo["thresholds_met"].get<bool>() && slo["violations"].size() == 1 &&
                  slo["violations"][0] == "p95_final_ms",
              "declared SLO did not detect final latency violation");
        auto zero_failures = spec;
        zero_failures.max_failure_rate = 0.0;
        check(asr::evaluate_load_slo(zero_failures, result)["profile_declared"] == true,
              "explicit zero-failure bound was treated as absent");
        auto large_mock = result;
        large_mock["metrics"]["offered_calls"] = 20;
        check(asr::evaluate_load_slo(zero_failures, large_mock)["qualified"] == false,
              "mock load was incorrectly qualified as capacity evidence");
        const auto a = result["phases"][1]["calls"][0]["summary"]["worker_id"].get<std::string>();
        const auto b = result["phases"][1]["calls"][1]["summary"]["worker_id"].get<std::string>();
        check(a != b, "concurrent calls did not use separate workers");
        check(std::filesystem::exists(std::filesystem::path(result["directory"].get<std::string>()) /
                                      "plan.json"),
              "load plan artifact missing");
        std::filesystem::remove_all(root);
        std::cout << "M6 deterministic plan, preflight and concurrent mock load passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
