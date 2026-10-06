#pragma once
#include <asr/benchmark/load.hpp>
#include <asr/config/config.hpp>

namespace asr::app {
int run_service(const RunConfig &config, const std::filesystem::path &config_path,
                const std::vector<std::string> &overrides, const LoadSpec &load_spec, int serve_port);
} // namespace asr::app
