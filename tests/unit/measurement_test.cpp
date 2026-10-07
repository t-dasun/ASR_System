#include <asr/observability/metrics.hpp>
#include <asr/observability/system_sampler.hpp>
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
struct FixtureSampler : asr::ISystemSampler {
    bool fail;
    explicit FixtureSampler(bool failure) : fail(failure) {}
    nlohmann::json sample() override {
        if (fail)
            throw std::runtime_error("fixture telemetry loss");
        return {{"processes",
                 {{{"pid", -1},
                   {"threads", 2},
                   {"rss_bytes", 100},
                   {"pss_bytes", 80},
                   {"cpu_core_equivalents", 1.5}}}}};
    }
};
int main() {
    try {
        asr::CallMeasurements m;
        m.session_requested_ns = 100000000;
        m.ready_ns = 200000000;
        m.stream_start_ns = 200000000;
        m.eof_requested_ns = 1210000000;
        m.audio_samples = 16000;
        asr::RecognitionEvent first;
        first.text = " \n";
        first.produced_ns = 300000000;
        first.published_ns = 315000000;
        m.events.push_back(first);
        first.text = "hello";
        first.produced_ns = 500000000;
        first.published_ns = 520000000;
        m.events.push_back(first);
        first.kind = asr::EventKind::final;
        first.produced_ns = 1490000000;
        first.published_ns = 1500000000;
        m.events.push_back(first);
        m.chunks.push_back({{"lag_ns", 10000000},
                            {"enqueued_ns", 401000000},
                            {"sent_ns", 410000000},
                            {"submit_started_ns", 411000000},
                            {"submit_returned_ns", 415000000}});
        auto value = m.summary();
        check(value["startup_ns"] == 100000000, "startup definition");
        check(value["first_usable_transcript_ns"] == 320000000, "first text definition");
        check(value["final_result_ns"] == 1300000000, "final latency definition");
        check(value["finalization_ns"] == 290000000, "EOF delay definition");
        check(value["scheduled_final_lag_ns"] == 300000000, "scheduled final lag definition");
        check(std::abs(value["effective_rtf"].get<double>() - 1.3) < 1e-12, "effective RTF definition");
        check(value["inference_compute_rtf"].is_null(), "fabricated compute time");
        check(value["controller_queue_wait"]["p50"] == 9000000, "queue wait definition");
        check(value["publication_delay"]["p50"] == 15000000, "publication distribution");
        const auto dist = asr::distribution({1, 2, 3, 4}, "fixture", "ns");
        check(dist["p50"] == 2.5 && std::abs(dist["p95"].get<double>() - 3.85) < 1e-12, "quantile estimator");
        check(asr::distribution({}, "empty", "ns")["p99"].is_null(), "empty percentile");
        m.audio_samples = 0;
        check(m.summary()["effective_rtf"].is_null(), "zero audio RTF");
        asr::RuntimeObservation eof;
        eof.stage = "worker_eof_received";
        eof.timestamp_ns = 1220000000;
        m.runtime.push_back(eof);
        check(m.summary()["finalization_ns"] == 280000000, "worker EOF boundary");
        auto shared = m;
        shared.audio_samples = 16000;
        const auto observe = [&](const char *stage, std::int64_t duration) {
            asr::RuntimeObservation value;
            value.stage = stage;
            value.duration_ns = duration;
            shared.runtime.push_back(value);
        };
        observe("shared_model_load", 750000000);
        observe("prefix_decode", 400000000);
        observe("eof_refinement", 800000000);
        observe("prefix_decode_queue_wait", 2000000);
        observe("eof_decode_queue_wait", 3000000);
        const auto shared_metrics = shared.summary();
        check(shared_metrics["model_load_ns"].is_null() &&
                  shared_metrics["shared_model_load_ns"] == 750000000 &&
                  shared_metrics["startup_ns"] == 100000000,
              "reused context load was counted as per-call startup");
        check(shared_metrics["runtime_queue_wait_ns"] == 5000000 &&
                  shared_metrics["offline_decode_wall_ns"] == 1200000000 &&
                  std::abs(shared_metrics["offline_decode_wall_rtf"].get<double>() - 1.2) < 1e-12 &&
                  shared_metrics["inference_compute_rtf"].is_null(),
              "shared decode wall, scheduling wait, and active-compute definitions conflated");
        asr::LinuxSystemSampler sampler;
        const auto sample = sampler.sample();
        check(!sample["processes"].empty() && sample["cpu"].contains("cpu"), "Linux sampling");
        check(sample["processes"][0]["cpu_core_equivalents"].is_null(), "first CPU delta must be null");
        int control[2];
        check(pipe(control) == 0, "fixture pipe failed");
        std::atomic<pid_t> child_pid{-2};
        std::atomic<bool> release{false};
        std::thread owner([&] {
            const auto pid = fork();
            if (pid == 0) {
                close(control[1]);
                char marker;
                (void)read(control[0], &marker, 1);
                _exit(0);
            }
            child_pid.store(pid);
            while (!release.load())
                std::this_thread::yield();
            const char marker = 'x';
            (void)write(control[1], &marker, 1);
            int status = 0;
            (void)waitpid(pid, &status, 0);
        });
        while (child_pid.load() == -2)
            std::this_thread::yield();
        const auto threaded = child_pid.load() < 0 ? nlohmann::json() : sampler.sample();
        bool found = false;
        if (!threaded.is_null())
            for (const auto &item : threaded["processes"])
                found |= item["pid"] == child_pid.load();
        release.store(true);
        owner.join();
        close(control[0]);
        close(control[1]);
        check(found, "sampler missed a child forked from a runner thread");
        auto streamed = m;
        streamed.audio_samples = 16000;
        for (const auto duration : {100000000LL, 200000000LL}) {
            asr::RuntimeObservation step;
            step.stage = "stream_step";
            step.duration_ns = duration;
            streamed.runtime.push_back(step);
        }
        asr::RuntimeObservation count;
        count.stage = "stream_steps";
        count.counter_value = 2;
        streamed.runtime.push_back(count);
        const auto stream_metrics = streamed.summary();
        check(stream_metrics["stream_decode_wall_ns"] == 300000000 &&
                  stream_metrics["stream_decode_steps"] == 2 &&
                  stream_metrics["offline_decode_wall_rtf"].is_null(),
              "stream step totals or offline/stream metric separation incorrect");
        asr::ResourceMonitor fixture(std::make_unique<FixtureSampler>(false), 200);
        fixture.stop();
        check(fixture.summary()["sampled_peak_tree_rss_bytes"] == 100 &&
                  fixture.summary()["max_tree_cpu_core_equivalents"] == 1.5,
              "replaceable sampler summary");
        asr::ResourceMonitor failed(std::make_unique<FixtureSampler>(true), 200);
        failed.stop();
        check(!failed.summary()["measurement_valid"].get<bool>(), "silent critical telemetry loss");
        std::cout << "measurement timeline, quantiles, nulls, and Linux sampler passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
