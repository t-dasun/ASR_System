#include <asr/core/utf8.hpp>
#include <asr/engines/prefix_engine.hpp>
#include <asr/engines/prefix_scheduler.hpp>
extern "C" {
#include "qwen_asr.h"
#include "qwen_asr_kernels.h"
#include <cblas.h>
}
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <omp.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace asr {
namespace {
constexpr std::int64_t max_call_samples = 16000LL * 60;
// The pinned native runtime owns mutable process globals. Only one shared
// context may own them; REST suites reuse this engine instead of loading another.
std::mutex native_runtime_owner;

struct Call {
    SessionConfig config;
    std::unique_ptr<qwen_resumable_state, decltype(&qwen_resumable_destroy)> stream{nullptr,
                                                                                    qwen_resumable_destroy};
    std::int64_t decoded_samples = 0, step_ready_ns = 0;
    int steps = 0;
    std::int64_t reused_prefill = 0;
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
    Call(SessionConfig value, IRecognitionSink &output, IClock &source, const std::string &worker_id)
        : config(std::move(value)), sink(&output), clock(&source) {
        snapshot.worker_id = worker_id;
        created_ns = last_audio_ns = source.now_ns();
    }
};

const char *language_name(const std::string &language) {
    if (language == "en")
        return "English";
    if (language == "id")
        return "Indonesian";
    if (language == "zh")
        return "Chinese";
    return nullptr;
}

RecognitionEvent make_event(Call &call, EventKind kind, std::int64_t samples, std::string text, Status status,
                            bool before_eof) {
    RecognitionEvent event;
    event.run_id = call.config.run_id;
    event.call_id = call.config.call_id;
    event.producer_id = "qwen_prefix_multiplex";
    event.worker_id = call.snapshot.worker_id;
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
    unsigned previews_since_final = 0;
    std::string worker_id;
    int max_calls, preview_samples, runtime_threads, idle_timeout_ms, total_timeout_ms, decode_timeout_ms;
    SharedStreamOptions streaming;
    int blas_threads = 0;
    std::uint64_t failures = 0;
    std::string last_error;
    std::int64_t model_load_ns = 0, model_loaded_ns = 0;
    bool stopping = false, draining = false;

    PrefixShared(const std::string &directory, int limit, int preview_ms, int threads, int idle_ms,
                 int total_ms, int decode_ms, std::string id, SharedStreamOptions stream_options)
        : runtime_owner(native_runtime_owner, std::try_to_lock), max_calls(limit), preview_samples(0),
          runtime_threads(threads), idle_timeout_ms(idle_ms), total_timeout_ms(total_ms),
          decode_timeout_ms(decode_ms) {
        worker_id = std::move(id);
        streaming = stream_options;
        if (streaming.enabled &&
            (streaming.step_ms < 1000 || streaming.step_ms > 8000 || streaming.max_tokens < 1 ||
             streaming.max_tokens > 256 || streaming.unfixed_chunks < 0 || streaming.unfixed_chunks > 4))
            throw std::invalid_argument("invalid resumable stream options");
        if (!runtime_owner.owns_lock())
            throw std::runtime_error("a shared Qwen context already owns this process runtime");
        if (limit < 1 || limit > 8 || preview_ms < 1000 || preview_ms > 20000 || threads < 1 ||
            threads > 16 || idle_ms < 1 || total_ms < idle_ms || decode_ms < 1)
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
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
        model_load_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - load_start)
                            .count();
        if (!model)
            throw std::runtime_error("cannot load Qwen model for prefix worker");
        inference = std::thread([this] { loop(); });
    }
    ~PrefixShared() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (inference.joinable())
            inference.join();
        calls.clear(); // Release borrowed per-call contexts before their model weights.
    }

    int step_samples_for(const Call &call) const {
        return (call.config.decode_step_ms ? call.config.decode_step_ms : streaming.step_ms) * 16;
    }
    bool decode_ready(const Call &call) const {
        return streaming.enabled
                   ? call.audio.size() >=
                         static_cast<std::size_t>(call.decoded_samples + step_samples_for(call))
                   : !call.preview_done &&
                         call.audio.size() >= static_cast<std::size_t>(preview_samples_for(call));
    }

    int preview_samples_for(const Call &call) const {
        return call.config.prefix_preview_ms ? call.config.prefix_preview_ms * 16 : preview_samples;
    }

    struct Job {
        std::shared_ptr<Call> call;
        std::vector<float> samples;
        bool final = false;
        Status status;
        std::int64_t ready_ns = 0, eof_ns = 0;
    };

    Job select() {
        std::vector<PrefixReadyJob> ready;
        for (std::size_t count = 0; count < calls.size(); ++count) {
            const std::size_t index = (next_call + count) % calls.size();
            auto &call = calls[index];
            if (call->busy || call->cancelled || call->terminal)
                continue;
            const auto now = call->clock->now_ns();
            const bool expired =
                now - call->created_ns >= std::int64_t(total_timeout_ms) * 1000000 ||
                (!call->eof && now - call->last_audio_ns >= std::int64_t(idle_timeout_ms) * 1000000);
            if (expired || call->eof || decode_ready(*call))
                ready.push_back({index,
                                 expired             ? call->created_ns
                                 : streaming.enabled ? std::max(call->step_ready_ns, call->eof_received_ns)
                                 : call->eof         ? call->eof_received_ns
                                                     : call->preview_ready_ns,
                                 call->eof && !streaming.enabled, expired});
        }
        const auto selected = select_prefix_job(ready, previews_since_final);
        if (!selected)
            return {};
        auto &call = calls[selected->index];
        call->busy = true;
        next_call = (selected->index + 1) % calls.size();
        Job job;
        job.call = call;
        if (selected->expired) {
            job.status = {ErrorCode::deadline_expired, "shared call idle or total deadline expired"};
            return job;
        }
        job.final = streaming.enabled ? call->eof : selected->final;
        previews_since_final = job.final ? 0 : previews_since_final + 1;
        job.ready_ns = selected->ready_ns;
        job.eof_ns = call->eof_received_ns;
        const auto count_samples =
            streaming.enabled
                ? std::min(call->audio.size(),
                           static_cast<std::size_t>(call->decoded_samples + step_samples_for(*call)))
            : job.final ? call->audio.size()
                        : static_cast<std::size_t>(preview_samples_for(*call));
        // Only pass EOF on the iteration consuming the final delivered sample.
        if (streaming.enabled)
            job.final = call->eof && count_samples == call->audio.size();
        job.samples.assign(call->audio.begin(), call->audio.begin() + count_samples);
        return job;
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
                if (stopping)
                    break;
                job = select();
                if (!job.call) {
                    wake.wait_for(lock, std::chrono::milliseconds(20));
                    continue;
                }
            }
            std::string text;
            std::int64_t stream_step_end = 0;
            bool did_refine = false;
            Status status = job.status;
            const auto decode_started_ns = job.call->clock->now_ns();
            const auto decode_start = std::chrono::steady_clock::now();
            struct DecodeGuard {
                PrefixShared *worker;
                Call *call;
                std::chrono::steady_clock::time_point started;
            } guard{this, job.call.get(), decode_start};
            model->offline_decode_guard_userdata = &guard;
            model->offline_decode_guard = [](void *userdata) -> int {
                auto &value = *static_cast<DecodeGuard *>(userdata);
                std::lock_guard lock(value.worker->mutex);
                return !value.call->cancelled && !value.worker->stopping &&
                       std::chrono::steady_clock::now() - value.started <
                           std::chrono::milliseconds(value.worker->decode_timeout_ms) &&
                       value.call->clock->now_ns() - value.call->created_ns <
                           std::int64_t(value.worker->total_timeout_ms) * 1000000;
            };
            try {
                if (status && !job.samples.empty()) {
                    if (streaming.enabled) {
                        auto &call = *job.call;
                        if (!call.stream)
                            call.stream.reset(qwen_resumable_create(
                                model.get(), language_name(call.config.language), step_samples_for(call) / 16,
                                streaming.max_tokens, streaming.unfixed_chunks));
                        if (!call.stream)
                            throw std::runtime_error("cannot create resumable call state");
                        const int progress =
                            qwen_resumable_step(call.stream.get(), job.samples.data(),
                                                static_cast<int>(job.samples.size()), job.final);
                        stream_step_end = call.clock->now_ns();
                        if (progress < 0)
                            throw std::runtime_error(progress == -2
                                                         ? "stream step interrupted at token boundary"
                                                         : "resumable step failed");
                        job.final = progress == 2;
                        text = qwen_resumable_text(call.stream.get());
                        const auto complete = complete_utf8_prefix(text);
                        if (job.final && complete != text.size())
                            throw std::runtime_error("incomplete UTF-8 at stream EOF");
                        text.resize(complete); // State retains unfinished bytes for the next step.
                        if (job.final && streaming.refine_final) {
                            did_refine = true;
                            if (qwen_set_force_language(model.get(), language_name(call.config.language)) !=
                                0)
                                throw std::runtime_error("invalid refinement language");
                            std::unique_ptr<char, decltype(&std::free)> refined(
                                qwen_transcribe_audio(model.get(), job.samples.data(),
                                                      static_cast<int>(job.samples.size())),
                                std::free);
                            if (!refined || model->offline_decode_aborted)
                                throw std::runtime_error("stream EOF refinement interrupted or failed");
                            text = refined.get();
                        }
                    } else {
                        if (qwen_set_force_language(model.get(), language_name(job.call->config.language)) !=
                            0)
                            throw std::runtime_error("native language selection failed");
                        std::unique_ptr<char, decltype(&std::free)> result(
                            qwen_transcribe_audio(model.get(), job.samples.data(),
                                                  static_cast<int>(job.samples.size())),
                            std::free);
                        if (!result)
                            throw std::runtime_error("native prefix decode failed");
                        text = result.get();
                        if (model->offline_decode_aborted) {
                            text.clear();
                            status = {ErrorCode::deadline_expired,
                                      "prefix decode interrupted at token boundary"};
                        }
                    }
                }
            } catch (const std::exception &error) {
                status = {ErrorCode::runtime_failure, error.what()};
            }
            model->offline_decode_guard = nullptr;
            model->offline_decode_guard_userdata = nullptr;
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
                    if (streaming.enabled && call.stream) {
                        call.decoded_samples = qwen_resumable_cursor(call.stream.get());
                        call.steps = qwen_resumable_steps(call.stream.get());
                        call.reused_prefill = qwen_resumable_reused_prefill(call.stream.get());
                        // Reinsert this call behind already waiting calls after a quantum.
                        call.step_ready_ns = (call.eof || decode_ready(call)) ? decode_finished_ns : 0;
                    }
                    const bool pre_eof = !job.final && !call.eof && !text.empty();
                    event = make_event(
                        call,
                        !status     ? EventKind::failed
                        : job.final ? EventKind::final
                                    : EventKind::partial,
                        static_cast<std::int64_t>(job.status ? job.samples.size() : call.audio.size()),
                        !status && text.empty() ? call.snapshot.text : std::move(text), status, pre_eof);
                    if (!job.final && !streaming.enabled)
                        call.preview_done = true;
                    call.snapshot.state = !status     ? SessionState::failed
                                          : job.final ? SessionState::completed
                                          : call.eof  ? SessionState::finalizing
                                                      : SessionState::streaming;
                    publish = true;
                }
            }
            if (publish) {
                try {
                    const auto observe = [&](const char *stage, std::int64_t timestamp,
                                             std::optional<std::int64_t> duration,
                                             std::optional<std::int64_t> counter = {}) {
                        RuntimeObservation value;
                        value.stage = stage;
                        value.worker_id = worker_id;
                        value.process_id = getpid();
                        value.timestamp_ns = timestamp;
                        value.duration_ns = duration;
                        value.counter_value = counter;
                        value.sequence = job.final ? 1 : 0;
                        value.buffered_samples = static_cast<std::int64_t>(job.samples.size());
                        job.call->sink->on_observation(value);
                    };
                    if (model_info)
                        observe("shared_model_load", model_loaded_ns, model_load_ns);
                    if (job.final)
                        observe("worker_eof_received", job.eof_ns, std::nullopt);
                    if (!streaming.enabled && job.status && !job.samples.empty()) {
                        observe(job.final ? "eof_decode_queue_wait" : "prefix_decode_queue_wait",
                                decode_started_ns, decode_started_ns - job.ready_ns);
                        observe(job.final ? "eof_refinement" : "prefix_decode", decode_finished_ns,
                                decode_finished_ns - decode_started_ns);
                    }
                    if (streaming.enabled && stream_step_end) {
                        observe("stream_step_queue_wait", decode_started_ns,
                                decode_started_ns - job.ready_ns);
                        observe("stream_step", stream_step_end, stream_step_end - decode_started_ns);
                        observe("stream_steps", decode_finished_ns, std::nullopt, job.call->steps);
                        observe("stream_reused_prefill_tokens", decode_finished_ns, std::nullopt,
                                job.call->reused_prefill);
                        if (did_refine)
                            observe("eof_refinement", decode_finished_ns,
                                    decode_finished_ns - stream_step_end);
                    }
                    job.call->sink->on_event(event);
                } catch (...) {
                    status = {ErrorCode::runtime_failure, "result delivery failed"};
                    terminal = true;
                }
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
        if (call_->terminal && !call_->terminal_status)
            return call_->terminal_status;
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
        if (shared_->streaming.enabled && !call_->step_ready_ns && shared_->decode_ready(*call_))
            call_->step_ready_ns = call_->last_audio_ns;
        if (!call_->preview_ready_ns &&
            call_->audio.size() >= static_cast<std::size_t>(shared_->preview_samples_for(*call_)))
            call_->preview_ready_ns = call_->last_audio_ns;
        call_->snapshot.state = SessionState::streaming;
        call_->snapshot.consumed_samples = static_cast<std::int64_t>(call_->audio.size());
        shared_->wake.notify_all();
        return {};
    }
    Status finish_input() override {
        std::unique_lock lock(shared_->mutex);
        if (call_->terminal)
            return call_->terminal_status;
        if (call_->cancelled)
            return {ErrorCode::cancelled, "call cancelled"};
        if (!call_->eof)
            call_->eof_received_ns = call_->clock->now_ns();
        call_->eof = true;
        call_->snapshot.state = SessionState::finalizing;
        shared_->wake.notify_all();
        call_->completed.wait(lock, [&] { return call_->terminal || call_->cancelled; });
        return call_->cancelled ? Status{ErrorCode::cancelled, "call cancelled"} : call_->terminal_status;
    }
    Status cancel(CancelReason reason) override {
        std::unique_lock lock(shared_->mutex);
        if (call_->busy && (call_->snapshot.state == SessionState::completed ||
                            call_->snapshot.state == SessionState::failed))
            call_->completed.wait(lock, [&] { return !call_->busy; });
        if (call_->terminal)
            return call_->terminal_status;
        call_->cancelled = true;
        shared_->wake.notify_all();
        call_->completed.wait(lock, [&] { return !call_->busy; });
        call_->snapshot.state = SessionState::stopped;
        Status status{reason == CancelReason::deadline ? ErrorCode::deadline_expired : ErrorCode::cancelled,
                      "experimental prefix call cancelled"};
        auto event = make_event(*call_, EventKind::stopped, static_cast<std::int64_t>(call_->audio.size()),
                                call_->snapshot.text, status, false);
        call_->terminal = true;
        call_->terminal_status = status;
        lock.unlock();
        try {
            call_->sink->on_event(event);
        } catch (...) {
            return {ErrorCode::runtime_failure, "result delivery failed"};
        }
        return {};
    }
    SessionSnapshot snapshot() const override {
        std::lock_guard lock(shared_->mutex);
        return call_->snapshot;
    }
};
} // namespace

