#pragma once
#include <asr/engines/engine.hpp>

namespace asr {
class MockEngine final : public IASREngine {
  public:
    EngineCapabilities capabilities() const override;
    Result<std::unique_ptr<IASRSession>> create_session(const SessionConfig &config, IRecognitionSink &sink,
                                                        IClock &clock) override;
};
} // namespace asr
