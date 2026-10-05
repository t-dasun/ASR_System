#include <asr/engines/prefix_engine.hpp>
extern "C" {
#include "qwen_asr.h"
#include "qwen_asr_kernels.h"
#include <cblas.h>
}
#include <omp.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

namespace asr {
namespace {
constexpr std::int64_t max_call_samples = 16000LL * 60;
// The pinned native runtime owns mutable process globals. Only one shared
// context may own them; REST suites reuse this engine instead of loading another.
std::mutex native_runtime_owner;

struct Call {
    SessionConfig config;
    IRecognitionSink *sink;
    IClock *clock;
    std::vector<float> audio;
    SessionSnapshot snapshot;
    std::uint64_t next_sequence = 0, event_sequence = 0;
    std::int64_t created_ns = 0, last_audio_ns = 0;
    std::int64_t preview_ready_ns = 0, eof_received_ns = 0;
    bool model_info_sent = false;
    bool eof = false, cancelled = false, terminal = false, busy = false, preview_done = false;
    Status terminal_status;
    std::condition_variable completed;
    Call(SessionConfig value, IRecognitionSink &output, IClock &source)
        : config(std::move(value)), sink(&output), clock(&source) {
        snapshot.worker_id = "prefix_shared_0";
        created_ns = last_audio_ns = source.now_ns();
    }
};

const char *language_name(const std::string &language) {
    if (language == "en") return "English";
    if (language == "id") return "Indonesian";
    if (language == "zh") return "Chinese";
    return nullptr;
}

RecognitionEvent make_event(Call &call, EventKind kind, std::int64_t samples,
                            std::string text, Status status, bool before_eof) {
    RecognitionEvent event;
    event.run_id = call.config.run_id;
    event.call_id = call.config.call_id;
    event.producer_id = "qwen_prefix_multiplex";
    event.worker_id = "prefix_shared_0";
    event.sequence = call.event_sequence++;
    event.revision = ++call.snapshot.revision;
    event.produced_ns = call.clock->now_ns();
    event.clock_domain = call.clock->domain();
    event.utc = call.clock->utc_now();
    event.consumed_samples = samples;
    event.kind = kind;
    event.before_eof = before_eof;
    event.text = std::move(text);
    event.status = std::move(status);
    call.snapshot.text = event.text;
    call.snapshot.consumed_samples = samples;
    return event;
}
} // namespace

struct PrefixShared {
    std::unique_lock<std::mutex> runtime_owner;
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::shared_ptr<Call>> calls;
    std::unique_ptr<qwen_ctx_t, decltype(&qwen_free)> model{nullptr, qwen_free};
    std::thread inference;
    std::size_t next_call = 0;
    int max_calls, preview_samples, runtime_threads, idle_timeout_ms, total_timeout_ms, decode_timeout_ms;
    int blas_threads = 0;
    std::uint64_t failures = 0;
    std::string last_error;
    std::int64_t model_load_ns = 0, model_loaded_ns = 0;
    bool stopping = false, draining = false;

