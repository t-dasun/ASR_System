#include <algorithm>
#include <asr/observability/metrics.hpp>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace asr {
using Json = nlohmann::json;
Json distribution(std::vector<double> values, const std::string &population, const std::string &unit) {
    Json result{{"count", values.size()},
                {"population", population},
                {"unit", unit},
                {"estimator", "linear_(n-1)*p_type7"}};
    for (const auto value : values)
        if (!std::isfinite(value))
            throw std::invalid_argument("nonfinite metric observation");
    std::sort(values.begin(), values.end());
    for (const auto &[name, p] :
         std::vector<std::pair<std::string, double>>{{"p50", .5}, {"p90", .9}, {"p95", .95}, {"p99", .99}}) {
        if (values.empty()) {
            result[name] = nullptr;
            continue;
        }
        const auto index = (values.size() - 1) * p;
        const auto low = static_cast<std::size_t>(index);
        const auto high = std::min(low + 1, values.size() - 1);
        result[name] = values[low] + (values[high] - values[low]) * (index - low);
    }
    result["max"] = values.empty() ? Json(nullptr) : Json(values.back());
    result["mean"] = values.empty()
                         ? Json(nullptr)
                         : Json(std::accumulate(values.begin(), values.end(), 0.0) / values.size());
    return result;
}
Json observation_json(const RuntimeObservation &value) {
    Json result{{"stage", value.stage},
                {"worker_id", value.worker_id},
                {"timestamp_ns", value.timestamp_ns},
                {"process_id", value.process_id}};
    auto optional = [&](const char *key, auto item) { result[key] = item ? Json(*item) : Json(nullptr); };
    optional("duration_ns", value.duration_ns);
    optional("counter_value", value.counter_value);
    optional("sequence", value.sequence);
    optional("buffered_samples", value.buffered_samples);
    optional("cpu_ns", value.cpu_ns);
    optional("peak_rss_bytes", value.peak_rss_bytes);
    return result;
}
Json CallMeasurements::summary() const {
    Json result{{"schema_version", 1},
                {"session_requested_ns", session_requested_ns},
                {"ready_ns", ready_ns},
                {"stream_start_ns", stream_start_ns},
                {"startup_ns", ready_ns - session_requested_ns},
                {"audio_duration_ns", audio_samples * 62500},
                {"first_usable_transcript_ns", nullptr},
                {"first_partial_ns", nullptr},
                {"final_result_ns", nullptr},
                {"finalization_ns", nullptr},
                {"scheduled_final_lag_ns", nullptr},
                {"effective_rtf", nullptr},
                {"model_load_ns", nullptr},
                {"shared_model_load_ns", nullptr},
                {"prefix_decode_wall_ns", nullptr},
                {"prefix_decode_queue_wait_ns", nullptr},
                {"eof_decode_queue_wait_ns", nullptr},
                {"offline_decode_wall_ns", nullptr},
                {"offline_decode_wall_rtf", nullptr},
                {"live_invocation_wall_ns", nullptr},
                {"eof_refinement_wall_ns", nullptr},
                {"worker_cpu_ns", nullptr},
                {"worker_peak_rss_bytes", nullptr},
                {"inference_compute_rtf", nullptr},
                {"first_inference_compute_ns", nullptr},
                {"first_stable_transcript_ns", nullptr},
                {"runtime_queue_wait_ns", nullptr},
                {"partial_service_lag_ns", nullptr},
                {"unavailable_reason",
                 "Native API has no active-decode, dequeue, stable-prefix, or consumed-audio "
                 "boundary; delivered samples are not acoustic alignment."},
                {"effective_rtf_definition", "final publication minus stream start, divided by unique input "
                                             "duration; includes paced waiting and EOF refinement"},
                {"publication_boundary",
                 "controller sink receipt before synchronous artifact append; no network/UI"}};
    std::optional<std::int64_t> worker_eof;
    for (const auto &item : runtime)
        if (item.stage == "worker_eof_received")
            worker_eof = item.timestamp_ns;
    result["worker_eof_received_ns"] = worker_eof ? Json(*worker_eof) : Json(nullptr);
    result["finalization_reference"] =
        worker_eof ? "worker EOF receipt" : "controller EOF request (worker receipt unavailable)";
    result["eof_requested_ns"] = eof_requested_ns ? Json(*eof_requested_ns) : Json(nullptr);
    std::vector<double> lag, queue, submit, publication;
    for (const auto &chunk : chunks) {
        lag.push_back(chunk.at("lag_ns").get<double>());
        queue.push_back(chunk.at("sent_ns").get<std::int64_t>() -
                        chunk.at("enqueued_ns").get<std::int64_t>());
        submit.push_back(chunk.at("submit_returned_ns").get<std::int64_t>() -
                         chunk.at("submit_started_ns").get<std::int64_t>());
    }
    for (const auto &event : events) {
        if (!event.published_ns)
            continue;
        const auto published = *event.published_ns;
        publication.push_back(static_cast<double>(published - event.produced_ns));
        if (event.text.find_first_not_of(" \t\r\n\v\f") != std::string::npos &&
            (event.kind == EventKind::partial || event.kind == EventKind::final)) {
            if (result["first_usable_transcript_ns"].is_null())
                result["first_usable_transcript_ns"] = published - stream_start_ns;
            if (event.kind == EventKind::partial && result["first_partial_ns"].is_null())
                result["first_partial_ns"] = published - stream_start_ns;
        }
        if (event.kind == EventKind::final) {
            result["final_result_ns"] = published - stream_start_ns;
            result["scheduled_final_lag_ns"] = published - stream_start_ns - audio_samples * 62500;
            if (eof_requested_ns)
                result["finalization_ns"] = published - worker_eof.value_or(*eof_requested_ns);
            if (audio_samples)
                result["effective_rtf"] = double(published - stream_start_ns) / double(audio_samples * 62500);
        }
    }
    std::int64_t stream_wall = 0, stream_wait = 0;
    bool has_stream = false;
    for (const auto &item : runtime) {
        if (item.stage == "stream_step" && item.duration_ns) {
            has_stream = true;
            stream_wall += *item.duration_ns;
        }
        if (item.stage == "stream_step_queue_wait" && item.duration_ns)
            stream_wait += *item.duration_ns;
        if (item.stage == "stream_steps" && item.counter_value)
            result["stream_decode_steps"] = *item.counter_value;
        if (item.stage == "stream_reused_prefill_tokens" && item.counter_value)
            result["stream_reused_prefill_tokens"] = *item.counter_value;
        if (item.stage == "shared_model_load" && item.duration_ns) {
            result["shared_model_load_ns"] = *item.duration_ns;
            result["shared_model_load_definition"] =
                "one context load before call admission; reused metadata, not per-call startup";
        }
        if (item.stage == "prefix_decode" && item.duration_ns)
            result["prefix_decode_wall_ns"] = *item.duration_ns;
        if (item.stage == "prefix_decode_queue_wait" && item.duration_ns)
            result["prefix_decode_queue_wait_ns"] = *item.duration_ns;
        if (item.stage == "eof_decode_queue_wait" && item.duration_ns)
            result["eof_decode_queue_wait_ns"] = *item.duration_ns;
        if (item.stage == "model_load" && item.duration_ns)
            result["model_load_ns"] = *item.duration_ns;
        if (item.stage == "live_invocation" && item.duration_ns)
            result["live_invocation_wall_ns"] = *item.duration_ns;
        if (item.stage == "eof_refinement" && item.duration_ns)
            result["eof_refinement_wall_ns"] = *item.duration_ns;
        if (item.cpu_ns)
            result["worker_cpu_ns"] = *item.cpu_ns;
        if (item.peak_rss_bytes)
            result["worker_peak_rss_bytes"] = *item.peak_rss_bytes;
    }
    if (!result["shared_model_load_ns"].is_null()) {
        const auto amount = [&](const char *key) -> std::int64_t {
            return result[key].is_null() ? 0 : result[key].get<std::int64_t>();
        };
        result["runtime_queue_wait_ns"] =
            amount("prefix_decode_queue_wait_ns") + amount("eof_decode_queue_wait_ns");
        result["offline_decode_wall_ns"] = amount("prefix_decode_wall_ns") + amount("eof_refinement_wall_ns");
        if (audio_samples &&
            (!result["prefix_decode_wall_ns"].is_null() || !result["eof_refinement_wall_ns"].is_null()))
            result["offline_decode_wall_rtf"] =
                double(amount("offline_decode_wall_ns")) / double(audio_samples * 62500);
        result["offline_decode_wall_definition"] =
            "sum of prefix and EOF offline invocation wall time, divided by unique source duration for RTF; "
            "excludes call pacing and ready-job wait";
        result["unavailable_reason"] =
            "Prefix wrapper measures ready-to-decode wait and offline invocation wall time; vendor internal "
            "decode/word alignment and stable text remain unavailable.";
    }
    if (has_stream) {
        const auto refinement = result["eof_refinement_wall_ns"].is_null()
                                    ? 0
                                    : result["eof_refinement_wall_ns"].get<std::int64_t>();
        result["stream_decode_wall_ns"] = stream_wall;
        result["stream_decode_queue_wait_ns"] = stream_wait;
        result["runtime_queue_wait_ns"] = stream_wait;
        result["stream_invocation_wall_ns"] = stream_wall + refinement;
        result["stream_invocation_wall_rtf"] =
            audio_samples ? Json(double(stream_wall + refinement) / double(audio_samples * 62500))
                          : Json(nullptr);
        result["offline_decode_wall_ns"] = nullptr;
        result["offline_decode_wall_rtf"] = nullptr;
        result["offline_decode_wall_definition"] =
            "Not a prefix/offline metric; use stream_invocation_wall_rtf (all serialized streaming steps "
            "plus optional EOF refinement).";
        result["unavailable_reason"] =
            "Resumable steps retain per-call encoder/token/decoder caches. Invocation wall includes kernels; "
            "internal active-compute and word alignment remain unavailable.";
    }

    result["send_lag"] = distribution(lag, "delivered chunks", "ns");
    result["controller_queue_wait"] = distribution(queue, "delivered chunks", "ns");
    result["submit_wall"] = distribution(submit, "submitted chunks", "ns");
    result["publication_delay"] = distribution(publication, "recognition events", "ns");
    result["tail_warning"] =
        "Small populations do not establish tail SLOs; never pool by averaging percentiles.";
    return result;
}
} // namespace asr
