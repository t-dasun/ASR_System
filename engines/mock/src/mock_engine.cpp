#include <algorithm>
#include <asr/engines/mock_engine.hpp>

namespace asr {
namespace {
class MockSession final : public IASRSession {
    SessionConfig config_;
    IRecognitionSink &sink_;
    IClock &clock_;
    SessionSnapshot state_;
    std::uint64_t sequence_ = 0, event_sequence_ = 0;
    std::int64_t next_partial_;
    bool terminal() const {
        return state_.state == SessionState::completed || state_.state == SessionState::stopped ||
               state_.state == SessionState::failed;
    }
    void emit(EventKind kind, Status status = {}) {
        RecognitionEvent event;
        event.run_id = config_.run_id;
        event.call_id = config_.call_id;
        event.sequence = event_sequence_++;
        event.revision = ++state_.revision;
        event.produced_ns = clock_.now_ns();
        event.clock_domain = clock_.domain();
        event.utc = clock_.utc_now();
        event.consumed_samples = state_.consumed_samples;
        event.kind = kind;
        event.before_eof = kind == EventKind::partial;
        event.text = state_.text;
        event.status = std::move(status);
        sink_.on_event(event);
    }
    void update_text() {
        state_.text = "MOCK " + config_.language + " seed=" + std::to_string(config_.seed) +
                      " samples=" + std::to_string(state_.consumed_samples);
    }

  public:
    MockSession(SessionConfig config, IRecognitionSink &sink, IClock &clock)
        : config_(std::move(config)), sink_(sink), clock_(clock),
          next_partial_(static_cast<std::int64_t>(config_.sample_rate_hz) * config_.partial_every_ms / 1000) {
    }
    Status submit(AudioChunk chunk) override {
        if (terminal())
            return {ErrorCode::invalid_state, "audio after terminal state"};
        if (chunk.run_id != config_.run_id || chunk.call_id != config_.call_id ||
            chunk.sequence != sequence_ || chunk.first_sample != state_.consumed_samples ||
            chunk.sample_rate_hz != config_.sample_rate_hz || chunk.channels != 1 || !chunk.pcm ||
            chunk.pcm->empty())
            return {ErrorCode::invalid_input, "invalid audio identity, order, or format"};
        const auto count = static_cast<std::int64_t>(chunk.pcm->size());
        if (count > config_.max_chunk_samples || state_.consumed_samples + count > 16000 * 600)
            return {ErrorCode::resource_exhausted, "mock audio limit exceeded"};
        ++sequence_;
        state_.state = SessionState::streaming;
        state_.consumed_samples += count;
        if (state_.consumed_samples >= next_partial_) {
            update_text();
            const auto step =
                static_cast<std::int64_t>(config_.sample_rate_hz) * config_.partial_every_ms / 1000;
            next_partial_ = (state_.consumed_samples / step + 1) * step;
            emit(EventKind::partial);
        }
        return {};
    }
    Status finish_input() override {
        if (state_.state == SessionState::completed)
            return {};
        if (terminal())
            return {ErrorCode::invalid_state, "EOF on stopped or failed session"};
        state_.state = SessionState::completed;
        if (state_.consumed_samples)
            update_text();
        emit(EventKind::final);
        return {};
    }
    Status cancel(CancelReason reason) override {
        if (terminal())
            return {};
        state_.state = SessionState::stopped;
        emit(EventKind::stopped,
             {reason == CancelReason::deadline ? ErrorCode::deadline_expired : ErrorCode::cancelled,
              "mock session cancelled"});
        return {};
    }
    SessionSnapshot snapshot() const override { return state_; }
};
} // namespace
EngineCapabilities MockEngine::capabilities() const {
    EngineCapabilities value;
    value.engine_id = "mock";
    value.revision = "mock_v1";
    value.streaming_kind = "synthetic_incremental";
    return value;
}
Result<std::unique_ptr<IASRSession>> MockEngine::create_session(const SessionConfig &config,
                                                                IRecognitionSink &sink, IClock &clock) {
    const auto languages = capabilities().languages;
    if (std::find(languages.begin(), languages.end(), config.language) == languages.end() ||
        config.sample_rate_hz != 16000)
        return {{ErrorCode::unsupported, "mock supports en/id/zh at 16 kHz"}, nullptr};
    if (config.run_id.empty() || config.call_id.empty() || config.partial_every_ms < 1 ||
        config.partial_every_ms > 60000 || config.max_chunk_samples < 1 || config.max_chunk_samples > 16000)
        return {{ErrorCode::invalid_input, "invalid mock session settings"}, nullptr};
    return {{}, std::make_unique<MockSession>(config, sink, clock)};
}
} // namespace asr