    PrefixShared(const std::string &directory, int limit, int preview_ms, int threads,
                 int idle_ms, int total_ms, int decode_ms)
        : runtime_owner(native_runtime_owner, std::try_to_lock), max_calls(limit),
          preview_samples(0), runtime_threads(threads), idle_timeout_ms(idle_ms),
          total_timeout_ms(total_ms), decode_timeout_ms(decode_ms) {
        if (!runtime_owner.owns_lock())
            throw std::runtime_error("a shared Qwen context already owns this process runtime");
        if (limit < 1 || limit > 8 || preview_ms < 1000 || preview_ms > 20000 ||
            threads < 1 || threads > 16 || idle_ms < 1 || total_ms < idle_ms || decode_ms < 1)
            throw std::invalid_argument("invalid experimental prefix worker settings");
        preview_samples = preview_ms * 16;
        // BLAS may initialize before main. Its explicit runtime control is
        // required in addition to qwen_set_threads (which controls custom kernels).
        setenv("QWEN_BF16_CACHE_MB", "0", 1);
        omp_set_dynamic(0);
        omp_set_num_threads(threads);
        openblas_set_num_threads(threads);
        blas_threads = openblas_get_num_threads();
        if (blas_threads != threads)
            throw std::runtime_error("OpenBLAS refused the configured compute thread budget");
        qwen_verbose = 0;
        qwen_set_threads(threads);
        const auto load_start = std::chrono::steady_clock::now();
        model.reset(qwen_load(directory.c_str()));
        model_loaded_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        model_load_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - load_start).count();
        if (!model) throw std::runtime_error("cannot load Qwen model for prefix worker");
        inference = std::thread([this] { loop(); });
    }
    ~PrefixShared() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (inference.joinable()) inference.join();
    }

    struct Job {
        std::shared_ptr<Call> call;
        std::vector<float> samples;
        bool final = false;
        Status status;
        std::int64_t ready_ns = 0, eof_ns = 0;
    };

    Job select() {
        if (calls.empty()) return {};
        for (std::size_t count = 0; count < calls.size(); ++count) {
            const std::size_t index = (next_call + count) % calls.size();
            auto &call = calls[index];
            if (call->busy || call->cancelled || call->terminal) continue;
            const auto now = call->clock->now_ns();
            const bool expired = now - call->created_ns >= std::int64_t(total_timeout_ms) * 1000000 ||
                (!call->eof && now - call->last_audio_ns >= std::int64_t(idle_timeout_ms) * 1000000);
            const bool final = call->eof;
            if (!expired && !final && (call->preview_done ||
                           call->audio.size() < static_cast<std::size_t>(preview_samples)))
                continue;
            call->busy = true;
            next_call = (index + 1) % calls.size();
            Job job;
            job.call = call;
            if (expired) {
                job.status = {ErrorCode::deadline_expired, "shared call idle or total deadline expired"};
                return job;
            }
            job.final = final;
            job.ready_ns = final ? call->eof_received_ns : call->preview_ready_ns;
            job.eof_ns = call->eof_received_ns;
            const auto count_samples = final ? call->audio.size() :
                static_cast<std::size_t>(preview_samples);
            job.samples.assign(call->audio.begin(), call->audio.begin() + count_samples);
            return job;
        }
        return {};
    }

    void loop() {
        // OpenMP-backed OpenBLAS keeps a thread-local runtime budget.
        omp_set_dynamic(0);
        omp_set_num_threads(runtime_threads);
        openblas_set_num_threads(runtime_threads);
        {
            std::lock_guard lock(mutex);
            blas_threads = openblas_get_num_threads();
        }
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                if (stopping) break;
                job = select();
                if (!job.call) {
                    wake.wait_for(lock, std::chrono::milliseconds(20));
                    continue;
                }
            }
            std::string text;
            Status status = job.status;
            const auto decode_started_ns = job.call->clock->now_ns();
            const auto decode_start = std::chrono::steady_clock::now();
            try {
                if (status && !job.samples.empty()) {
                    if (qwen_set_force_language(model.get(),
                                                language_name(job.call->config.language)) != 0)
                        throw std::runtime_error("native language selection failed");
                    std::unique_ptr<char, decltype(&std::free)> result(
                        qwen_transcribe_audio(model.get(), job.samples.data(),
                                              static_cast<int>(job.samples.size())), std::free);
                    if (!result) throw std::runtime_error("native prefix decode failed");
                    text = result.get();
                }
            } catch (const std::exception &error) {
                status = {ErrorCode::runtime_failure, error.what()};
            }
            const auto decode_finished_ns = job.call->clock->now_ns();
            if (status && (std::chrono::steady_clock::now() - decode_start >=
                           std::chrono::milliseconds(decode_timeout_ms) ||
                           job.call->clock->now_ns() - job.call->created_ns >=
                           std::int64_t(total_timeout_ms) * 1000000))
                status = {ErrorCode::deadline_expired, "shared call decode or total deadline expired"};

            RecognitionEvent event;
            bool publish = false, model_info = false, terminal = job.final || !status;
            {
                std::lock_guard lock(mutex);
                auto &call = *job.call;
                if (!call.cancelled) {
                    model_info = !call.model_info_sent;
                    call.model_info_sent = true;
                    const bool pre_eof = !job.final && !call.eof && !text.empty();
                    event = make_event(call, !status ? EventKind::failed :
                                       job.final ? EventKind::final : EventKind::partial,
                                       static_cast<std::int64_t>(job.status ? job.samples.size() : call.audio.size()),
                                       !status && text.empty() ? call.snapshot.text : std::move(text), status, pre_eof);
                    if (!job.final) call.preview_done = true;
                    call.snapshot.state = !status ? SessionState::failed :
                                          job.final ? SessionState::completed : SessionState::streaming;
                    publish = true;
                }
            }
            if (publish) {
                try {
                    const auto observe = [&](const char *stage, std::int64_t timestamp,
                                             std::optional<std::int64_t> duration) {
                        RuntimeObservation value;
                        value.stage = stage;
                        value.worker_id = "prefix_shared_0";
                        value.process_id = getpid();
                        value.timestamp_ns = timestamp;
                        value.duration_ns = duration;
                        value.sequence = job.final ? 1 : 0;
                        value.buffered_samples = static_cast<std::int64_t>(job.samples.size());
                        job.call->sink->on_observation(value);
                    };
                    if (model_info)
                        observe("shared_model_load", model_loaded_ns, model_load_ns);
                    if (job.final)
                        observe("worker_eof_received", job.eof_ns, std::nullopt);
                    if (job.status && !job.samples.empty()) {
                        observe(job.final ? "eof_decode_queue_wait" : "prefix_decode_queue_wait",
                                decode_started_ns, decode_started_ns - job.ready_ns);
                        observe(job.final ? "eof_refinement" : "prefix_decode",
                                decode_finished_ns, decode_finished_ns - decode_started_ns);
                    }
                    job.call->sink->on_event(event);
                }
                catch (...) { status = {ErrorCode::runtime_failure, "result delivery failed"}; terminal = true; }
            }
            {
                std::lock_guard lock(mutex);
                job.call->busy = false;
                if (terminal && !job.call->cancelled) {
                    job.call->terminal = true;
                    job.call->terminal_status = status;
                    if (!status) {
                        job.call->snapshot.state = SessionState::failed;
                        ++failures;
                        last_error = status.message;
                    }
                }
                job.call->completed.notify_all();
            }
            wake.notify_all();
        }
    }
};

