#include <asr/benchmark/baseline.hpp>
#include <asr/engines/mock_engine.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace asr;
namespace {
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class Function> void rejects(Function operation, const std::string &message) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, message);
}
struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / new_run_id();
    TempDirectory() { check(std::filesystem::create_directory(path), "temporary directory collision"); }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
struct Sink : IRecognitionSink {
    std::vector<RecognitionEvent> events;
    void on_event(const RecognitionEvent &event) override { events.push_back(event); }
};
AudioChunk chunk(const std::string &call, std::uint64_t seq, std::int64_t first, std::size_t count) {
    return {"test", call,  seq,
            first,  16000, 1,
            0,      0,     std::make_shared<const std::vector<std::int16_t>>(count, 10)};
}
void clock_and_source() {
    FakeClock clock;
    clock.sleep_until_ns(200000000);
    clock.sleep_until_ns(100000000);
    check(clock.now_ns() == 200000000 && !clock.utc_now(),
          "fake clock must remain monotonic and labelled simulated");
    rejects([&] { clock.sleep_until_ns(-1); }, "negative clock accepted");
    SyntheticPcmSource a(17, 42), b(17, 42), different(17, 43);
    auto whole = a.read(17);
    auto left = b.read(7), right = b.read(16);
    std::vector<std::int16_t> combined(left->begin(), left->end());
    combined.insert(combined.end(), right->begin(), right->end());
    check(combined == *whole && right->size() == 10, "source must be deterministic across chunk boundaries");
    check(*whole != *different.read(17) && a.read(1)->empty(), "source seed or EOF incorrect");
}
void session_contract() {
    MockEngine engine;
    FakeClock clock;
    Sink english, chinese;
    SessionConfig cfg;
    cfg.run_id = "test";
    cfg.call_id = "en";
    cfg.partial_every_ms = 200;
    auto one = engine.create_session(cfg, english, clock);
    cfg.call_id = "zh";
    cfg.language = "zh";
    auto two = engine.create_session(cfg, chinese, clock);
    check(static_cast<bool>(one) && static_cast<bool>(two), "session creation failed");
    check(one.value->submit(chunk("en", 1, 0, 3200)).code == ErrorCode::invalid_input,
          "sequence gap accepted");
    check(one.value->snapshot().consumed_samples == 0, "rejected input changed state");
    check(static_cast<bool>(one.value->submit(chunk("en", 0, 0, 3200))), "valid audio rejected");
    check(static_cast<bool>(two.value->submit(chunk("zh", 0, 0, 3200))), "second session rejected");
    check(english.events.back().text.find("MOCK en") == 0 && chinese.events.back().text.find("MOCK zh") == 0,
          "cross-session language contamination");
    check(one.value->submit(chunk("en", 1, 3200, 16001)).code == ErrorCode::resource_exhausted,
          "oversize chunk accepted");
    check(static_cast<bool>(one.value->submit(chunk("en", 1, 3200, 123))), "tail rejected");
    check(static_cast<bool>(one.value->finish_input()), "EOF failed");
    check(static_cast<bool>(one.value->finish_input()), "duplicate EOF not idempotent");
    check(static_cast<bool>(one.value->cancel(CancelReason::user_request)), "cancel after final failed");
    check(english.events.size() == 2 && english.events.back().kind == EventKind::final &&
              english.events.back().consumed_samples == 3323,
          "wrong terminal event count or tail watermark");
    check(one.value->submit(chunk("en", 2, 3323, 1)).code == ErrorCode::invalid_state,
          "post-EOF audio accepted");
    const auto previous = chinese.events.back().text;
    check(static_cast<bool>(two.value->cancel(CancelReason::deadline)), "cancel failed");
    check(static_cast<bool>(two.value->cancel(CancelReason::deadline)), "duplicate cancel failed");
    check(chinese.events.size() == 2 && chinese.events.back().kind == EventKind::stopped &&
              chinese.events.back().text == previous &&
              chinese.events.back().status.code == ErrorCode::deadline_expired,
          "cancel should retain last partial and emit one stopped event");
    check(!two.value->finish_input(), "cancelled session became successful");
    check(english.events[0].sequence == 0 && english.events[1].sequence == 1 &&
              english.events[1].revision > english.events[0].revision,
          "event ordering invalid");
    cfg.language = "xx";
    check(engine.create_session(cfg, english, clock).status.code == ErrorCode::unsupported,
          "unsupported language accepted");
}
void configurations(const std::filesystem::path &directory) {
    const auto file = directory / "config.yaml";
    auto write = [&](const std::string &text) {
        std::ofstream out(file);
        out << text;
    };
    write("audio:\n  chunk_ms: 100\noutput:\n  directory: artifacts\n");
    const auto config = resolve_config(file, {"audio.chunk_ms=500", "dataset.language=id"});
    check(config.chunk_ms == 500 && config.duration_ms == 1200 && config.language == "id",
          "configuration precedence failed");
    check(config.output_directory == directory / "artifacts", "relative output base incorrect");
    for (const std::string value :
         {"audio:\n  chunk_ms: 100\n  chunk_ms: 200\n", "unknown: 1\n", "audio:\n  channels: 2\n",
          "audio:\n  chunk_ms: -1\n", "audio:\n  realtime_pacing: yes\n", "model:\n  runtime: qwen_native\n",
          "experiment:\n  name: !unsafe value\n", "---\n{}\n---\n{}\n",
          "audio:\n  duration_ms: 9999999999999999999999999\n", "audio: null\n"}) {
        write(value);
        rejects([&] { (void)resolve_config(file); }, "invalid YAML accepted: " + value);
    }
    rejects([] { (void)resolve_config({}, {"audio.chunk_ms=0"}); }, "invalid CLI range accepted");
    rejects([] { (void)resolve_config({}, {"audio.unknown=1"}); }, "unknown override accepted");
    rejects([] { (void)resolve_config({}, {"audio.queue_max_ms=100"}); },
            "queue smaller than chunk accepted");
    rejects([] { (void)resolve_config({}, {"audio.queue_max_bytes=6399"}); },
            "byte bound smaller than chunk accepted");
    rejects([] { (void)resolve_config({}, {"audio.late_tolerance_ms=1001"}); },
            "late tolerance above maximum lag accepted");
    const auto native = resolve_config(
        {}, {"model.runtime=qwen_native", "model.path=missing-model", "audio.realtime_pacing=true"});
    check(native.runtime == "qwen_native" && native.native_threads == 4 && native.refine_final,
          "native settings did not resolve");
    rejects([] { (void)resolve_config({}, {"model.runtime=qwen_native", "model.path=missing-model"}); },
            "native simulated pacing accepted");
    rejects([] { (void)resolve_config({}, {"model.threads=17"}); },
            "unsupported native thread count accepted");
    rejects([] { (void)resolve_config({}, {"model.decode_step_ms=500"}); },
            "unsupported native decode step accepted");
    const auto managed = resolve_config({}, {"workers.processes=2", "workers.scheduler=round_robin"});
    check(managed.worker_processes == 2 && managed.scheduler == "round_robin" &&
              managed.resolved["workers"]["executor"] == "in_process" &&
              managed.resolved["workers"]["threads_per_process"] == managed.native_threads,
          "M5 worker layout did not resolve");
    rejects([] { (void)resolve_config({}, {"workers.inference_slots_per_process=2"}); },
            "unproven shared runtime accepted");
    rejects([] { (void)resolve_config({}, {"workers.processes=5"}); },
            "unsafe provisional process count accepted");
    rejects([] { (void)resolve_config({}, {"workers.scheduler=unknown"}); },
            "unsupported scheduler accepted");
    rejects([] { (void)resolve_config({}, {"cpu.cores=[0,0]"}); },
            "duplicate affinity CPU accepted");
    rejects([] { (void)resolve_config({}, {"cpu.affinity_enabled=true", "cpu.cores=[0,1]"}); },
            "mock affinity accepted");
}
void runners_and_repositories(const std::filesystem::path &directory) {
    auto config = resolve_config({}, {"audio.duration_ms=1250"});
    MockEngine engine;
    FakeClock clock;
    SyntheticPcmSource source(20000, config.seed);
    MemoryResultRepository memory;
    const auto result = run_mock_baseline(config, engine, source, clock, memory, "test");
    check(result["chunks"] == 7 && result["audio_samples"] == 20000 &&
              result["elapsed_clock_ns"] == 1250000000,
          "baseline pacing or tail count incorrect");
    check(memory.events.size() == 4 && memory.events.front().produced_ns == 400000000 &&
              memory.status == "COMPLETE",
          "mock timeline or persistence incorrect");
    rejects([&] { memory.append(memory.events.front()); }, "append after seal accepted");
    FakeClock second_clock;
    SyntheticPcmSource second_source(20000, config.seed);
    FileResultRepository disk(directory / "runs");
    const auto second_result = run_mock_baseline(config, engine, second_source, second_clock, disk, "test");
    check(second_result == result, "changing repository changed experiment output");
    std::ifstream status_file(disk.directory() / "status.json");
    const auto status = nlohmann::json::parse(status_file);
    check(status["status"] == "COMPLETE" && status["is_mock"] == true, "status artifact incorrect");
    std::ifstream events_file(disk.directory() / "events.jsonl");
    std::string line;
    std::size_t index = 0;
    while (std::getline(events_file, line)) {
        check(nlohmann::json::parse(line) == event_json(memory.events.at(index++)),
              "file/in-memory event disagreement");
    }
    check(index == memory.events.size(), "events missing on disk");
    check(memory.audio_timings.size() == 7, "per-chunk audio timings missing");
    std::ifstream audio_file(disk.directory() / "audio_timing.jsonl");
    index = 0;
    while (std::getline(audio_file, line)) {
        check(nlohmann::json::parse(line) == memory.audio_timings.at(index++),
              "file/in-memory audio timing disagreement");
    }
    check(index == memory.audio_timings.size(), "audio timings missing on disk");
    FileResultRepository collision(directory / "runs");
    rejects([&] { collision.begin("test", {}, {}); }, "existing run overwritten");
    FileResultRepository unsafe(directory / "runs");
    rejects([&] { unsafe.begin("../escape", {}, {}); }, "path traversal run ID accepted");
    config = resolve_config({}, {"audio.duration_ms=0"});
    FakeClock empty_clock;
    SyntheticPcmSource empty_source(0, 42);
    MemoryResultRepository empty_repository;
    (void)run_mock_baseline(config, engine, empty_source, empty_clock, empty_repository, "empty");
    check(empty_repository.events.size() == 1 && empty_repository.events[0].text.empty(),
          "empty stream contract failed");
    config = resolve_config();
    FakeClock failed_clock;
    SyntheticPcmSource short_source(1, 42);
    MemoryResultRepository failed_repository;
    rejects(
        [&] {
            (void)run_mock_baseline(config, engine, short_source, failed_clock, failed_repository, "failed");
        },
        "early source EOF reported success");
    check(failed_repository.status == "FAILED", "failure did not persist terminal status");
    check(failed_repository.records["calls"].at(0)["status"] == "FAILED" &&
              failed_repository.records["errors"].size() == 1 &&
              failed_repository.summary.contains("measurements"),
          "failed call lost measurements/error evidence");
}
} // namespace
int main() {
    try {
        TempDirectory temporary;
        clock_and_source();
        session_contract();
        configurations(temporary.path);
        runners_and_repositories(temporary.path);
        std::cout << "clock, PCM, session isolation/lifecycle, strict configuration, and interchangeable "
                     "repositories passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
