#pragma once
#include <asr/audio/source.hpp>
#include <asr/config/config.hpp>
#include <asr/engines/engine.hpp>
#include <asr/storage/repository.hpp>

namespace asr {
// Single-call mock runner; injected collaborators allow independent tests.
nlohmann::json run_mock_baseline(const RunConfig &config, IASREngine &engine, IAudioSource &source,
                                 IClock &clock, IResultRepository &repository, const std::string &run_id,
                                 std::int64_t total_samples = -1,
                                 const nlohmann::json &audio_metadata = nullptr);
nlohmann::json run_baseline(const RunConfig &config, IASREngine &engine, IAudioSource &source, IClock &clock,
                            IResultRepository &repository, const std::string &run_id,
                            std::int64_t total_samples, const nlohmann::json &audio_metadata);
} // namespace asr
