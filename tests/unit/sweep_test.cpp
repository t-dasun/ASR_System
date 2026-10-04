#include <asr/benchmark/sweep.hpp>
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
int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::invalid_argument("expected mock config path");
        const auto root =
            std::filesystem::temp_directory_path() / ("asr_m6_sweep_" + std::to_string(getpid()));
        const std::vector<std::string> base{"audio.duration_ms=200", "audio.realtime_pacing=true",
                                            "metrics.resource_sampling=false",
                                            "output.directory=" + root.string()};
        asr::SweepSpec spec;
        spec.strategy = "oat";
        spec.load.calls = 2;
        spec.load.concurrency = 1;
        spec.load.languages = {"en", "id"};
        spec.axes = {{"model.threads", {"2", "4"}}, {"audio.chunk_ms", {"100", "200"}}};
        const auto oat = asr::plan_sweep(argv[1], base, spec, 3200, 4ULL * 1024 * 1024 * 1024);
        check(oat.cases.size() == 5 && oat.cases[0].id == "baseline", "OAT plan not complete");
        const auto oat_again = asr::plan_sweep(argv[1], base, spec, 3200, 4ULL * 1024 * 1024 * 1024);
        for (std::size_t i = 0; i < oat.cases.size(); ++i)
            check(oat.cases[i].id == oat_again.cases[i].id &&
                      oat.cases[i].overrides == oat_again.cases[i].overrides,
                  "seeded OAT order changed");
        spec.strategy = "matrix";
        const auto matrix = asr::plan_sweep(argv[1], base, spec, 3200, 4ULL * 1024 * 1024 * 1024);
        check(matrix.cases.size() == 4, "matrix expansion incorrect");
        spec.strategy = "selected";
        spec.axes.clear();
        spec.selected = {{"audio.chunk_ms=100"}, {"unknown.option=1"}};
        const auto selected = asr::plan_sweep(argv[1], base, spec, 3200, 4ULL * 1024 * 1024 * 1024);
        check(selected.cases.size() == 2 && selected.cases[0].skip_reasons.empty() &&
                  !selected.cases[1].skip_reasons.empty(),
              "selected unsupported case did not become SKIPPED");
        spec.strategy = "scale";
        spec.axes = {{"concurrency", {"1", "2", "4"}}};
        spec.selected.clear();
        const auto scale = asr::plan_sweep(argv[1], base, spec, 3200, 4ULL * 1024 * 1024 * 1024);
        check(scale.cases.size() == 3, "scale levels missing");
        const auto result =
            asr::run_sweep(scale, root, [](const asr::RunConfig &config, const asr::LoadPlan &load) {
                const int level = load.spec.concurrency;
                check(config.worker_processes == level, "worker layout not matched to scale level");
                return nlohmann::json{{"status", "COMPLETE"},
                                      {"directory", "fixture"},
                                      {"slo", {{"profile_declared", true}, {"thresholds_met", level <= 2}}}};
            });
        check(result["cases"].size() == 4 && result["saturation"]["last_passing_concurrency"] == 2 &&
                  result["saturation"]["first_failing_concurrency"] == 3 &&
                  result["cases"].back()["id"] == "refine_3",
              "scale did not refine first failing region");
        std::filesystem::remove_all(root);
        std::cout << "M6 OAT, matrix, selected and saturation-refinement plans passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
