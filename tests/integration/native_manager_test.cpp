#include <asr/backend/session_manager.hpp>
#include <asr/engines/native_engine.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
void check(bool result, const char *message) {
    if (!result)
        throw std::runtime_error(message);
}
struct Sink : asr::IRecognitionSink {
    std::vector<asr::RecognitionEvent> events;
    void on_event(const asr::RecognitionEvent &event) override { events.push_back(event); }
};
asr::SessionConfig config(std::string id, std::string language) {
    asr::SessionConfig result;
    result.run_id = "m5_native";
    result.call_id = std::move(id);
    result.language = std::move(language);
    result.max_chunk_samples = 3200;
    return result;
}
asr::AudioChunk chunk(const std::string &id) {
    return {
        "m5_native", id, 0, 0, 16000, 1, 0, 0, std::make_shared<const std::vector<std::int16_t>>(3200, 7)};
}
struct Directory {
    std::filesystem::path path;
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::invalid_argument("expected stub worker path");
        Directory fixture{std::filesystem::temp_directory_path() /
                          ("asr_m5_native_" + std::to_string(getpid()))};
        std::filesystem::create_directory(fixture.path);
        std::ofstream(fixture.path / "model.safetensors").put('x');
        std::ofstream(fixture.path / "acquisition.json")
            << "{\"status\":\"verified\",\"revision\":\"5eb144179a02acc5e5ba31e748d22b0cf3e303b0\"}";
        asr::NativeQwenOptions options;
        options.worker_executable = argv[1];
        options.model_directory = fixture.path;
        options.timeout_ms = 1000;
        asr::WorkerLayout layout;
        layout.processes = 2;
        layout.runtime_threads = options.threads;
        asr::SessionManager manager(
            layout,
            std::make_unique<asr::FactoryWorkerExecutor>(
                "process", [options] { return std::make_unique<asr::NativeQwenEngine>(options); }),
            std::make_unique<asr::RoundRobinScheduler>());
        check(manager.capabilities().concurrent_sessions && manager.executor_kind() == "process",
              "process-isolated manager capability missing");
        asr::SteadyClock clock;
        unsetenv("ASR_NATIVE_STUB_MODE");
        Sink en, id, zh;
        auto english = manager.create_session(config("english", "en"), en, clock);
        setenv("ASR_NATIVE_STUB_MODE", "crash", 1);
        auto indonesian = manager.create_session(config("indonesian", "id"), id, clock);
        unsetenv("ASR_NATIVE_STUB_MODE");
        check(english && indonesian, "two process sessions did not start");
        auto occupied = manager.workers();
        check(occupied[0].process_id > 0 && occupied[1].process_id > 0 &&
                  occupied[0].process_id != occupied[1].process_id && occupied[0].call_id == "english" &&
                  occupied[1].call_id == "indonesian",
              "workers were not distinct sticky processes");
        asr::Status en_status, id_status;
        std::thread a([&] {
            en_status = english.value->submit(chunk("english"));
            if (en_status)
                en_status = english.value->finish_input();
        });
        std::thread b([&] {
            id_status = indonesian.value->submit(chunk("indonesian"));
            if (id_status)
                id_status = indonesian.value->finish_input();
        });
        a.join();
        b.join();
        check(en_status && english.value->snapshot().state == asr::SessionState::completed &&
                  english.value->snapshot().text == "stub final English" && en.events.size() == 2 &&
                  en.events.back().worker_id == "worker_0_g1",
              "healthy language corrupted by another worker crash");
        check(!id_status && indonesian.value->snapshot().state == asr::SessionState::failed &&
                  !id.events.empty() && id.events.back().kind == asr::EventKind::failed &&
                  id.events.back().worker_id == "worker_1_g1" && manager.workers()[1].failures == 1,
              "crashed worker not isolated/reported");
        indonesian.value.reset();
        check(!manager.workers()[1].occupied && manager.workers()[0].occupied,
              "crashed worker slot did not recover independently");
        for (int iteration = 0; iteration < 5; ++iteration) {
            const auto name = "chinese_" + std::to_string(iteration);
            auto current = manager.create_session(config(name, "zh"), zh, clock);
            check(current &&
                      current.value->snapshot().worker_id == "worker_1_g" + std::to_string(iteration + 2),
                  "worker generation or repeated admission incorrect");
            check(static_cast<bool>(current.value->submit(chunk(name))) &&
                      static_cast<bool>(current.value->finish_input()) &&
                      current.value->snapshot().text == "stub final Chinese",
                  "recreated context leaked prior language/text");
            current.value.reset();
        }
        check(english.value->snapshot().text == "stub final English" && manager.workers()[1].failures == 1,
              "repeated worker cycles changed existing call or failure history");
        std::cout << "M5 native process isolation, two languages, crash, and cycles passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
