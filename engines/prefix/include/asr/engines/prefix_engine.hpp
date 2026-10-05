#pragma once
#include <asr/engines/engine.hpp>
#include <memory>
#include <string>

namespace asr {
struct PrefixShared;
struct PrefixCallStatus {
    std::string call_id, language;
    SessionState state = SessionState::ready;
    std::int64_t buffered_samples = 0;
    bool decoding = false;
};
struct PrefixWorkerStatus {
    int max_calls = 0, runtime_threads = 0, blas_threads = 0, queued_jobs = 0;
    bool draining = false;
    std::uint64_t failures = 0;
    std::string last_error;
    std::vector<PrefixCallStatus> calls;
};
// Experimental causal prefix redecoder. One loaded model, serial inference,
// multiple active call sessions. It is not a resumable native streaming decoder.
class PrefixMultiplexEngine final : public IASREngine {
    std::shared_ptr<PrefixShared> shared_;

  public:
    explicit PrefixMultiplexEngine(std::string model_directory, int max_calls = 2,
                                   int preview_ms = 4000, int threads = 4,
                                   int idle_timeout_ms = 30000, int total_timeout_ms = 600000,
                                   int decode_timeout_ms = 45000);
    ~PrefixMultiplexEngine() override;
    EngineCapabilities capabilities() const override;
    PrefixWorkerStatus worker_status() const;
    void begin_draining();
    Result<std::unique_ptr<IASRSession>> create_session(const SessionConfig &config,
                                                        IRecognitionSink &sink, IClock &clock) override;
};
} // namespace asr