namespace {
class PrefixSession final : public IASRSession {
    std::shared_ptr<PrefixShared> shared_;
    std::shared_ptr<Call> call_;

  public:
    PrefixSession(std::shared_ptr<PrefixShared> shared, std::shared_ptr<Call> call)
        : shared_(std::move(shared)), call_(std::move(call)) {}
    ~PrefixSession() override {
        std::unique_lock lock(shared_->mutex);
        call_->cancelled = true;
        shared_->wake.notify_all();
        call_->completed.wait(lock, [&] { return !call_->busy; });
        auto &calls = shared_->calls;
        calls.erase(std::remove(calls.begin(), calls.end(), call_), calls.end());
        shared_->next_call = 0;
    }
    Status submit(AudioChunk chunk) override {
        std::lock_guard lock(shared_->mutex);
        if (call_->terminal && !call_->terminal_status) return call_->terminal_status;
        if (call_->eof || call_->cancelled || call_->terminal)
            return {ErrorCode::invalid_state, "audio after terminal state"};
        if (chunk.run_id != call_->config.run_id || chunk.call_id != call_->config.call_id ||
            chunk.sequence != call_->next_sequence ||
            chunk.first_sample != static_cast<std::int64_t>(call_->audio.size()) ||
            chunk.sample_rate_hz != 16000 || chunk.channels != 1 || !chunk.pcm || chunk.pcm->empty() ||
            chunk.pcm->size() > static_cast<std::size_t>(call_->config.max_chunk_samples))
            return {ErrorCode::invalid_input, "invalid PCM identity, order, or format"};
        if (call_->audio.size() + chunk.pcm->size() > static_cast<std::size_t>(max_call_samples))
            return {ErrorCode::resource_exhausted, "experimental call audio limit exceeded"};
        for (auto sample : *chunk.pcm)
            call_->audio.push_back(static_cast<float>(sample) / 32768.0f);
        ++call_->next_sequence;
        call_->last_audio_ns = call_->clock->now_ns();
        if (!call_->preview_ready_ns && call_->audio.size() >= static_cast<std::size_t>(shared_->preview_samples))
            call_->preview_ready_ns = call_->last_audio_ns;
        call_->snapshot.state = SessionState::streaming;
        call_->snapshot.consumed_samples = static_cast<std::int64_t>(call_->audio.size());
        shared_->wake.notify_all();
        return {};
    }
    Status finish_input() override {
        std::unique_lock lock(shared_->mutex);
        if (call_->terminal) return call_->terminal_status;
        if (call_->cancelled) return {ErrorCode::cancelled, "call cancelled"};
        if (!call_->eof) call_->eof_received_ns = call_->clock->now_ns();
        call_->eof = true;
        call_->snapshot.state = SessionState::finalizing;
        shared_->wake.notify_all();
        call_->completed.wait(lock, [&] { return call_->terminal || call_->cancelled; });
        return call_->cancelled ? Status{ErrorCode::cancelled, "call cancelled"} : call_->terminal_status;
    }
    Status cancel(CancelReason reason) override {
        std::unique_lock lock(shared_->mutex);
        if (call_->terminal) return call_->terminal_status;
        call_->cancelled = true;
        shared_->wake.notify_all();
        call_->completed.wait(lock, [&] { return !call_->busy; });
        call_->snapshot.state = SessionState::stopped;
        Status status{reason == CancelReason::deadline ? ErrorCode::deadline_expired : ErrorCode::cancelled,
                      "experimental prefix call cancelled"};
        auto event = make_event(*call_, EventKind::stopped,
                                static_cast<std::int64_t>(call_->audio.size()),
                                call_->snapshot.text, status, false);
        call_->terminal = true;
        call_->terminal_status = status;
        lock.unlock();
        try { call_->sink->on_event(event); }
        catch (...) { return {ErrorCode::runtime_failure, "result delivery failed"}; }
        return {};
    }
    SessionSnapshot snapshot() const override {
        std::lock_guard lock(shared_->mutex);
        return call_->snapshot;
    }
};
} // namespace

