#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace asr {
enum class ErrorCode {
    none,
    invalid_input,
    unsupported,
    invalid_state,
    resource_exhausted,
    deadline_expired,
    runtime_failure,
    cancelled
};
struct Status {
    ErrorCode code = ErrorCode::none;
    std::string message;
    explicit operator bool() const { return code == ErrorCode::none; }
};
template <class T> struct Result {
    Status status;
    T value{};
    explicit operator bool() const { return static_cast<bool>(status); }
};
enum class SessionState { creating, ready, streaming, finalizing, completed, stopped, failed };
enum class EventKind { partial, final, stopped, failed };
enum class CancelReason { user_request, deadline, shutdown };
using PcmBuffer = std::shared_ptr<const std::vector<std::int16_t>>;
struct AudioChunk {
    std::string run_id;
    std::string call_id;
    std::uint64_t sequence = 0;
    std::int64_t first_sample = 0;
    int sample_rate_hz = 16000;
    int channels = 1;
    std::int64_t scheduled_ready_ns = 0;
    std::int64_t sent_ns = 0;
    PcmBuffer pcm;
};
struct RecognitionEvent {
    int schema_version = 1;
    std::string run_id, call_id, producer_id = "mock";
    std::string worker_id;
    std::uint64_t sequence = 0, revision = 0;
    std::int64_t produced_ns = 0, consumed_samples = 0;
    std::optional<std::int64_t> published_ns;
    std::string clock_domain;
    std::optional<std::string> utc;
    EventKind kind = EventKind::partial;
    bool before_eof = false;
    std::string text;
    Status status;
};
// Runtime observations are independent of vendor types. Missing boundaries stay absent.
struct RuntimeObservation {
    std::string stage;
    std::string worker_id;
    std::int64_t timestamp_ns = 0;
    std::optional<std::int64_t> duration_ns, sequence, buffered_samples, cpu_ns, peak_rss_bytes;
    int process_id = 0;
    std::optional<std::int64_t> counter_value;
};
struct SessionConfig {
    std::string run_id, call_id, language = "en";
    std::uint32_t seed = 42;
    int sample_rate_hz = 16000;
    int partial_every_ms = 400;
    std::int64_t max_chunk_samples = 16000;
    // Zero retains engine defaults; optional per-call controls do not reload weights.
    int decode_step_ms = 0;
    int prefix_preview_ms = 0;
};
struct SessionSnapshot {
    SessionState state = SessionState::ready;
    std::string worker_id;
    std::int64_t consumed_samples = 0;
    std::uint64_t revision = 0;
    std::string text;
};
struct EngineCapabilities {
    std::string engine_id, revision, streaming_kind;
    bool is_mock = true;
    std::string device = "cpu", precision = "synthetic";
    std::vector<std::string> languages{"en", "id", "zh"};
    int sample_rate_hz = 16000, channels = 1;
    bool cooperative_cancellation = true, aligned_timestamps = false, batching = false;
    bool concurrent_sessions = false;
    std::string transcript_semantics = "full_snapshot";
};
} // namespace asr
