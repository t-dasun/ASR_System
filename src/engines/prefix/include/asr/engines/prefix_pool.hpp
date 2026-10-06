#pragma once
#include <asr/engines/prefix_engine.hpp>
#include <filesystem>
#include <mutex>
namespace asr {
struct PrefixProcess;
struct PrefixPoolWorkerStatus {
    std::string worker_id;
    int process_id = 0;
    bool healthy = false;
    PrefixWorkerStatus status;
};
class PrefixProcessPool final : public IASREngine {
    std::vector<std::shared_ptr<PrefixProcess>> workers_;
    mutable std::mutex mutex_;
    bool draining_ = false;
    std::size_t tie_break_ = 0;
    std::string scheduler_;

  public:
    PrefixProcessPool(const std::filesystem::path &executable, std::string model, int workers, int slots,
                      int preview_ms, int threads, int idle_ms, int total_ms, int decode_ms,
                      std::string scheduler = "least_active");
    ~PrefixProcessPool() override;
    EngineCapabilities capabilities() const override;
    std::vector<PrefixPoolWorkerStatus> workers();
    void begin_draining();
    Result<std::unique_ptr<IASRSession>> create_session(const SessionConfig &, IRecognitionSink &,
                                                        IClock &) override;
};
} // namespace asr
