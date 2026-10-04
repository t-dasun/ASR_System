#include <arpa/inet.h>
#include <asr/backend/api_service.hpp>
#include <asr/backend/session_manager.hpp>
#include <asr/engines/mock_engine.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Reply {
    int status = 0;
    nlohmann::json body;
    std::string headers;
};
Reply request(std::uint16_t port, const std::string &method, const std::string &target,
              const std::string &body = "", const std::string &origin = "") {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    check(fd >= 0, "HTTP fixture socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    check(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
          "HTTP fixture connect failed");
    timeval timeout{3, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const auto header =
        method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" +
        (origin.empty() ? "" : "Origin: " + origin + "\r\n") +
        (method == "OPTIONS" ? "Access-Control-Request-Headers: content-type\r\n" : "") +
        (method == "POST"
             ? "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n"
             : "") +
        "Connection: close\r\n\r\n" + body;
    check(send(fd, header.data(), header.size(), MSG_NOSIGNAL) == ssize_t(header.size()),
          "HTTP fixture send failed");
    std::string response;
    char buffer[4096];
    while (true) {
        const auto count = recv(fd, buffer, sizeof(buffer), 0);
        if (count == 0)
            break;
        check(count > 0, "HTTP fixture recv failed");
        response.append(buffer, count);
    }
    close(fd);
    const auto split = response.find("\r\n\r\n");
    check(split != std::string::npos, "HTTP response has no body boundary");
    const auto response_body = response.substr(split + 4);
    return {std::stoi(response.substr(9, 3)),
            response_body.empty() ? nlohmann::json::object() : nlohmann::json::parse(response_body),
            response.substr(0, split)};
}
} // namespace
int main(int argc, char **argv) {
    try {
        check(argc == 2, "config path required");
        const auto root =
            std::filesystem::temp_directory_path() / ("asr_m7_http_" + std::to_string(getpid()));
        std::filesystem::create_directories(root / "safe_run");
        std::filesystem::create_directories(root / "safe_run" / "call_0");
        std::filesystem::create_directories(root / "reports" / "r1");
        {
            std::ofstream stream(root / "safe_run" / "summary.json");
            stream << R"({"status":"COMPLETE","is_mock":true})";
        }
        {
            std::ofstream stream(root / "safe_run" / "call_0" / "config.json");
            stream << R"({"language":"en"})";
        }
        {
            std::ofstream stream(root / "reports" / "r1" / "report.json");
            stream << R"({"report_id":"r1"})";
        }
        std::filesystem::create_directory_symlink(root / "safe_run", root / "linked_run");
        asr::WorkerLayout layout;
        layout.processes = 1;
        asr::SessionManager manager(layout,
                                    std::make_unique<asr::FactoryWorkerExecutor>(
                                        "in_process", [] { return std::make_unique<asr::MockEngine>(); }),
                                    std::make_unique<asr::RoundRobinScheduler>());
        std::atomic<int> executions{0};
        asr::ServiceAdmissionGate gate;
        asr::ApiService api(
            argv[1], {}, root, {{"engine", "mock"}},
            [&](const nlohmann::json &body, bool dry, const std::atomic<bool> &cancelled) -> nlohmann::json {
                ++executions;
                if (body.value("delay", false))
                    for (int i = 0; i < 50 && !cancelled; ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return {{"status", "COMPLETE"}, {"dry_run", dry}, {"calls", body.value("calls", 0)}};
            },
            &gate);
        {
            asr::WebSocketServer server(manager, 0, &api, &gate);
            const auto port = server.port();
            auto capabilities = request(port, "GET", "/v1/capabilities");
            check(capabilities.status == 200 && capabilities.body["engine"] == "mock",
                  "capabilities route failed");
            check(request(port, "GET", "/v1/runtime").body["workers"].is_array() &&
                      request(port, "GET", "/v1/jobs").body["items"].empty(),
                  "read-only dashboard routes failed");
            auto resolved =
                request(port, "POST", "/v1/config/resolve", R"({"overrides":["dataset.language=zh"]})");
            check(resolved.status == 200 && resolved.body["resolved"]["dataset"]["language"] == "zh",
                  "config resolution route failed");
            check(request(port, "POST", "/v1/config/resolve", R"({"overrides":["audio.path=/etc/passwd"]})")
                          .status == 400,
                  "HTTP path override was allowed");
            check(request(port, "GET", "/v1/capabilities", "", "http://evil.example").status == 400,
                  "forbidden HTTP origin was accepted");
            const auto browser = request(port, "GET", "/v1/capabilities", "", "http://localhost:5173");
            check(browser.status == 200 &&
                      browser.headers.find("Access-Control-Allow-Origin: http://localhost:5173") !=
                          std::string::npos,
                  "browser dev origin was not allowed");
            const auto preflight =
                request(port, "OPTIONS", "/v1/config/resolve", "", "http://localhost:5173");
            check(preflight.status == 204 &&
                      preflight.headers.find("Access-Control-Allow-Headers: Content-Type") !=
                          std::string::npos,
                  "browser JSON preflight failed");
            check(request(port, "GET", "/v1/history").body["items"].size() == 1,
                  "history did not list safe artifact");
            check(request(port, "GET", "/v1/history/safe_run").body["status"] == "COMPLETE",
                  "history detail failed");
            check(request(port, "GET", "/v1/artifacts/safe_run/summary.json").status == 200,
                  "artifact retrieval failed");
            check(request(port, "GET", "/v1/artifacts/safe_run/call_0/config.json").body["language"] ==
                      "en",
                  "nested call artifact retrieval failed");
            check(request(port, "GET", "/v1/artifacts/safe_run/../config.json").status == 404,
                  "nested artifact traversal was allowed");
            check(request(port, "GET", "/v1/artifacts/../summary.json").status == 404,
                  "artifact traversal was allowed");
            check(request(port, "GET", "/v1/artifacts/linked_run/summary.json").status == 404,
                  "symlinked artifact directory was allowed");
            check(request(port, "GET", "/v1/reports").body["items"].size() == 1 &&
                      request(port, "GET", "/v1/reports/r1").body["report_id"] == "r1",
                  "report retrieval failed");
            check(request(port, "POST", "/v1/suites/dry-run", R"({"calls":3})").body["dry_run"] == true,
                  "suite dry run failed");
            check(gate.enter_live(), "fixture could not reserve interactive admission");
            check(request(port, "POST", "/v1/suites", R"({"calls":2})").status == 409,
                  "suite started beside an interactive call");
            gate.leave_live();
            auto start =
                request(port, "POST", "/v1/suites", R"({"calls":2,"idempotency_key":"fixture_key"})");
            check(start.status == 202, "suite start failed");
            const auto id = start.body["job_id"].get<std::string>();
            check(request(port, "POST", "/v1/suites", R"({"calls":2,"idempotency_key":"fixture_key"})")
                          .body["job_id"] == id,
                  "idempotency did not return original job");
            check(request(port, "POST", "/v1/suites", R"({"calls":3,"idempotency_key":"fixture_key"})")
                          .status == 409,
                  "idempotency key accepted a different request");
            nlohmann::json state;
            for (int attempt = 0; attempt < 50; ++attempt) {
                state = request(port, "GET", "/v1/jobs/" + id).body;
                if (state.value("status", "") != "RUNNING")
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(state["status"] == "COMPLETE" && executions == 2, "suite job lifecycle failed");
            const auto stopped = request(port, "POST", "/v1/suites", R"({"delay":true})");
            check(stopped.status == 202, "stoppable job did not start");
            check(!gate.enter_live(), "interactive call admitted during benchmark suite");
            const auto stopped_id = stopped.body["job_id"].get<std::string>();
            check(request(port, "POST", "/v1/jobs/" + stopped_id + "/stop").status == 202,
                  "job stop request failed");
            for (int attempt = 0; attempt < 50; ++attempt) {
                state = request(port, "GET", "/v1/jobs/" + stopped_id).body;
                if (state.value("status", "") != "RUNNING")
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(state["status"] == "STOPPED", "job stop state was not observed");
            check(gate.enter_live(), "interactive admission did not reopen after suite");
            gate.leave_live();
        }
        std::filesystem::remove_all(root);
        std::cout << "M7 HTTP API contracts passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
