#pragma once
#include <asr/backend/service_gate.hpp>
#include <asr/engines/engine.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace asr {
class ObservationHub;
struct HttpRequest {
    std::string method, target, origin, body;
};
struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};
class IHttpHandler {
  public:
    virtual ~IHttpHandler() = default;
    virtual HttpResponse handle(const HttpRequest &) = 0;
};
// Loopback-only v1 HTTP/WebSocket service boundary. The server owns no model:
// every audio connection enters the injected SessionManager through IASREngine.
class WebSocketServer {
    IASREngine &engine_;
    IHttpHandler *http_ = nullptr;
    ServiceAdmissionGate *gate_ = nullptr;
    std::unique_ptr<ObservationHub> observations_;
    int listener_ = -1;
    std::uint16_t port_ = 0;
    std::thread accept_thread_;
    std::mutex mutex_;
    std::vector<int> clients_;
    struct Handler {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<Handler> handlers_;
    std::atomic<bool> stopping_{false};
    void accept_loop();
    void handle_client(int fd);
    void handle_observer(int fd, const std::string &request);

  public:
    explicit WebSocketServer(IASREngine &engine, std::uint16_t port = 0, IHttpHandler *http = nullptr,
                             ServiceAdmissionGate *gate = nullptr);
    ~WebSocketServer();
    WebSocketServer(const WebSocketServer &) = delete;
    WebSocketServer &operator=(const WebSocketServer &) = delete;
    std::uint16_t port() const { return port_; }
};

class WebSocketEngine final : public IASREngine {
    std::uint16_t port_;
    EngineCapabilities capabilities_;

  public:
    WebSocketEngine(std::uint16_t port, EngineCapabilities capabilities);
    EngineCapabilities capabilities() const override { return capabilities_; }
    Result<std::unique_ptr<IASRSession>> create_session(const SessionConfig &, IRecognitionSink &,
                                                        IClock &) override;
};
} // namespace asr
