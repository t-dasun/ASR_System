#pragma once
#include <asr/core/clock.hpp>
#include <asr/core/types.hpp>

namespace asr {
class IRecognitionSink {
  public:
    virtual ~IRecognitionSink() = default;
    virtual void on_event(const RecognitionEvent &event) = 0;
    virtual void on_observation(const RuntimeObservation &) {}
};
// Calls on one session are serialized. The sink and clock must outlive the session.
// No callback may outlive a session. A rejected chunk transfers no ownership.
class IASRSession {
  public:
    virtual ~IASRSession() = default;
    virtual Status submit(AudioChunk chunk) = 0;
    virtual Status finish_input() = 0;
    virtual Status cancel(CancelReason reason) = 0;
    virtual SessionSnapshot snapshot() const = 0;
};
class IASREngine {
  public:
    virtual ~IASREngine() = default;
    virtual EngineCapabilities capabilities() const = 0;
    virtual Result<std::unique_ptr<IASRSession>> create_session(const SessionConfig &config,
                                                                IRecognitionSink &sink, IClock &clock) = 0;
};
} // namespace asr
