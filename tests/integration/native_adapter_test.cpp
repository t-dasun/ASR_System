#include <asr/engines/native_engine.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Sink final : asr::IRecognitionSink {
    std::vector<asr::RecognitionEvent> events;
    void on_event(const asr::RecognitionEvent &event) override { events.push_back(event); }
};
asr::AudioChunk chunk(const std::string &call, std::uint64_t sequence) {
    return {"stub_run", call,  sequence,
            0,          16000, 1,
            0,          0,     std::make_shared<const std::vector<std::int16_t>>(3200, 42)};
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("expected stub worker path");
        const auto fixture =
            std::filesystem::temp_directory_path() / ("asr_native_stub_" + std::to_string(::getpid()));
        std::filesystem::create_directories(fixture);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } cleanup{fixture};
        std::ofstream(fixture / "model.safetensors").put('x');
        std::ofstream(fixture / "acquisition.json")
            << "{\"status\":\"verified\",\"revision\":\"5eb144179a02acc5e5ba31e748d22b0cf3e303b0\"}";
        asr::NativeQwenOptions options;
        options.worker_executable = argv[1];
        options.model_directory = fixture;
        options.timeout_ms = 1000;
        setenv("ASR_NATIVE_STUB_MODE", "expect_step", 1);
        setenv("OMP_NUM_THREADS", "31", 1);
        asr::NativeQwenEngine engine(options);
        check(!engine.capabilities().is_mock && !engine.capabilities().cooperative_cancellation,
              "native capabilities incorrect");
        asr::SteadyClock clock;
        Sink normal;
        asr::SessionConfig cfg;
        cfg.run_id = "stub_run";
        cfg.call_id = "normal";
        cfg.decode_step_ms = 4000;
        auto result = engine.create_session(cfg, normal, clock);
        check(static_cast<bool>(result), "stub session creation failed");
        check(std::string(std::getenv("OMP_NUM_THREADS")) == "31", "worker changed parent thread budget");
        check(result.value->submit(chunk("normal", 1)).code == asr::ErrorCode::invalid_input,
              "out-of-order audio accepted");
        check(static_cast<bool>(result.value->submit(chunk("normal", 0))), "valid stub audio rejected");
        check(static_cast<bool>(result.value->finish_input()), "stub EOF failed");
        check(static_cast<bool>(result.value->finish_input()), "duplicate native EOF failed");
        check(result.value->snapshot().state == asr::SessionState::completed && normal.events.size() == 2 &&
                  normal.events[0].kind == asr::EventKind::partial && normal.events[0].before_eof &&
                  normal.events[1].kind == asr::EventKind::final && !normal.events[1].before_eof &&
                  normal.events[0].sequence == 0 && normal.events[1].revision > normal.events[0].revision,
              "native partial/final order or revision incorrect");
        check(result.value->submit(chunk("normal", 1)).code == asr::ErrorCode::invalid_state,
              "post-EOF audio accepted");
        asr::FakeClock simulated;
        check(engine.create_session(cfg, normal, simulated).status.code == asr::ErrorCode::unsupported,
              "native simulated clock accepted");
        auto invalid_language = cfg;
        invalid_language.language = "xx";
        check(engine.create_session(invalid_language, normal, clock).status.code ==
                  asr::ErrorCode::unsupported,
              "unsupported native language accepted");
        Sink stopped;
        cfg.call_id = "stopped";
        auto cancellable = engine.create_session(cfg, stopped, clock);
        check(cancellable && static_cast<bool>(cancellable.value->cancel(asr::CancelReason::user_request)) &&
                  stopped.events.size() == 1 && stopped.events[0].kind == asr::EventKind::stopped &&
                  !cancellable.value->finish_input(),
              "native cancellation contract failed");
        setenv("ASR_NATIVE_STUB_MODE", "hang", 1);
        Sink timed;
        cfg.call_id = "timed";
        auto hanging = engine.create_session(cfg, timed, clock);
        check(hanging && static_cast<bool>(hanging.value->submit(chunk("timed", 0))) &&
                  hanging.value->finish_input().code == asr::ErrorCode::deadline_expired &&
                  hanging.value->snapshot().state == asr::SessionState::failed &&
                  timed.events.back().kind == asr::EventKind::failed,
              "native watchdog did not kill a hung worker");
        setenv("ASR_NATIVE_STUB_MODE", "crash", 1);
        Sink crashed;
        cfg.call_id = "crashed";
        auto dying = engine.create_session(cfg, crashed, clock);
        check(dying && static_cast<bool>(dying.value->submit(chunk("crashed", 0))) &&
                  dying.value->finish_input().code == asr::ErrorCode::runtime_failure &&
                  crashed.events.back().kind == asr::EventKind::failed,
              "worker crash not reported as terminal failure");
        unsetenv("ASR_NATIVE_STUB_MODE");
        std::cout << "native worker contract, cancellation, timeout, crash passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
