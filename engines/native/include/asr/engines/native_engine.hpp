#pragma once
#include <asr/engines/engine.hpp>
#include <filesystem>

namespace asr {
struct NativeQwenOptions {
    std::filesystem::path worker_executable, model_directory;
    int threads = 4, decode_step_ms = 2000, max_new_tokens = 32, timeout_ms = 45000;
    bool refine_final = true;
};
// One isolated worker process per session. No vendor header crosses this boundary.
class NativeQwenEngine final : public IASREngine {
    NativeQwenOptions options_;

  public:
    explicit NativeQwenEngine(NativeQwenOptions options);
    EngineCapabilities capabilities() const override;
    Result<std::unique_ptr<IASRSession>> create_session(const SessionConfig &config, IRecognitionSink &sink,
                                                        IClock &clock) override;
};
} // namespace asr
