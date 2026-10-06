#include "process_wire.hpp"
#include <asr/core/clock.hpp>
#include <atomic>
#include <iostream>
#include <map>
#include <mutex>
#include <signal.h>
#include <sys/prctl.h>
#include <thread>
using namespace asr;
using namespace asr::prefix_wire;
namespace {
std::mutex output_mutex;
void output(const Json &j) {
    std::lock_guard lock(output_mutex);
    if (!prefix_wire::send(3, j))
        throw std::runtime_error("prefix IPC output closed");
}
struct Call final : IRecognitionSink {
    std::string id;
    std::unique_ptr<IASRSession> session;
    explicit Call(std::string value) : id(std::move(value)) {}
    void on_event(const RecognitionEvent &e) override { output(prefix_wire::event(e)); }
    void on_observation(const RuntimeObservation &o) override { output(prefix_wire::observation(o, id)); }
};
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 9)
            throw std::invalid_argument("prefix worker arguments");
        auto parent = getppid();
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent)
            return 1;
        SteadyClock clock;
        PrefixMultiplexEngine engine(argv[1], std::stoi(argv[2]), std::stoi(argv[3]), std::stoi(argv[4]),
                                     std::stoi(argv[5]), std::stoi(argv[6]), std::stoi(argv[7]), argv[8]);
        std::map<std::string, std::shared_ptr<Call>> calls;
        // EOF/cancel can wait for a decode. Keep ingress for other calls moving.
        struct Operation {
            std::thread thread;
            std::shared_ptr<std::atomic<bool>> done;
        };
        std::vector<Operation> operations;
        output({{"type", "boot"}, {"status", prefix_wire::status(Status{})}});
        Json command;
        while (prefix_wire::receive(3, command)) {
            for (auto it = operations.begin(); it != operations.end();) {
                if (it->done->load()) {
                    it->thread.join();
                    it = operations.erase(it);
                } else
                    ++it;
            }
            const auto request = command.at("request").get<std::uint64_t>();
            auto reply = [request](Status s, Json value = Json::object()) {
                output({{"type", "reply"},
                        {"request", request},
                        {"status", prefix_wire::status(s)},
                        {"value", value}});
            };
            try {
                const auto op = command.at("op").get<std::string>();
                if (op == "status") {
                    reply({}, prefix_wire::worker_status(engine.worker_status()));
                    continue;
                }
                if (op == "drain") {
                    engine.begin_draining();
                    reply({});
                    continue;
                }
                const auto id = command.at("call_id").get<std::string>();
                if (op == "create") {
                    if (calls.contains(id)) {
                        reply({ErrorCode::invalid_input, "duplicate call ID"});
                        continue;
                    }
                    SessionConfig config;
                    config.call_id = id;
                    config.run_id = command.at("run_id");
                    config.language = command.at("language");
                    config.max_chunk_samples = command.at("max_chunk_samples");
                    config.sample_rate_hz = command.at("sample_rate_hz");
                    auto call = std::make_shared<Call>(id);
                    auto result = engine.create_session(config, *call, clock);
                    if (result) {
                        call->session = std::move(result.value);
                        calls.emplace(id, call);
                    }
                    reply(result.status);
                    continue;
                }
                auto found = calls.find(id);
                if (found == calls.end()) {
                    reply({ErrorCode::invalid_state, "unknown call"});
                    continue;
                }
                auto call = found->second;
                if (op == "audio") {
                    AudioChunk c;
                    c.call_id = id;
                    c.run_id = command.at("run_id");
                    c.sequence = command.at("sequence");
                    c.first_sample = command.at("first_sample");
                    c.sample_rate_hz = command.at("sample_rate_hz");
                    c.channels = command.at("channels");
                    c.pcm = std::make_shared<const std::vector<std::int16_t>>(
                        command.at("pcm").get<std::vector<std::int16_t>>());
                    reply(call->session->submit(std::move(c)));
                } else if (op == "eof" || op == "cancel") {
                    auto reason = static_cast<CancelReason>(command.value("reason", 0));
                    auto done = std::make_shared<std::atomic<bool>>(false);
                    operations.push_back({std::thread([call, op, reason, reply, done]() mutable {
                                              Status status;
                                              try {
                                                  status = op == "eof" ? call->session->finish_input()
                                                                       : call->session->cancel(reason);
                                              } catch (const std::exception &e) {
                                                  status = {ErrorCode::runtime_failure, e.what()};
                                              }
                                              call.reset();
                                              reply(status);
                                              done->store(true);
                                          }),
                                          done});
                } else if (op == "drop") {
                    // Parent sends drop only after finish/cancel has acknowledged.
                    calls.erase(found);
                    call.reset();
                    reply({});
                } else
                    reply({ErrorCode::invalid_input, "unknown prefix IPC operation"});
            } catch (const std::exception &e) {
                reply({ErrorCode::runtime_failure, e.what()});
            }
        }
        for (auto &[id, c] : calls)
            c->session->cancel(CancelReason::shutdown);
        for (auto &operation : operations)
            if (operation.thread.joinable())
                operation.thread.join();
        calls.clear();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "prefix worker: " << e.what() << '\n';
        return 1;
    }
}
