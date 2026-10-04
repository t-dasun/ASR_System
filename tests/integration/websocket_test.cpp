#include <arpa/inet.h>
#include <asr/backend/session_manager.hpp>
#include <asr/backend/websocket.hpp>
#include <asr/benchmark/load.hpp>
#include <asr/engines/mock_engine.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int connect_local(std::uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    check(fd >= 0, "raw WebSocket fixture socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    check(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
          "raw WebSocket fixture connect failed");
    timeval timeout{2, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return fd;
}
void recv_exact(int fd, void *target, std::size_t size) {
    auto *bytes = static_cast<unsigned char *>(target);
    while (size) {
        const auto count = recv(fd, bytes, size, 0);
        check(count > 0, "observer frame receive failed");
        bytes += count;
        size -= std::size_t(count);
    }
}
nlohmann::json observer_frame(int fd) {
    unsigned char header[2];
    recv_exact(fd, header, 2);
    check(header[0] == 0x81 && (header[1] & 0x80) == 0, "observer frame is not plain text");
    std::size_t count = header[1] & 0x7f;
    if (count == 126) {
        unsigned char length[2];
        recv_exact(fd, length, 2);
        count = (std::size_t(length[0]) << 8) | length[1];
    }
    check(count < 65536, "observer frame oversized");
    std::string payload(count, '\0');
    if (count)
        recv_exact(fd, payload.data(), count);
    return nlohmann::json::parse(payload);
}
void client_text(int fd, const nlohmann::json &value) {
    const auto payload = value.dump();
    check(payload.size() < 65536, "raw client JSON too large");
    std::vector<unsigned char> frame;
    frame.push_back(0x81);
    if (payload.size() < 126)
        frame.push_back(0x80 | unsigned(payload.size()));
    else {
        frame.push_back(0x80 | 126);
        frame.push_back(unsigned(payload.size() >> 8));
        frame.push_back(unsigned(payload.size()));
    }
    const unsigned char mask[4] = {1, 2, 3, 4};
    frame.insert(frame.end(), mask, mask + 4);
    for (std::size_t i = 0; i < payload.size(); ++i)
        frame.push_back(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
    check(send(fd, frame.data(), frame.size(), MSG_NOSIGNAL) == ssize_t(frame.size()),
          "raw WebSocket frame send failed");
}
void client_pcm_zeros(int fd, std::size_t samples) {
    const auto size = samples * 2;
    check(size < 65536, "PCM fixture too large");
    std::vector<unsigned char> frame{0x82, 0x80 | 126,
                                     static_cast<unsigned char>(size >> 8),
                                     static_cast<unsigned char>(size)};
    const unsigned char mask[4] = {5, 6, 7, 8};
    frame.insert(frame.end(), mask, mask + 4);
    for (std::size_t index = 0; index < size; ++index)
        frame.push_back(mask[index % 4]); // zero PCM XOR mask
    std::size_t sent = 0;
    while (sent < frame.size()) {
        const auto count = send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
        check(count > 0, "PCM fixture send failed");
        sent += std::size_t(count);
    }
}
void expect_ack(int fd, std::uint64_t sequence) {
    for (int index = 0; index < 16; ++index) {
        const auto frame = observer_frame(fd);
        if (frame.value("type", "") == "ack") {
            check(frame["sequence"] == sequence && frame["status"]["code"] == "none" &&
                      frame["credits"] == 1,
                  "jitter fixture lost one-credit ACK");
            return;
        }
    }
    throw std::runtime_error("jitter fixture ACK missing");
}
int raw_session(std::uint16_t port, const std::string &id) {
    const auto fd = connect_local(port);
    const auto upgrade = std::string("GET /v1/asr HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n") +
                         "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                         "Origin: http://127.0.0.1\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    check(send(fd, upgrade.data(), upgrade.size(), MSG_NOSIGNAL) == ssize_t(upgrade.size()),
          "raw session upgrade send failed");
    std::string response;
    char byte;
    while (!response.ends_with("\r\n\r\n") && response.size() < 8192) {
        check(recv(fd, &byte, 1, 0) == 1, "raw session upgrade response missing");
        response.push_back(byte);
    }
    check(response.starts_with("HTTP/1.1 101 "), "raw session upgrade rejected");
    client_text(fd, {{"v", 1},
                     {"type", "start"},
                     {"run_id", id},
                     {"call_id", id},
                     {"language", "en"},
                     {"seed", 42},
                     {"max_chunk_samples", 3200},
                     {"partial_every_ms", 400},
                     {"sample_rate_hz", 16000}});
    for (int i = 0; i < 8; ++i) {
        const auto frame = observer_frame(fd);
        if (frame.value("type", "") == "ready") {
            check(frame["status"]["code"] == "none" && frame["credits"] == 1,
                  "raw session not ready with one credit");
            return fd;
        }
    }
    throw std::runtime_error("raw session ready missing");
}
int open_observer(std::uint16_t port, const std::string &run_id, const std::string &call_id,
                  std::uint64_t since = 0) {
    const auto fd = connect_local(port);
    const auto upgrade = "GET /v1/observe?run_id=" + run_id + "&call_id=" + call_id +
                         "&since=" + std::to_string(since) +
                         " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                         "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                         "Origin: http://127.0.0.1\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    check(send(fd, upgrade.data(), upgrade.size(), MSG_NOSIGNAL) == ssize_t(upgrade.size()),
          "observer upgrade send failed");
    std::string response;
    char byte;
    while (!response.ends_with("\r\n\r\n") && response.size() < 8192) {
        check(recv(fd, &byte, 1, 0) == 1, "observer upgrade response missing");
        response.push_back(byte);
    }
    check(response.starts_with("HTTP/1.1 101 "), "observer upgrade failed");
    return fd;
}
class CountingSink final : public asr::IRecognitionSink {
  public:
    int events = 0;
    void on_event(const asr::RecognitionEvent &) override { ++events; }
};
} // namespace
int main() {
    try {
        const auto root =
            std::filesystem::temp_directory_path() / ("asr_m6_websocket_" + std::to_string(getpid()));
        auto config = asr::resolve_config({}, {"audio.duration_ms=200", "audio.realtime_pacing=true",
                                               "workers.processes=2", "metrics.resource_sampling=false"});
        config.output_directory = root;
        asr::WorkerLayout layout;
        layout.processes = 2;
        asr::SessionManager manager(layout,
                                    std::make_unique<asr::FactoryWorkerExecutor>(
                                        "in_process", [] { return std::make_unique<asr::MockEngine>(); }),
                                    std::make_unique<asr::RoundRobinScheduler>());
        asr::WebSocketServer server(manager);
        {
            const auto fd = connect_local(server.port());
            const std::string bad_origin = "GET /v1/asr HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                                           "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                                           "Origin: http://evil.example\r\n"
                                           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
            check(send(fd, bad_origin.data(), bad_origin.size(), 0) ==
                      static_cast<ssize_t>(bad_origin.size()),
                  "invalid-origin fixture send failed");
            char reply;
            check(recv(fd, &reply, 1, 0) == 0, "WebSocket server accepted forbidden origin");
            close(fd);
        }
        {
            const auto fd = connect_local(server.port());
            const std::string upgrade = "GET /v1/asr HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                                        "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                                        "Origin: http://127.0.0.1\r\n"
                                        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
            check(send(fd, upgrade.data(), upgrade.size(), 0) == static_cast<ssize_t>(upgrade.size()),
                  "upgrade fixture send failed");
            std::string response;
            char byte;
            while (!response.ends_with("\r\n\r\n") && response.size() < 8192) {
                check(recv(fd, &byte, 1, 0) == 1, "upgrade fixture missing response");
                response.push_back(byte);
            }
            check(response.starts_with("HTTP/1.1 101 "), "valid upgrade rejected");
            const unsigned char unmasked_start[] = {0x81, 0x02, '{', '}'};
            check(send(fd, unmasked_start, sizeof(unmasked_start), 0) ==
                      static_cast<ssize_t>(sizeof(unmasked_start)),
                  "unmasked fixture send failed");
            check(recv(fd, &byte, 1, 0) == 0, "WebSocket server accepted unmasked client frame");
            close(fd);
        }
        asr::WebSocketEngine ingress(server.port(), manager.capabilities());
        {
            const auto fd = raw_session(server.port(), "bad_eof_fixture");
            client_text(fd, {{"v", 1}, {"type", "eof"}, {"expected_next_sequence", 1}, {"total_samples", 0}});
            std::string remainder;
            char buffer[4096];
            while (true) {
                const auto size = recv(fd, buffer, sizeof(buffer), 0);
                if (size == 0)
                    break;
                check(size > 0, "bad EOF connection did not close");
                remainder.append(buffer, size);
            }
            check(remainder.find("\"type\":\"done\"") == std::string::npos, "mismatched EOF was accepted");
            close(fd);
        }
        {
            const auto fd = raw_session(server.port(), "duplicate_eof_fixture");
            client_text(fd, {{"v", 1}, {"type", "eof"}, {"expected_next_sequence", 0}, {"total_samples", 0}});
            int done = 0;
            for (int i = 0; i < 8; ++i) {
                const auto frame = observer_frame(fd);
                if (frame.value("type", "") == "done") {
                    ++done;
                    break;
                }
            }
            char byte;
            check(done == 1 && recv(fd, &byte, 1, 0) == 0, "EOF did not terminate connection after one done");
            close(fd);
        }
        {
            const auto fd = raw_session(server.port(), "disconnect_fixture");
            close(fd);
            bool released = false;
            for (int i = 0; i < 50; ++i) {
                released = true;
                for (const auto &worker : manager.workers())
                    released &= worker.call_id != "disconnect_fixture";
                if (released)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(released, "disconnected client retained worker slot");
        }
        {
            const auto fd = raw_session(server.port(), "jitter_fixture");
            for (std::uint64_t sequence = 0; sequence < 2; ++sequence) {
                if (sequence == 1)
                    std::this_thread::sleep_for(std::chrono::milliseconds(350));
                client_text(fd, {{"v", 1}, {"type", "chunk"}, {"sequence", sequence},
                                 {"first_sample", sequence * 3200}, {"sample_count", 3200}});
                client_pcm_zeros(fd, 3200);
                expect_ack(fd, sequence);
            }
            client_text(fd, {{"v", 1}, {"type", "eof"},
                             {"expected_next_sequence", 2}, {"total_samples", 6400}});
            bool done = false;
            for (int index = 0; index < 16; ++index) {
                if (observer_frame(fd).value("type", "") == "done") {
                    done = true;
                    break;
                }
            }
            check(done, "delayed network chunk did not reach terminal state");
            close(fd);
        }
        asr::LoadSpec spec;
        spec.calls = 2;
        spec.concurrency = 2;
        spec.languages = {"en", "id"};
        spec.mode = "network";
        const auto plan = asr::plan_load(config, spec, 3200, 4ULL * 1024 * 1024 * 1024);
        check(plan.allowed, "network preflight failed");
        const auto result = asr::run_load(config, ingress, plan, 3200, nullptr, {{"source", "synthetic"}});
        check(result["status"] == "COMPLETE" && result["mode"] == "network" &&
                  result["completed_calls"] == 2 && result["metrics"]["offered_calls"] == 2,
              "network load did not complete over WebSocket");
        const auto &calls = result["phases"][0]["calls"];
        check(calls[0]["summary"]["worker_id"] != calls[1]["summary"]["worker_id"] &&
                  calls[0]["summary"]["chunks"] == 1 && calls[1]["summary"]["chunks"] == 1 &&
                  calls[0]["summary"]["mode"] == "network" &&
                  calls[0]["summary"]["measurements"]["publication_boundary"] ==
                      "load-client sink receipt after WebSocket transport; includes network latency, no UI",
              "WebSocket ingress did not preserve isolated sessions/chunk counts");
        {
            const auto run_id = calls[0]["run_id"].get<std::string>();
            const auto fd = open_observer(server.port(), run_id, run_id + "_call_0");
            bool saw_event = false, saw_done = false;
            for (int i = 0; i < 32; ++i) {
                const auto frame = observer_frame(fd);
                saw_event |= frame.value("type", "") == "event";
                if (frame.value("type", "") == "observer_done") {
                    saw_done = true;
                    break;
                }
            }
            check(saw_event && saw_done, "observer reconnect replay missed transcript or terminal state");
            close(fd);
        }
        {
            auto longer = config;
            longer.duration_ms = 800;
            asr::LoadSpec stream_spec;
            stream_spec.calls = 1;
            stream_spec.concurrency = 1;
            stream_spec.languages = {"en"};
            stream_spec.mode = "network";
            const auto stream_plan = asr::plan_load(longer, stream_spec, 12800, 4ULL * 1024 * 1024 * 1024);
            const auto stream_result =
                asr::run_load(longer, ingress, stream_plan, 12800, nullptr, {{"source", "synthetic"}});
            check(stream_result["status"] == "COMPLETE", "incremental network call failed");
            const auto directory =
                std::filesystem::path(stream_result["phases"][0]["calls"][0]["directory"].get<std::string>());
            std::ifstream events(directory / "events.jsonl");
            bool before_eof = false;
            std::string line;
            while (std::getline(events, line)) {
                const auto event = nlohmann::json::parse(line);
                before_eof |= event.value("kind", "") == "partial" && event.value("before_eof", false);
            }
            check(before_eof, "network streaming did not publish a transcript before EOF");
        }
        {
            const std::string id = "slow_observer_fixture";
            const auto unread = open_observer(server.port(), id, id + "_call_0");
            asr::SessionConfig session_config;
            session_config.run_id = id;
            session_config.call_id = id + "_call_0";
            session_config.partial_every_ms = 1;
            session_config.max_chunk_samples = 16;
            asr::SteadyClock clock;
            CountingSink sink;
            auto created = ingress.create_session(session_config, sink, clock);
            check(created && created.value, "chatty WebSocket session failed");
            auto session = std::move(created.value);
            for (std::uint64_t sequence = 0; sequence < 140; ++sequence) {
                auto pcm = std::make_shared<std::vector<std::int16_t>>(16, 0);
                asr::AudioChunk chunk{
                    id, id + "_call_0", sequence, std::int64_t(sequence * 16), 16000, 1, 0, 0, pcm};
                check(bool(session->submit(std::move(chunk))),
                      "slow observer blocked incremental audio ingress");
            }
            check(bool(session->finish_input()), "chatty session EOF failed");
            session.reset();
            close(unread);
            const auto replay = open_observer(server.port(), id, id + "_call_0");
            const auto first = observer_frame(replay);
            check(first.value("type", "") == "gap" && first["first_available_sequence"] > 1,
                  "bounded observer replay did not report evicted messages");
            bool saw_final = false, saw_done = false;
            for (int i = 0; i < 130; ++i) {
                const auto frame = observer_frame(replay);
                if (frame.value("type", "") == "event" && frame["event"]["kind"] == "final")
                    saw_final = true;
                if (frame.value("type", "") == "observer_done") {
                    saw_done = true;
                    break;
                }
            }
            check(saw_final && saw_done && sink.events > 128,
                  "slow observer affected transcript delivery or terminal replay");
            close(replay);
        }
        std::filesystem::remove_all(root);
        std::cout << "M6/M7 WebSocket network load path passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
