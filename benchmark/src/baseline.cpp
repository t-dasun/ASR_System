#include <asr/audio/delivery.hpp>
#include <asr/benchmark/baseline.hpp>
#include <asr/core/chunk_schedule.hpp>
#include <asr/observability/metrics.hpp>
#include <asr/observability/system_sampler.hpp>
#include <stdexcept>

namespace asr {
namespace {
class RepositorySink final : public IRecognitionSink {
    IResultRepository &repository_;
    IClock &clock_;
    CallMeasurements &measurements_;

  public:
    std::uint64_t count = 0, finals = 0;
    RepositorySink(IResultRepository &repository, IClock &clock, CallMeasurements &measurements)
        : repository_(repository), clock_(clock), measurements_(measurements) {}
    void on_event(const RecognitionEvent &event) override {
        auto published = event;
        published.published_ns = clock_.now_ns();
        if (measurements_.events.size() >= 100000)
            throw std::runtime_error("critical event measurement capacity exceeded");
        measurements_.events.push_back(published);
        repository_.append(published);
        ++count;
        if (event.kind == EventKind::final)
            ++finals;
    }
    void on_observation(const RuntimeObservation &observation) override {
        if (measurements_.runtime.size() >= 100000)
            throw std::runtime_error("runtime measurement capacity exceeded");
        measurements_.runtime.push_back(observation);
        auto record = observation_json(observation);
        record["clock_domain"] = clock_.domain();
        record["controller_received_ns"] = clock_.now_ns();
        repository_.append_record("runtime_timing", record);
    }
};
void require(Status status) {
    if (!status)
        throw std::runtime_error(status.message);
}
} // namespace
nlohmann::json run_mock_baseline(const RunConfig &config, IASREngine &engine, IAudioSource &source,
                                 IClock &clock, IResultRepository &repository, const std::string &run_id,
                                 std::int64_t total_samples, const nlohmann::json &audio_metadata) {
    if (!engine.capabilities().is_mock)
        throw std::invalid_argument("mock runner requires a mock engine");
    return run_baseline(config, engine, source, clock, repository, run_id, total_samples, audio_metadata);
}
nlohmann::json run_baseline(const RunConfig &config, IASREngine &engine, IAudioSource &source, IClock &clock,
                            IResultRepository &repository, const std::string &run_id,
                            std::int64_t total_samples, const nlohmann::json &audio_metadata) {
    if (total_samples < 0 && config.audio_source != "synthetic")
        throw std::invalid_argument("WAV run requires its prepared total sample count");
    repository.begin(run_id, config.resolved, environment_json(engine.capabilities().engine_id));
    CallMeasurements measurements;
    measurements.audio_samples =
        total_samples < 0 ? static_cast<std::int64_t>(config.duration_ms) * 16 : total_samples;
    RepositorySink sink(repository, clock, measurements);
    std::unique_ptr<ResourceMonitor> resources;
    std::unique_ptr<IASRSession> session;
    auto persist_measurements = [&](nlohmann::json &summary) {
        if (resources) {
            resources->stop();
            for (const auto &sample : resources->samples())
                repository.append_record("system_metrics", sample);
            summary["resources"] = resources->summary();
            if (!summary["resources"]["measurement_valid"].get<bool>())
                summary["status"] = "FAILED";
        } else {
            summary["resources"] = {{"enabled", false},
                                    {"reason", config.realtime
                                                   ? "disabled by config"
                                                   : "simulated clock; host samples not meaningful"}};
        }
        summary["measurements"] = measurements.summary();
        repository.append_record("calls", {{"run_id", run_id},
                                           {"call_id", run_id + "_call_0"},
                                           {"language", config.language},
                                           {"status", summary.at("status")},
                                           {"measurements", summary["measurements"]}});
        for (const auto &item : measurements.runtime)
            if (item.stage == "worker_usage" || item.stage == "worker_started")
                repository.append_record("workers", observation_json(item));
    };
    try {
        if (config.resource_sampling && config.realtime)
            resources = std::make_unique<ResourceMonitor>(std::make_unique<LinuxSystemSampler>(),
                                                          config.sample_interval_ms);
        SessionConfig session_config;
        session_config.run_id = run_id;
        session_config.call_id = run_id + "_call_0";
        session_config.language = config.language;
        session_config.seed = config.seed;
        session_config.partial_every_ms = config.partial_every_ms;
        const auto chunk_size = chunk_samples(config.chunk_ms, 16000);
        session_config.max_chunk_samples = chunk_size;
        measurements.session_requested_ns = clock.now_ns();
        auto created = engine.create_session(session_config, sink, clock);
        measurements.ready_ns = clock.now_ns();
        require(created.status);
        if (!created.value)
            throw std::runtime_error("engine returned no session");
        session = std::move(created.value);
        const auto total =
            total_samples < 0 ? static_cast<std::int64_t>(config.duration_ms) * 16 : total_samples;
        PacedAudioStream delivery(source, clock, run_id, session_config.call_id, total, config.chunk_ms,
                                  DeliveryLimits{static_cast<std::size_t>(config.queue_max_chunks),
                                                 config.queue_max_ms, config.max_lag_ms,
                                                 config.late_tolerance_ms,
                                                 static_cast<std::size_t>(config.queue_max_bytes)});
        const auto started = delivery.origin_ns();
        measurements.stream_start_ns = started;
        while (delivery.state() == DeliveryState::running || delivery.state() == DeliveryState::draining) {
            if (delivery.state() == DeliveryState::running && delivery.stats().produced_samples < total)
                clock.sleep_until_ns(delivery.next_deadline_ns());
            require(delivery.produce_ready());
            if (delivery.state() == DeliveryState::running && delivery.stats().produced_samples == total)
                require(delivery.finish_input(delivery.stats().produced_samples == 0
                                                  ? 0
                                                  : (delivery.stats().produced_samples + chunk_size - 1) /
                                                        chunk_size,
                                              total));
            auto available = delivery.pop();
            require(available.status);
            if (!available.value)
                continue;
            auto item = std::move(*available.value);
            if (measurements.chunks.size() >= 100000)
                throw std::runtime_error("chunk measurement capacity exceeded");
            nlohmann::json timing{{"schema_version", 1},
                                  {"run_id", run_id},
                                  {"call_id", session_config.call_id},
                                  {"sequence", item.timing.sequence},
                                  {"first_sample", item.timing.first_sample},
                                  {"sample_count", item.timing.sample_count},
                                  {"scheduled_ready_ns", item.timing.scheduled_ready_ns},
                                  {"source_read_ns", item.timing.source_read_ns},
                                  {"enqueued_ns", item.timing.enqueued_ns},
                                  {"dequeued_ns", item.timing.sent_ns},
                                  {"sent_ns", item.timing.sent_ns},
                                  {"lag_ns", item.timing.lag_ns},
                                  {"queued_samples_after_pop", item.timing.queued_samples_after_pop},
                                  {"clock_domain", clock.domain()}};
            timing["submit_started_ns"] = clock.now_ns();
            auto submitted = session->submit(std::move(item.chunk));
            timing["submit_returned_ns"] = clock.now_ns();
            timing["accepted"] = static_cast<bool>(submitted);
            measurements.chunks.push_back(timing);
            repository.append_audio(timing);
            require(submitted);
        }
        const auto &stats = delivery.stats();
        if (stats.delivered_samples != total || delivery.state() != DeliveryState::completed)
            throw std::runtime_error("audio delivery did not complete all samples");
        measurements.eof_requested_ns = clock.now_ns();
        require(session->finish_input());
        const auto snapshot = session->snapshot();
        if (snapshot.state != SessionState::completed || sink.finals != 1)
            throw std::runtime_error("missing unique successful terminal event");
        nlohmann::json summary{
            {"schema_version", 1},
            {"run_id", run_id},
            {"status", "COMPLETE"},
            {"is_mock", engine.capabilities().is_mock},
            {"engine", engine.capabilities().engine_id},
            {"clock_domain", clock.domain()},
            {"audio_samples", stats.delivered_samples},
            {"audio_seconds", static_cast<double>(stats.delivered_samples) / 16000},
            {"chunks", stats.delivered_chunks},
            {"audio", audio_metadata},
            {"late_chunks", stats.late_chunks},
            {"max_send_lag_ns", stats.max_lag_ns},
            {"peak_queued_chunks", stats.peak_chunks},
            {"peak_queued_samples", stats.peak_samples},
            {"peak_queued_bytes", stats.peak_bytes},
            {"audio_overflows", stats.overflows},
            {"events", sink.count},
            {"transcript", snapshot.text},
            {"elapsed_clock_ns", clock.now_ns() - started},
            {"note", engine.capabilities().is_mock
                         ? "MOCK transcript; excludes ASR accuracy and capacity claims"
                         : "Measured native single call; offline evaluator adds human-reference accuracy"}};
        persist_measurements(summary);
        if (resources && !summary["resources"]["measurement_valid"].get<bool>()) {
            summary["status"] = "FAILED";
            summary["error"] = "critical resource measurement loss";
            repository.append_record("errors", {{"message", "critical resource measurement loss"}});
            repository.finish("FAILED", summary);
            return summary;
        }
        repository.finish("COMPLETE", summary);
        return summary;
    } catch (const std::exception &error) {
        nlohmann::json summary{{"schema_version", 1},
                               {"run_id", run_id},
                               {"is_mock", engine.capabilities().is_mock},
                               {"status", "FAILED"},
                               {"error", error.what()},
                               {"transcript", session ? session->snapshot().text : ""}};
        // Stop the worker before persisting failure artifacts; keep partial text as evidence.
        session.reset();
        repository.append_record("errors", {{"message", error.what()}, {"timestamp_ns", clock.now_ns()}});
        persist_measurements(summary);
        repository.finish("FAILED", summary);
        throw;
    }
}
} // namespace asr
