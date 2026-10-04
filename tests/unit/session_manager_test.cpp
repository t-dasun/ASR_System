#include <asr/audio/delivery.hpp>
#include <asr/backend/session_manager.hpp>
#include <asr/engines/mock_engine.hpp>
#include <barrier>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Sink : asr::IRecognitionSink {
    std::vector<asr::RecognitionEvent> events;
    void on_event(const asr::RecognitionEvent &event) override { events.push_back(event); }
};
std::unique_ptr<asr::IWorkerExecutor> executor() {
    return std::make_unique<asr::FactoryWorkerExecutor>("in_process",
                                                        [] { return std::make_unique<asr::MockEngine>(); });
}
asr::SessionConfig config(const char *id, const char *language) {
    asr::SessionConfig result;
    result.run_id = "m5_contract";
    result.call_id = id;
    result.language = language;
    result.max_chunk_samples = 3200;
    result.partial_every_ms = 200;
    return result;
}
asr::AudioChunk chunk(const char *id) {
    return {
        "m5_contract", id, 0, 0, 16000, 1, 0, 0, std::make_shared<const std::vector<std::int16_t>>(3200, 42)};
}
} // namespace
int main() {
    try {
        asr::WorkerLayout layout;
        layout.processes = 2;
        layout.idle_timeout_ms = 1000;
        layout.total_timeout_ms = 10000;
        asr::SessionManager manager(layout, executor(), std::make_unique<asr::RoundRobinScheduler>());
        check(manager.capabilities().concurrent_sessions && manager.executor_kind() == "in_process",
              "managed capability/executor mismatch");
        asr::SteadyClock clock;
        Sink english, indonesian, third;
        auto en = manager.create_session(config("en_0", "en"), english, clock);
        auto id = manager.create_session(config("id_0", "id"), indonesian, clock);
        check(en && id, "independent language sessions failed admission");
        auto occupied = manager.workers();
        check(occupied[0].occupied && occupied[1].occupied && occupied[0].call_id != occupied[1].call_id &&
                  occupied[0].active_sessions == 1 && occupied[1].active_sessions == 1,
              "sticky assignments or worker limits incorrect");
        check(manager.create_session(config("third", "zh"), third, clock).status.code ==
                  asr::ErrorCode::resource_exhausted,
              "admission limit ignored");
        std::barrier rendezvous(2);
        auto operate = [&](std::unique_ptr<asr::IASRSession> &session, const char *call) {
            rendezvous.arrive_and_wait();
            check(static_cast<bool>(session->submit(chunk(call))), "concurrent submit rejected");
            check(static_cast<bool>(session->finish_input()), "concurrent final rejected");
        };
        std::thread a([&] { operate(en.value, "en_0"); });
        std::thread b([&] { operate(id.value, "id_0"); });
        a.join();
        b.join();
        check(en.value->snapshot().state == asr::SessionState::completed &&
                  id.value->snapshot().state == asr::SessionState::completed &&
                  en.value->snapshot().text.find("MOCK en") != std::string::npos &&
                  id.value->snapshot().text.find("MOCK id") != std::string::npos &&
                  english.events.size() == 2 && indonesian.events.size() == 2 &&
                  english.events.back().worker_id != indonesian.events.back().worker_id,
              "language state or worker identities crossed");
        check(manager.reset(en.value, config("en_0", "en"), third, clock).status.code ==
                      asr::ErrorCode::invalid_input &&
                  en.value,
              "reset with reused call ID cancelled old session");
        auto new_generation = manager.reset(en.value, config("zh_1", "zh"), third, clock);
        check(new_generation && !en.value &&
                  new_generation.value->snapshot().worker_id != english.events.back().worker_id,
              "reset did not release/reassign a distinct generation");
        check(manager.create_session(config("en_0", "en"), english, clock).status.code ==
                  asr::ErrorCode::invalid_input,
              "reused call ID accepted");
        check(static_cast<bool>(new_generation.value->cancel(asr::CancelReason::user_request)) &&
                  static_cast<bool>(new_generation.value->cancel(asr::CancelReason::user_request)) &&
                  new_generation.value->snapshot().state == asr::SessionState::stopped &&
                  third.events.size() == 1 && third.events.front().kind == asr::EventKind::stopped,
              "stop idempotence/terminal state failed");
        new_generation.value.reset();
        auto recovered = manager.create_session(config("recovered_after_overload", "en"), english, clock);
        check(recovered && bool(recovered.value->submit(chunk("recovered_after_overload"))) &&
                  bool(recovered.value->finish_input()) &&
                  recovered.value->snapshot().state == asr::SessionState::completed,
              "manager did not recover admission after overload and cancellation");
        recovered.value.reset();
        manager.begin_draining();
        check(manager.draining() && manager.create_session(config("new", "en"), english, clock).status.code ==
                                        asr::ErrorCode::invalid_state,
              "draining admitted new work");
        check(id.value->snapshot().state == asr::SessionState::completed,
              "draining damaged an existing call");

        asr::WorkerLayout timed_layout;
        timed_layout.idle_timeout_ms = 100;
        timed_layout.total_timeout_ms = 1000;
        asr::SessionManager timed(timed_layout, executor(), std::make_unique<asr::LeastActiveScheduler>());
        asr::FakeClock fake;
        Sink expired;
        auto deadline = timed.create_session(config("idle", "en"), expired, fake);
        fake.sleep_until_ns(200000000);
        check(deadline && deadline.value->submit(chunk("idle")).code == asr::ErrorCode::deadline_expired &&
                  deadline.value->snapshot().state == asr::SessionState::stopped &&
                  expired.events.size() == 1 && expired.events[0].kind == asr::EventKind::stopped,
              "idle deadline did not cancel call");
        deadline.value.reset();
        check(!timed.workers().front().occupied, "destroyed call did not release worker");

        asr::WorkerLayout autonomous_layout;
        autonomous_layout.idle_timeout_ms = 50;
        autonomous_layout.total_timeout_ms = 1000;
        asr::SessionManager autonomous(autonomous_layout, executor(),
                                       std::make_unique<asr::LeastActiveScheduler>());
        Sink unattended;
        auto quiet = autonomous.create_session(config("quiet", "en"), unattended, clock);
        check(static_cast<bool>(quiet), "autonomous deadline session did not start");
        std::this_thread::sleep_for(std::chrono::milliseconds(90));
        check(quiet.value->snapshot().state == asr::SessionState::stopped && unattended.events.size() == 1 &&
                  unattended.events.front().kind == asr::EventKind::stopped &&
                  unattended.events.front().status.code == asr::ErrorCode::deadline_expired,
              "idle watchdog did not stop inactive call");

        asr::FakeClock queue_clock;
        asr::SyntheticPcmSource audio(6400, 42);
        asr::PacedAudioStream delivery(audio, queue_clock, "m5_contract", "queue", 6400, 200,
                                       {1, 200, 1000, 5, 6400});
        queue_clock.sleep_until_ns(400000000);
        check(delivery.produce_ready().code == asr::ErrorCode::resource_exhausted &&
                  delivery.stats().overflows == 1 && delivery.state() == asr::DeliveryState::failed,
              "bounded ingress queue overflow was not deterministic");
        std::cout << "M5 admission, isolation, reset, drain, deadlines, and queue overflow passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