PrefixMultiplexEngine::PrefixMultiplexEngine(std::string directory, int max_calls, int preview_ms,
                                             int threads, int idle_ms, int total_ms, int decode_ms,
                                             std::string worker_id, SharedStreamOptions streaming)
    : shared_(std::make_shared<PrefixShared>(directory, max_calls, preview_ms, threads, idle_ms, total_ms,
                                             decode_ms, std::move(worker_id), streaming)) {}
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
        status.calls.push_back({call->config.call_id, call->config.language, call->snapshot.state,
                                static_cast<std::int64_t>(call->audio.size()), call->busy});
        if (!call->busy && !call->cancelled && !call->terminal && (call->eof || shared_->decode_ready(*call)))
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
    value.engine_id =
        shared_->streaming.enabled ? "qwen_stream_shared" : "qwen_prefix_multiplex_experimental";
    value.revision = shared_->streaming.enabled ? "resumable_v1_step_fair" : "prefix_v2_preview_fair";
    value.streaming_kind =
        shared_->streaming.enabled ? "resumable_windowed_streaming" : "causal_prefix_redecode";
    value.is_mock = false;
    value.device = "cpu";
    value.precision = "bf16_weights";
    value.cooperative_cancellation = true;
    value.concurrent_sessions = true;
    value.transcript_semantics = "revisable_full_snapshot";
    return value;
}

