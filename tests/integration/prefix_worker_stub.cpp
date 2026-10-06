#include "process_wire.hpp"
#include <map>
#include <mutex>
#include <thread>
using namespace asr;
using namespace asr::prefix_wire;
int main(int argc, char **argv) {
    if (argc != 9)
        return 1;
    std::mutex mutex;
    std::map<std::string, std::string> ids;
    std::vector<std::thread> tasks;
    auto out = [&](Json j) {
        std::lock_guard lock(mutex);
        prefix_wire::send(3, j);
    };
    out({{"type", "boot"}});
    Json j;
    while (prefix_wire::receive(3, j)) {
        auto op = j.at("op").get<std::string>();
        auto request = j.at("request");
        auto reply = [&, request](Status status = {}, Json value = Json::object()) {
            out({{"type", "reply"},
                 {"request", request},
                 {"status", prefix_wire::status(status)},
                 {"value", value}});
        };
        auto id = j.value("call_id", std::string{});
        if (op == "create") {
            if (id == "capacity_0" && j.value("prefix_preview_ms", 0) != 2000) {
                reply({ErrorCode::invalid_input, "missing preview override"});
                continue;
            }
            ids[id] = j.at("run_id");
            reply();
        } else if (op == "audio") {
            if (std::string(argv[1]) == "crash")
                _exit(42);
            reply();
        } else if (op == "drop") {
            ids.erase(id);
            reply();
        } else if (op == "eof" || op == "cancel") {
            auto run = ids.at(id);
            auto worker = std::string(argv[8]);
            tasks.emplace_back([=, &out] {
                if (op == "eof")
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                RecognitionEvent e;
                e.call_id = id;
                e.run_id = run;
                e.worker_id = worker;
                e.kind = op == "eof" ? EventKind::final : EventKind::stopped;
                e.text = id;
                e.consumed_samples = 3200;
                SteadyClock c;
                e.produced_ns = c.now_ns();
                e.clock_domain = c.domain();
                out(prefix_wire::event(e));
                reply();
            });
        } else if (op == "status") {
            PrefixWorkerStatus s;
            s.max_calls = std::stoi(argv[2]);
            s.runtime_threads = s.blas_threads = std::stoi(argv[4]);
            reply({}, prefix_wire::worker_status(s));
        } else
            reply();
    }
    for (auto &t : tasks)
        t.join();
}
