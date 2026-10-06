#pragma once
#include <asr/config/config.hpp>
#include <asr/engines/engine.hpp>
#include <memory>
#include <nlohmann/json.hpp>

namespace asr::app {
std::unique_ptr<IASREngine> make_engine(const RunConfig &config);
void drain_engine(IASREngine &engine);
nlohmann::json workers_json(IASREngine &engine);
void validate_shared_suite(const RunConfig &startup, const RunConfig &selected);
} // namespace asr::app
