#pragma once
#include <asr/audio/source.hpp>
#include <asr/core/clock.hpp>
#include <deque>

namespace asr {
enum class DeliveryState { running, draining, completed, stopped, failed };
struct DeliveryLimits {
    std::size_t max_chunks = 8;
    int max_audio_ms = 1000;
    int max_lag_ms = 1000;
    int late_tolerance_ms = 5;
    std::size_t max_bytes = 32000;
};
struct DeliveryStats {
    std::int64_t produced_samples = 0, delivered_samples = 0, discarded_samples = 0;
    std::uint64_t delivered_chunks = 0, late_chunks = 0, overflows = 0;
    std::int64_t max_lag_ns = 0;
    std::size_t peak_chunks = 0, peak_samples = 0;
    std::size_t peak_bytes = 0;
};
struct AudioTiming {
    std::uint64_t sequence = 0;
    std::int64_t first_sample = 0, sample_count = 0;
    std::int64_t scheduled_ready_ns = 0, source_read_ns = 0, sent_ns = 0, lag_ns = 0;
    std::size_t queued_samples_after_pop = 0;
    std::int64_t enqueued_ns = 0;
};
struct DeliveredChunk {
    AudioChunk chunk;
    AudioTiming timing;
};
// Single-owner, nonblocking controller. No internal threads. Call produce_ready,
// pop, and controls from one serialized executor; wait outside this object.
// Future source.read calls are withheld until their absolute sample deadlines.
class PacedAudioStream {
    struct Pending {
        AudioChunk chunk;
        std::int64_t read_ns;
        std::int64_t enqueued_ns;
    };
    IAudioSource *source_;
    IClock &clock_;
    std::string run_id_, call_id_;
    std::int64_t total_, origin_, produced_ = 0;
    int chunk_samples_;
    DeliveryLimits limits_;
    DeliveryState state_ = DeliveryState::running;
    DeliveryStats stats_;
    Status error_;
    std::deque<Pending> queue_;
    std::size_t queued_samples_ = 0;
    std::uint64_t next_sequence_ = 0, expected_sequence_ = 0;
    std::int64_t expected_sample_ = 0;
    bool graceful_ = false;
    bool eof_ = false;
    Status fail(ErrorCode code, const std::string &message);
    void settle();

  public:
    PacedAudioStream(IAudioSource &source, IClock &clock, std::string run_id, std::string call_id,
                     std::int64_t total_samples, int chunk_ms, DeliveryLimits limits = {});
    Status produce_ready();
    Result<std::optional<DeliveredChunk>> pop();
    // Normal EOF validates the complete sequence/sample watermark, then drains.
    Status finish_input(std::uint64_t expected_sequence, std::int64_t expected_samples);
    // Graceful stop drains only queued audio, without claiming normal EOF.
    void graceful_stop();
    // Immediate cancellation discards queued audio; does not emit a successful EOF.
    void cancel();
    // New generation requires a distinct call ID and a fresh/rewound source.
    void reset(IAudioSource &source, std::string new_call_id, std::int64_t total_samples);
    std::int64_t next_deadline_ns() const;
    std::int64_t origin_ns() const { return origin_; }
    DeliveryState state() const { return state_; }
    const DeliveryStats &stats() const { return stats_; }
    const Status &error() const { return error_; }
    bool graceful() const { return graceful_; }
};
} // namespace asr
