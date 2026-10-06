#include "wire.hpp"
#include <algorithm>
#include <asr/engines/native_engine.hpp>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
namespace asr {
namespace {
using Json = nlohmann::json;
std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
const char *native_language(const std::string &value) {
    if (value == "en")
        return "English";
    if (value == "id")
        return "Indonesian";
    if (value == "zh")
        return "Chinese";
    return nullptr;
}
void close_fd(int &fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}
// Keep SIGPIPE handling local to this sending thread; an exited worker yields EPIPE.
ssize_t pipe_write(int fd, const void *data, std::size_t count) {
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    const auto mask_error = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (mask_error) {
        errno = mask_error;
        return -1;
    }
    sigpending(&pending);
    const bool was_pending = sigismember(&pending, SIGPIPE) == 1;
    const auto result = ::write(fd, data, count);
    const auto write_error = errno;
    if (result < 0 && write_error == EPIPE && !was_pending) {
        const timespec no_wait{0, 0};
        while (sigtimedwait(&blocked, nullptr, &no_wait) < 0 && errno == EINTR) {
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    errno = write_error;
    return result;
}
class NativeSession final : public IASRSession {
    NativeQwenOptions options_;
    SessionConfig config_;
    IRecognitionSink &sink_;
    IClock &clock_;
    SessionSnapshot state_;
    pid_t child_ = -1;
    int input_ = -1, output_ = -1;
    std::string incoming_;
    std::uint64_t sequence_ = 0, event_sequence_ = 0;
    std::int64_t deadline_ = 0;
    std::optional<Json> pending_final_;
    Status last_error_;
    bool eof_sent_ = false;
    bool ready_seen_ = false;
    void terminate() {
        close_fd(input_);
        close_fd(output_);
        if (child_ > 0) {
            ::kill(child_, SIGKILL);
            int status = 0;
            while (::waitpid(child_, &status, 0) < 0 && errno == EINTR) {
            }
            child_ = -1;
        }
    }
    void emit(EventKind kind, Status status = {}, std::int64_t produced = -1, std::int64_t consumed = -1,
              bool before_eof = false) {
        RecognitionEvent event;
        event.run_id = config_.run_id;
        event.call_id = config_.call_id;
        event.producer_id = "qwen_native";
        event.sequence = event_sequence_++;
        event.revision = ++state_.revision;
        event.produced_ns = produced < 0 ? clock_.now_ns() : produced;
        event.clock_domain = "host_steady";
        event.utc = clock_.utc_now();
        event.consumed_samples = consumed < 0 ? state_.consumed_samples : consumed;
        event.kind = kind;
        event.text = state_.text;
        event.status = std::move(status);
        event.before_eof = before_eof;
        sink_.on_event(event);
    }
    Status fail(ErrorCode code, std::string message) {
        if (state_.state == SessionState::failed)
            return last_error_;
        terminate();
        state_.state = SessionState::failed;
        last_error_ = {code, std::move(message)};
        emit(EventKind::failed, last_error_);
        return last_error_;
    }
    void handle_line(const std::string &line) {
        auto record = Json::parse(line);
        const auto type = record.at("type").get<std::string>();
        if (type == "partial") {
            if (state_.state == SessionState::failed || pending_final_)
                return;
            state_.text = record.at("text").get<std::string>();
            emit(EventKind::partial, {}, record.at("produced_ns").get<std::int64_t>(),
                 record.at("consumed_samples").get<std::int64_t>(), record.value("before_eof", false));
        } else if (type == "timing") {
            RuntimeObservation observation;
            observation.stage = record.at("stage").get<std::string>();
            observation.timestamp_ns = record.at("timestamp_ns").get<std::int64_t>();
            observation.process_id = record.at("process_id").get<int>();
            if (record.contains("duration_ns"))
                observation.duration_ns = record["duration_ns"].get<std::int64_t>();
            if (record.contains("sequence"))
                observation.sequence = record["sequence"].get<std::int64_t>();
            if (record.contains("buffered_samples"))
                observation.buffered_samples = record["buffered_samples"].get<std::int64_t>();
            if (record.contains("cpu_ns"))
                observation.cpu_ns = record["cpu_ns"].get<std::int64_t>();
            if (record.contains("peak_rss_bytes"))
                observation.peak_rss_bytes = record["peak_rss_bytes"].get<std::int64_t>();
            sink_.on_observation(observation);
        } else if (type == "final")
            pending_final_ = std::move(record);
        else if (type == "failed")
            last_error_ = {ErrorCode::runtime_failure, record.value("message", "worker failed")};
        else if (type == "ready")
            ready_seen_ = true;
        else
            throw std::runtime_error("unknown native worker event");
    }
    bool read_available(int timeout_ms) {
        pollfd descriptor{output_, POLLIN | POLLHUP, 0};
        int outcome;
        do {
            outcome = ::poll(&descriptor, 1, timeout_ms);
        } while (outcome < 0 && errno == EINTR);
        if (outcome < 0)
            throw std::runtime_error("native worker poll failed");
        if (outcome == 0)
            return false;
        char buffer[8192];
        const auto count = ::read(output_, buffer, sizeof(buffer));
        if (count < 0 && (errno == EAGAIN || errno == EINTR))
            return false;
        if (count < 0)
            throw std::runtime_error("native worker output read failed");
        if (count == 0)
            return false;
        incoming_.append(buffer, count);
        if (incoming_.size() > 1024 * 1024)
            throw std::runtime_error("native worker line exceeds 1 MiB");
        std::size_t end;
        while ((end = incoming_.find('\n')) != std::string::npos) {
            auto line = incoming_.substr(0, end);
            incoming_.erase(0, end + 1);
            handle_line(line);
        }
        return true;
    }
    void drain() {
        while (read_available(0)) {
        }
    }
    bool write_bytes(const void *pointer, std::size_t count) {
        auto *data = static_cast<const char *>(pointer);
        const auto operation_deadline = std::min<std::int64_t>(deadline_, steady_ns() + 1000000000LL);
        while (count) {
            const auto sent = pipe_write(input_, data, count);
            if (sent > 0) {
                data += sent;
                count -= sent;
                continue;
            }
            if (sent < 0 && errno == EINTR)
                continue;
            if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                return false;
            drain();
            const auto left_ms = (operation_deadline - steady_ns()) / 1000000;
            if (left_ms <= 0)
                return false;
            pollfd descriptor{input_, POLLOUT, 0};
            if (::poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(left_ms, 20))) < 0 &&
                errno != EINTR)
                return false;
        }
        return true;
    }
    void spawn() {
        int input_pipe[2]{-1, -1}, output_pipe[2]{-1, -1};
        if (::pipe2(input_pipe, O_CLOEXEC) != 0 || ::pipe2(output_pipe, O_CLOEXEC) != 0) {
            close_fd(input_pipe[0]);
            close_fd(input_pipe[1]);
            close_fd(output_pipe[0]);
            close_fd(output_pipe[1]);
            throw std::runtime_error("native worker IPC creation failed");
        }
        // If the read end already is fd 3, dup2(3,3) cannot clear CLOEXEC.
        if (::fcntl(input_pipe[0], F_SETFD, 0) != 0) {
            close_fd(input_pipe[0]);
            close_fd(input_pipe[1]);
            close_fd(output_pipe[0]);
            close_fd(output_pipe[1]);
            throw std::runtime_error("cannot expose worker input descriptor");
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addclose(&actions, input_pipe[1]);
        posix_spawn_file_actions_adddup2(&actions, input_pipe[0], 3);
        if (input_pipe[0] != 3)
            posix_spawn_file_actions_addclose(&actions, input_pipe[0]);
        posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
        posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
        const auto worker = std::filesystem::absolute(options_.worker_executable).string();
        const auto model = std::filesystem::absolute(options_.model_directory).string();
        const auto threads = std::to_string(options_.threads);
        const auto step = std::to_string(options_.decode_step_ms);
        const auto tokens = std::to_string(options_.max_new_tokens);
        const auto refine = options_.refine_final ? std::string("1") : std::string("0");
        std::string staging = "20";
        const auto parent_pid = std::to_string(::getpid());
        std::vector<char *> args{const_cast<char *>(worker.c_str()),
                                 const_cast<char *>(model.c_str()),
                                 const_cast<char *>(native_language(config_.language)),
                                 const_cast<char *>(threads.c_str()),
                                 const_cast<char *>(step.c_str()),
                                 const_cast<char *>(tokens.c_str()),
                                 const_cast<char *>(refine.c_str()),
                                 staging.data(),
                                 const_cast<char *>(parent_pid.c_str()),
                                 nullptr};
        // OpenMP/BLAS may initialize before main(); set their limits at exec,
        // without modifying the controller's environment while its sampler runs.
        std::vector<std::string> environment;
        for (auto **entry = environ; *entry; ++entry) {
            const std::string value(*entry);
            if (!value.starts_with("OMP_NUM_THREADS=") && !value.starts_with("OPENBLAS_NUM_THREADS=") &&
                !value.starts_with("OMP_DYNAMIC=") && !value.starts_with("QWEN_BF16_CACHE_MB=") &&
                !value.starts_with("ASR_WORKER_CPU_CORES="))
                environment.push_back(value);
        }
        environment.push_back("OMP_NUM_THREADS=" + threads);
        environment.push_back("OPENBLAS_NUM_THREADS=" + threads);
        environment.push_back("OMP_DYNAMIC=FALSE");
        environment.push_back("QWEN_BF16_CACHE_MB=0");
        if (!options_.cpu_cores.empty()) {
            std::string cores;
            for (const auto core : options_.cpu_cores) {
                if (!cores.empty())
                    cores += ',';
                cores += std::to_string(core);
            }
            environment.push_back("ASR_WORKER_CPU_CORES=" + cores);
        }
        std::vector<char *> environment_pointers;
        for (auto &entry : environment)
            environment_pointers.push_back(entry.data());
        environment_pointers.push_back(nullptr);
        const auto result =
            posix_spawn(&child_, worker.c_str(), &actions, nullptr, args.data(), environment_pointers.data());
        posix_spawn_file_actions_destroy(&actions);
        close_fd(input_pipe[0]);
        close_fd(output_pipe[1]);
        if (result != 0) {
            close_fd(input_pipe[1]);
            close_fd(output_pipe[0]);
            throw std::runtime_error("native worker spawn failed: " + std::string(std::strerror(result)));
        }
        input_ = input_pipe[1];
        output_ = output_pipe[0];
        if (::fcntl(input_, F_SETFL, ::fcntl(input_, F_GETFL) | O_NONBLOCK) != 0) {
            terminate();
            throw std::runtime_error("cannot set native IPC writer nonblocking");
        }
        try {
            RuntimeObservation started;
            started.stage = "worker_started";
            started.timestamp_ns = clock_.now_ns();
            started.process_id = child_;
            sink_.on_observation(started);
            const auto startup_deadline = steady_ns() + 30000000000LL;
            while (steady_ns() < startup_deadline) {
                if (read_available(100)) {
                    if (ready_seen_ && last_error_.code == ErrorCode::none)
                        return;
                    if (last_error_.code != ErrorCode::none)
                        break;
                }
                int status = 0;
                if (::waitpid(child_, &status, WNOHANG) == child_) {
                    child_ = -1;
                    break;
                }
            }
        } catch (...) {
            terminate();
            throw;
        }
        terminate();
        throw std::runtime_error(last_error_.message.empty() ? "native worker startup failed or timed out"
                                                             : last_error_.message);
    }

  public:
    NativeSession(NativeQwenOptions options, SessionConfig config, IRecognitionSink &sink, IClock &clock)
        : options_(std::move(options)), config_(std::move(config)), sink_(sink), clock_(clock) {
        spawn();
        deadline_ = steady_ns() + std::int64_t(options_.timeout_ms) * 1000000;
    }
    ~NativeSession() override { terminate(); }
    Status submit(AudioChunk chunk) override {
        if (state_.state == SessionState::completed || state_.state == SessionState::stopped ||
            state_.state == SessionState::failed || eof_sent_)
            return {ErrorCode::invalid_state, "audio after terminal state or EOF"};
        if (chunk.run_id != config_.run_id || chunk.call_id != config_.call_id ||
            chunk.sequence != sequence_ || chunk.first_sample != state_.consumed_samples ||
            chunk.sample_rate_hz != 16000 || chunk.channels != 1 || !chunk.pcm || chunk.pcm->empty())
            return {ErrorCode::invalid_input, "invalid native audio identity, order, or format"};
        if (chunk.pcm->size() > static_cast<std::size_t>(config_.max_chunk_samples) ||
            state_.consumed_samples + chunk.pcm->size() > 16000 * 600)
            return {ErrorCode::resource_exhausted, "native audio limit exceeded"};
        if (steady_ns() > deadline_)
            return fail(ErrorCode::deadline_expired, "native call watchdog expired");
        try {
            drain();
            if (last_error_.code != ErrorCode::none)
                return fail(last_error_.code, last_error_.message);
            native_wire::Header header;
            header.type = native_wire::audio;
            header.sequence = sequence_;
            header.first_sample = state_.consumed_samples;
            header.sample_count = chunk.pcm->size();
            if (!write_bytes(&header, sizeof(header)) ||
                !write_bytes(chunk.pcm->data(), chunk.pcm->size() * sizeof(std::int16_t))) {
                const auto write_error = errno;
                drain();
                return fail(ErrorCode::runtime_failure, last_error_.message.empty()
                                                            ? "native worker input write failed: " +
                                                                  std::string(std::strerror(write_error))
                                                            : last_error_.message);
            }
            ++sequence_;
            state_.consumed_samples += chunk.pcm->size();
            state_.state = SessionState::streaming;
            drain();
            if (last_error_.code != ErrorCode::none)
                return fail(last_error_.code, last_error_.message);
            return {};
        } catch (const std::exception &error) {
            return fail(ErrorCode::runtime_failure, error.what());
        }
    }
    Status finish_input() override {
        if (state_.state == SessionState::completed)
            return {};
        if (state_.state == SessionState::stopped || state_.state == SessionState::failed)
            return {ErrorCode::invalid_state, "EOF on stopped or failed native session"};
        if (!eof_sent_) {
            native_wire::Header header;
            header.type = native_wire::eof;
            header.sequence = sequence_;
            header.first_sample = state_.consumed_samples;
            if (!write_bytes(&header, sizeof(header)))
                return fail(ErrorCode::runtime_failure, "native worker EOF write failed");
            eof_sent_ = true;
        }
        try {
            while (steady_ns() < deadline_) {
                (void)read_available(50);
                if (last_error_.code != ErrorCode::none)
                    return fail(last_error_.code, last_error_.message);
                int status = 0;
                if (::waitpid(child_, &status, WNOHANG) == child_) {
                    child_ = -1;
                    drain();
                    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !pending_final_)
                        return fail(ErrorCode::runtime_failure,
                                    "native worker exited without successful final");
                    if (pending_final_->at("consumed_samples").get<std::int64_t>() != state_.consumed_samples)
                        return fail(ErrorCode::runtime_failure, "native final sample watermark mismatch");
                    state_.text = pending_final_->at("text").get<std::string>();
                    state_.state = SessionState::completed;
                    emit(EventKind::final, {}, pending_final_->at("produced_ns").get<std::int64_t>());
                    close_fd(input_);
                    close_fd(output_);
                    return {};
                }
            }
            return fail(ErrorCode::deadline_expired, "native call watchdog expired at EOF");
        } catch (const std::exception &error) {
            return fail(ErrorCode::runtime_failure, error.what());
        }
    }
    Status cancel(CancelReason reason) override {
        if (state_.state == SessionState::completed || state_.state == SessionState::stopped)
            return {};
        if (state_.state == SessionState::failed)
            return last_error_;
        terminate();
        state_.state = SessionState::stopped;
        emit(EventKind::stopped,
             {reason == CancelReason::deadline ? ErrorCode::deadline_expired : ErrorCode::cancelled,
              "native worker terminated by cancellation"});
        return {};
    }
    SessionSnapshot snapshot() const override { return state_; }
};
} // namespace
NativeQwenEngine::NativeQwenEngine(NativeQwenOptions options) : options_(std::move(options)) {
    if (options_.threads < 1 || options_.threads > 16 || options_.decode_step_ms < 1000 ||
        options_.decode_step_ms > 8000 || options_.max_new_tokens < 1 || options_.max_new_tokens > 256 ||
        options_.timeout_ms < 1000 || options_.timeout_ms > 600000)
        throw std::invalid_argument("unsupported native model controls");
}
EngineCapabilities NativeQwenEngine::capabilities() const {
    EngineCapabilities result;
    result.engine_id = "qwen_native";
    result.revision = "924694251d9e0f18e5d86bbd06aa3ab5f870002d";
    result.streaming_kind = "windowed_redecode_live";
    result.is_mock = false;
    result.device = "cpu";
    result.precision = "bf16_weights_cpu";
    result.cooperative_cancellation = false;
    result.concurrent_sessions = false; // capacity is not yet qualified.
    return result;
}
Result<std::unique_ptr<IASRSession>> NativeQwenEngine::create_session(const SessionConfig &config,
                                                                      IRecognitionSink &sink, IClock &clock) {
    if (!native_language(config.language) || config.sample_rate_hz != 16000)
        return {{ErrorCode::unsupported, "native supports tested en/id/zh at 16 kHz"}, nullptr};
    if (clock.domain() != "host_steady")
        return {{ErrorCode::unsupported, "native requires host monotonic clock"}, nullptr};
    if (config.run_id.empty() || config.call_id.empty() || config.max_chunk_samples < 1 ||
        config.max_chunk_samples > 16000)
        return {{ErrorCode::invalid_input, "invalid native session settings"}, nullptr};
    if (!std::filesystem::is_regular_file(options_.worker_executable) ||
        !std::filesystem::is_regular_file(options_.model_directory / "model.safetensors") ||
        !std::filesystem::is_regular_file(options_.model_directory / "acquisition.json"))
        return {{ErrorCode::invalid_input, "missing native worker or verified model files"}, nullptr};
    try {
        std::ifstream manifest(options_.model_directory / "acquisition.json");
        const auto acquisition = Json::parse(manifest);
        if (acquisition.at("status") != "verified" ||
            acquisition.at("revision") != "5eb144179a02acc5e5ba31e748d22b0cf3e303b0")
            return {{ErrorCode::unsupported, "model acquisition is not the verified pinned revision"},
                    nullptr};
        auto options = options_;
        if (config.decode_step_ms) {
            if (config.decode_step_ms < 1000 || config.decode_step_ms > 8000)
                return {{ErrorCode::invalid_input, "decode step must be 1000..8000 ms"}, nullptr};
            options.decode_step_ms = config.decode_step_ms;
        }
        return {{}, std::make_unique<NativeSession>(options, config, sink, clock)};
    } catch (const std::exception &error) {
        return {{ErrorCode::runtime_failure, error.what()}, nullptr};
    }
}
} // namespace asr