PrefixMultiplexEngine::PrefixMultiplexEngine(std::string directory, int max_calls,
                                             int preview_ms, int threads, int idle_ms, int total_ms,
                                             int decode_ms)
    : shared_(std::make_shared<PrefixShared>(directory, max_calls, preview_ms, threads,
                                           idle_ms, total_ms, decode_ms)) {}
PrefixMultiplexEngine::~PrefixMultiplexEngine() = default;

PrefixWorkerStatus PrefixMultiplexEngine::worker_status() const {
    std::lock_guard lock(shared_->mutex);
    PrefixWorkerStatus status;
    status.max_calls = shared_->max_calls;
    status.runtime_threads = shared_->runtime_threads;
    status.blas_threads = shared_->blas_threads;
    status.draining = shared_->draining;
    status.failures = shared_->failures;
    status.last_error = shared_->last_error;
    for (const auto &call : shared_->calls) {
        status.calls.push_back({call->config.call_id, call->config.language,
                               call->snapshot.state, static_cast<std::int64_t>(call->audio.size()),
                               call->busy});
        if (!call->busy && !call->cancelled && !call->terminal &&
            (call->eof || (!call->preview_done &&
                          call->audio.size() >= static_cast<std::size_t>(shared_->preview_samples))))
            ++status.queued_jobs;
    }
    return status;
}

void PrefixMultiplexEngine::begin_draining() {
    std::lock_guard lock(shared_->mutex);
    shared_->draining = true;
}

EngineCapabilities PrefixMultiplexEngine::capabilities() const {
    EngineCapabilities value;
    value.engine_id = "qwen_prefix_multiplex_experimental";
    value.revision = "prefix_v1";
    value.streaming_kind = "causal_prefix_redecode";
    value.is_mock = false;
    value.device = "cpu";
    value.precision = "bf16_weights";
    value.cooperative_cancellation = false;
    value.concurrent_sessions = true;
    value.transcript_semantics = "revisable_full_snapshot";
    return value;
}

Result<std::unique_ptr<IASRSession>> PrefixMultiplexEngine::create_session(
    const SessionConfig &config, IRecognitionSink &sink, IClock &clock) {
    if (!language_name(config.language) || config.sample_rate_hz != 16000 ||
        clock.domain() != "host_steady")
        return {{ErrorCode::unsupported, "experimental engine supports en/id/zh at 16 kHz with host clock"}, nullptr};
    if (config.run_id.empty() || config.call_id.empty() || config.max_chunk_samples < 1 ||
        config.max_chunk_samples > 16000)
        return {{ErrorCode::invalid_input, "invalid experimental session configuration"}, nullptr};
    std::lock_guard lock(shared_->mutex);
    if (shared_->draining)
        return {{ErrorCode::invalid_state, "shared worker is draining"}, nullptr};
    if (shared_->calls.size() >= static_cast<std::size_t>(shared_->max_calls))
        return {{ErrorCode::resource_exhausted, "all shared-model call slots occupied"}, nullptr};
    for (const auto &existing : shared_->calls)
        if (existing->config.call_id == config.call_id)
            return {{ErrorCode::invalid_input, "duplicate active call ID"}, nullptr};
    auto call = std::make_shared<Call>(config, sink, clock);
    shared_->calls.push_back(call);
    shared_->wake.notify_all();
    return {{}, std::make_unique<PrefixSession>(shared_, std::move(call))};
}
} // namespace asr
