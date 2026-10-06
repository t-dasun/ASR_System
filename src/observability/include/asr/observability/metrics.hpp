#pragma once
#include <asr/core/types.hpp>
#include <nlohmann/json.hpp>

namespace asr {
// Linear interpolation at (n-1)*p (Hyndman-Fan type 7), with explicit population.
nlohmann::json distribution(std::vector<double> values, const std::string &population,
                            const std::string &unit);
nlohmann::json observation_json(const RuntimeObservation &value);
struct CallMeasurements {
    std::int64_t session_requested_ns = 0, ready_ns = 0, stream_start_ns = 0;
    std::optional<std::int64_t> eof_requested_ns;
    std::int64_t audio_samples = 0;
    std::vector<RecognitionEvent> events;
    std::vector<nlohmann::json> chunks;
    std::vector<RuntimeObservation> runtime;
    nlohmann::json summary() const;
};
} // namespace asr
