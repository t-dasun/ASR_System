#pragma once
#include <asr/backend/websocket.hpp>
#include <asr/config/config.hpp>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace asr {
// The HTTP adapter never owns an engine. Its injected executor is the same
// load/sweep implementation used by the CLI; tests can replace it with a mock.
class ApiService final : public IHttpHandler {
  public:
    using SuiteExecutor =
        std::function<nlohmann::json(const nlohmann::json &, bool, const std::atomic<bool> &)>;
    using RuntimeSnapshot = std::function<nlohmann::json()>;
    ApiService(std::filesystem::path config_path, std::vector<std::string> base_overrides,
               std::filesystem::path output_root, nlohmann::json capabilities, SuiteExecutor execute,
               ServiceAdmissionGate *gate = nullptr, RuntimeSnapshot runtime = {});
    ~ApiService() override;
    HttpResponse handle(const HttpRequest &) override;

  private:
    struct Job {
        std::string id;
        std::atomic<bool> cancel{false};
        nlohmann::json state;
        std::thread thread;
    };
    std::filesystem::path config_path_, output_root_;
    std::vector<std::string> base_overrides_;
    nlohmann::json capabilities_;
    SuiteExecutor execute_;
    RuntimeSnapshot runtime_;
    ServiceAdmissionGate *gate_ = nullptr;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    std::map<std::string, std::string> idempotency_;
    std::map<std::string, std::string> idempotency_bodies_;
};
} // namespace asr