Result<std::unique_ptr<IASRSession>>
PrefixMultiplexEngine::create_session(const SessionConfig &config, IRecognitionSink &sink, IClock &clock) {
    if (!language_name(config.language) || config.sample_rate_hz != 16000 || clock.domain() != "host_steady")
        return {{ErrorCode::unsupported, "experimental engine supports en/id/zh at 16 kHz with host clock"},
                nullptr};
    if (config.run_id.empty() || config.call_id.empty() || config.max_chunk_samples < 1 ||
        config.max_chunk_samples > 16000)
        return {{ErrorCode::invalid_input, "invalid experimental session configuration"}, nullptr};
    if (config.prefix_preview_ms != 0 &&
        (config.prefix_preview_ms < 1000 || config.prefix_preview_ms > 20000))
        return {{ErrorCode::invalid_input, "prefix preview must be 1000..20000 ms"}, nullptr};
    if (config.decode_step_ms != 0 && (config.decode_step_ms < 1000 || config.decode_step_ms > 8000))
        return {{ErrorCode::invalid_input, "decode step must be 1000..8000 ms"}, nullptr};
    std::lock_guard lock(shared_->mutex);
    if (shared_->draining)
        return {{ErrorCode::invalid_state, "shared worker is draining"}, nullptr};
    if (shared_->calls.size() >= static_cast<std::size_t>(shared_->max_calls))
        return {{ErrorCode::resource_exhausted, "all shared-model call slots occupied"}, nullptr};
    for (const auto &existing : shared_->calls)
        if (existing->config.call_id == config.call_id)
            return {{ErrorCode::invalid_input, "duplicate active call ID"}, nullptr};
    auto call = std::make_shared<Call>(config, sink, clock, shared_->worker_id);
    shared_->calls.push_back(call);
    shared_->wake.notify_all();
    return {{}, std::make_unique<PrefixSession>(shared_, std::move(call))};
}
} // namespace asr
