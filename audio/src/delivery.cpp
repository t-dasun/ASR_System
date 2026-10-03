#include <asr/audio/delivery.hpp>
#include <asr/core/chunk_schedule.hpp>

namespace asr {
PacedAudioStream::PacedAudioStream(IAudioSource &source, IClock &clock, std::string run_id,
                                   std::string call_id, std::int64_t total_samples, int chunk_ms,
                                   DeliveryLimits limits)
    : source_(&source), clock_(clock), run_id_(std::move(run_id)), call_id_(std::move(call_id)),
      total_(total_samples), origin_(clock.now_ns()), chunk_samples_(chunk_samples(chunk_ms, 16000)),
      limits_(limits) {
    if (run_id_.empty() || call_id_.empty() || total_ < 0 || total_ > 16000 * 600 || limits.max_chunks < 1 ||
        limits.max_chunks > 10000 || limits.max_audio_ms < chunk_ms || limits.max_audio_ms > 60000 ||
        limits.max_lag_ms < 0 || limits.max_lag_ms > 60000 || limits.late_tolerance_ms < 0 ||
        limits.late_tolerance_ms > limits.max_lag_ms ||
        limits.max_bytes < static_cast<std::size_t>(chunk_samples_) * 2 || limits.max_bytes > 2000000)
        throw std::invalid_argument("invalid paced stream IDs, length, queue, or lateness bounds");
}
Status PacedAudioStream::fail(ErrorCode code, const std::string &message) {
    error_ = {code, message};
    state_ = DeliveryState::failed;
    stats_.discarded_samples += queued_samples_;
    queue_.clear();
    queued_samples_ = 0;
    return error_;
}
void PacedAudioStream::settle() {
    if (state_ == DeliveryState::draining && queue_.empty())
        state_ = DeliveryState::completed;
}
std::int64_t PacedAudioStream::next_deadline_ns() const {
    return origin_ + audio_offset(std::min(total_, produced_ + chunk_samples_), 16000).count();
}
Status PacedAudioStream::produce_ready() {
    if (state_ == DeliveryState::failed)
        return error_;
    if (state_ != DeliveryState::running)
        return {};
    while (produced_ < total_ && clock_.now_ns() >= next_deadline_ns()) {
        const auto deadline = next_deadline_ns();
        const auto lag = clock_.now_ns() - deadline;
        stats_.max_lag_ns = std::max(stats_.max_lag_ns, lag);
        if (lag > std::int64_t(limits_.max_lag_ms) * 1000000)
            return fail(ErrorCode::deadline_expired,
                        "audio source deadline exceeded; media clock not shifted");
        const auto count =
            static_cast<std::size_t>(std::min<std::int64_t>(chunk_samples_, total_ - produced_));
        if (queue_.size() == limits_.max_chunks ||
            count + queued_samples_ > static_cast<std::size_t>(limits_.max_audio_ms) * 16 ||
            (count + queued_samples_) * 2 > limits_.max_bytes) {
            ++stats_.overflows;
            return fail(ErrorCode::resource_exhausted, "bounded audio queue overflow; no silent audio drop");
        }
        const auto read_time = clock_.now_ns();
        auto assembled = std::make_shared<std::vector<std::int16_t>>();
        assembled->reserve(count);
        try {
            while (assembled->size() < count) {
                const auto wanted = count - assembled->size();
                auto part = source_->read(wanted);
                if (!part || part->empty() || part->size() > wanted)
                    return fail(ErrorCode::invalid_input, "source returned invalid or early EOF data");
                assembled->insert(assembled->end(), part->begin(), part->end());
            }
        } catch (const std::exception &exception) {
            return fail(ErrorCode::runtime_failure, exception.what());
        }
        AudioChunk chunk{run_id_, call_id_, next_sequence_++, produced_, 16000, 1, deadline, 0, assembled};
        queue_.push_back({std::move(chunk), read_time, clock_.now_ns()});
        produced_ += count;
        queued_samples_ += count;
        stats_.produced_samples = produced_;
        stats_.peak_chunks = std::max(stats_.peak_chunks, queue_.size());
        stats_.peak_samples = std::max(stats_.peak_samples, queued_samples_);
        stats_.peak_bytes = std::max(stats_.peak_bytes, queued_samples_ * 2);
    }
    return {};
}
Status PacedAudioStream::finish_input(std::uint64_t expected_sequence, std::int64_t expected_samples) {
    if (state_ == DeliveryState::draining || state_ == DeliveryState::completed)
        return eof_ && expected_sequence == next_sequence_ && expected_samples == total_
                   ? Status{}
                   : Status{ErrorCode::invalid_state,
                            "EOF not valid after stop or with mismatched watermark"};
    if (state_ == DeliveryState::failed)
        return error_;
    if (state_ != DeliveryState::running)
        return {ErrorCode::invalid_state, "cannot finish stopped audio"};
    if (expected_sequence != next_sequence_ || expected_samples != total_ || produced_ != total_)
        return {ErrorCode::invalid_input, "EOF before all samples or wrong sequence/total"};
    eof_ = true;
    state_ = DeliveryState::draining;
    settle();
    return {};
}
Result<std::optional<DeliveredChunk>> PacedAudioStream::pop() {
    if (state_ == DeliveryState::failed)
        return {error_, {}};
    if (queue_.empty()) {
        settle();
        return {{}, {}};
    }
    auto &front = queue_.front();
    if (front.chunk.sequence != expected_sequence_ || front.chunk.first_sample != expected_sample_)
        return {fail(ErrorCode::invalid_input, "audio sequence/sample discontinuity"), {}};
    const auto sent = clock_.now_ns(), lag = sent - front.chunk.scheduled_ready_ns;
    stats_.max_lag_ns = std::max(stats_.max_lag_ns, lag);
    if (lag > std::int64_t(limits_.max_lag_ms) * 1000000)
        return {fail(ErrorCode::deadline_expired, "audio delivery deadline exceeded"), {}};
    if (lag > std::int64_t(limits_.late_tolerance_ms) * 1000000)
        ++stats_.late_chunks;
    auto item = std::move(front);
    queue_.pop_front();
    const auto count = static_cast<std::int64_t>(item.chunk.pcm->size());
    queued_samples_ -= count;
    item.chunk.sent_ns = sent;
    AudioTiming timing{item.chunk.sequence,
                       item.chunk.first_sample,
                       count,
                       item.chunk.scheduled_ready_ns,
                       item.read_ns,
                       sent,
                       lag,
                       queued_samples_,
                       item.enqueued_ns};
    ++expected_sequence_;
    expected_sample_ += count;
    stats_.delivered_samples += count;
    ++stats_.delivered_chunks;
    settle();
    return {{}, DeliveredChunk{std::move(item.chunk), timing}};
}
void PacedAudioStream::graceful_stop() {
    if (state_ == DeliveryState::running) {
        graceful_ = true;
        state_ = DeliveryState::draining;
        settle();
    }
}
void PacedAudioStream::cancel() {
    if (state_ != DeliveryState::running && state_ != DeliveryState::draining)
        return;
    stats_.discarded_samples += queued_samples_;
    queue_.clear();
    queued_samples_ = 0;
    state_ = DeliveryState::stopped;
}
void PacedAudioStream::reset(IAudioSource &source, std::string new_call_id, std::int64_t total_samples) {
    if (new_call_id.empty() || new_call_id == call_id_ || total_samples < 0 || total_samples > 16000 * 600)
        throw std::invalid_argument("reset requires a new call ID and valid source length");
    cancel();
    source_ = &source;
    call_id_ = std::move(new_call_id);
    total_ = total_samples;
    origin_ = clock_.now_ns();
    produced_ = expected_sample_ = 0;
    next_sequence_ = expected_sequence_ = 0;
    queue_.clear();
    queued_samples_ = 0;
    stats_ = {};
    error_ = {};
    graceful_ = false;
    eof_ = false;
    state_ = DeliveryState::running;
}
} // namespace asr
