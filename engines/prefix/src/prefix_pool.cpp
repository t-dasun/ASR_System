#include "process_wire.hpp"
#include <asr/engines/prefix_pool.hpp>
#include <atomic>
#include <condition_variable>
#include <fcntl.h>
#include <map>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
extern char **environ;
namespace asr {
using prefix_wire::Json;
namespace {
struct RemoteCall {
    std::mutex mutex;
    IRecognitionSink *sink = nullptr;
    SessionConfig config;
    SessionSnapshot snapshot;
    bool terminal = false;
    Status result;
    std::uint64_t sequence = 0;
};
} // namespace
struct PrefixProcess {
    std::string id;
    int pid = -1, fd = -1, slots = 0, total_ms = 0;
    std::atomic<bool> healthy{true};
    std::mutex mutex, send_mutex;
    std::condition_variable changed;
    std::map<std::string, std::shared_ptr<RemoteCall>> calls;
    std::map<std::uint64_t, Json> replies;
    std::uint64_t request = 0;
    bool boot = false;
    std::string error;
    std::thread reader;
    PrefixProcess(const std::filesystem::path &exe, std::string model, int index, int limit, int preview,
                  int threads, int idle, int total, int decode)
        : id("prefix_shared_" + std::to_string(index)), slots(limit), total_ms(total + decode + 10000) {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets))
            throw std::runtime_error("prefix socketpair failed");
        // Keep both source descriptors above child protocol FD 3.
        int child_fd = fcntl(sockets[1], F_DUPFD_CLOEXEC, 4);
        close(sockets[1]);
        if (child_fd < 0) {
            close(sockets[0]);
            throw std::runtime_error("prefix descriptor duplication failed");
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addclose(&actions, sockets[0]);
        posix_spawn_file_actions_adddup2(&actions, child_fd, 3);
        posix_spawn_file_actions_addclose(&actions, child_fd);
        std::vector<std::string> args = {exe.string(),
                                         model,
                                         std::to_string(limit),
                                         std::to_string(preview),
                                         std::to_string(threads),
                                         std::to_string(idle),
                                         std::to_string(total),
                                         std::to_string(decode),
                                         id};
        std::vector<char *> argv;
        for (auto &a : args)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        pid_t child = -1;
        auto rc = posix_spawn(&child, exe.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(child_fd);
        if (rc) {
            close(sockets[0]);
            throw std::runtime_error("cannot spawn prefix worker: " + std::to_string(rc));
        }
        pid = child;
        fd = sockets[0];
        timeval timeout{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        reader = std::thread([this] { read_loop(); });
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(120), [&] { return boot || !healthy; }) || !boot) {
            lock.unlock();
            stop();
            throw std::runtime_error("prefix worker model load failed");
        }
    }
    ~PrefixProcess() { stop(); }
    void stop() {
        if (pid > 0) {
            kill(pid, SIGKILL);
            shutdown(fd, SHUT_RDWR);
            if (reader.joinable())
                reader.join();
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            pid = -1;
        }
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
    void fault(const std::string &message) {
        std::vector<std::shared_ptr<RemoteCall>> active;
        {
            std::lock_guard lock(mutex);
            if (!healthy.exchange(false))
                return;
            error = message;
            for (auto &[key, c] : calls)
                active.push_back(c);
        }
        if (pid > 0)
            kill(pid, SIGKILL);
        shutdown(fd, SHUT_RDWR);
        changed.notify_all();
        for (auto &c : active) {
            std::lock_guard lock(c->mutex);
            if (c->terminal)
                continue;
            c->terminal = true;
            c->result = {ErrorCode::runtime_failure, message};
            c->snapshot.state = SessionState::failed;
            if (c->sink) {
                RecognitionEvent e;
                e.call_id = c->config.call_id;
                e.run_id = c->config.run_id;
                e.worker_id = id;
                e.producer_id = "qwen_prefix_pool";
                e.sequence = c->sequence++;
                e.revision = ++c->snapshot.revision;
                e.kind = EventKind::failed;
                e.status = c->result;
                e.text = c->snapshot.text;
                SteadyClock clock;
                e.produced_ns = clock.now_ns();
                e.clock_domain = clock.domain();
                e.utc = clock.utc_now();
                e.consumed_samples = c->snapshot.consumed_samples;
                try {
                    c->sink->on_event(e);
                } catch (...) {
                }
            }
        }
    }
    void read_loop() {
        try {
            Json j;
            while (prefix_wire::receive(fd, j)) {
                auto type = j.at("type").get<std::string>();
                if (type == "boot" || type == "reply") {
                    {
                        std::lock_guard lock(mutex);
                        if (type == "boot")
                            boot = true;
                        else
                            replies.emplace(j.at("request").get<std::uint64_t>(), j);
                    }
                    changed.notify_all();
                    continue;
                }
                std::shared_ptr<RemoteCall> call;
                {
                    std::lock_guard lock(mutex);
                    auto it = calls.find(j.at("call_id").get<std::string>());
                    if (it != calls.end())
                        call = it->second;
                }
                if (!call)
                    continue;
                std::lock_guard lock(call->mutex);
                if (!call->sink)
                    continue;
                if (type == "event") {
                    auto e = prefix_wire::event(j);
                    call->sequence = e.sequence + 1;
                    call->snapshot.text = e.text;
                    call->snapshot.revision = e.revision;
                    call->snapshot.consumed_samples = e.consumed_samples;
                    call->snapshot.state = e.kind == EventKind::final     ? SessionState::completed
                                           : e.kind == EventKind::failed  ? SessionState::failed
                                           : e.kind == EventKind::stopped ? SessionState::stopped
                                                                          : SessionState::streaming;
                    if (e.kind != EventKind::partial) {
                        call->terminal = true;
                        call->result = e.status;
                    }
                    call->sink->on_event(e);
                } else if (type == "observation")
                    call->sink->on_observation(prefix_wire::observation(j));
                else
                    throw std::runtime_error("unknown prefix IPC event");
            }
            fault("prefix worker disconnected");
        } catch (const std::exception &e) {
            fault(e.what());
        }
    }
    Json rpc(Json j, int timeout_ms = 30000) {
        std::uint64_t key;
        {
            std::lock_guard lock(mutex);
            if (!healthy)
                return {{"status", prefix_wire::status(Status{ErrorCode::runtime_failure, error})}};
            key = ++request;
        }
        j["request"] = key;
        bool sent;
        {
            std::lock_guard lock(send_mutex);
            sent = prefix_wire::send(fd, j);
        }
        if (!sent) {
            shutdown(fd, SHUT_RDWR);
            fault("prefix IPC send failed");
        }
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [&] { return replies.contains(key) || !healthy; })) {
            lock.unlock();
            kill(pid, SIGKILL);
            shutdown(fd, SHUT_RDWR);
            fault("prefix IPC response deadline expired");
            return {{"status", prefix_wire::status(Status{ErrorCode::deadline_expired,
                                                          "prefix IPC response deadline expired"})}};
        }
        auto it = replies.find(key);
        if (it == replies.end())
            return {{"status", prefix_wire::status(Status{ErrorCode::runtime_failure, error})}};
        auto result = std::move(it->second);
        replies.erase(it);
        return result;
    }
};
namespace {
class RemoteSession final : public IASRSession {
    std::shared_ptr<PrefixProcess> worker_;
    std::shared_ptr<RemoteCall> call_;

  public:
    RemoteSession(std::shared_ptr<PrefixProcess> w, std::shared_ptr<RemoteCall> c)
        : worker_(std::move(w)), call_(std::move(c)) {}
    ~RemoteSession() override {
        try {
            cancel(CancelReason::shutdown);
            worker_->rpc({{"op", "drop"}, {"call_id", call_->config.call_id}}, worker_->total_ms);
        } catch (...) {
        }
        {
            std::lock_guard lock(worker_->mutex);
            worker_->calls.erase(call_->config.call_id);
        }
        std::lock_guard lock(call_->mutex);
        call_->sink = nullptr;
    }
    Status submit(AudioChunk c) override {
        if (c.call_id != call_->config.call_id || c.run_id != call_->config.run_id || !c.pcm ||
            c.pcm->empty() || c.pcm->size() > static_cast<std::size_t>(call_->config.max_chunk_samples))
            return {ErrorCode::invalid_input, "invalid prefix IPC PCM size"};
        auto r = worker_->rpc({{"op", "audio"},
                               {"call_id", c.call_id},
                               {"run_id", c.run_id},
                               {"sequence", c.sequence},
                               {"first_sample", c.first_sample},
                               {"sample_rate_hz", c.sample_rate_hz},
                               {"channels", c.channels},
                               {"pcm", *c.pcm}});
        auto s = prefix_wire::status(r.at("status"));
        if (s) {
            std::lock_guard lock(call_->mutex);
            if (!call_->terminal) {
                call_->snapshot.state = SessionState::streaming;
                call_->snapshot.consumed_samples = c.first_sample + c.pcm->size();
            }
        }
        return s;
    }
    Status finish_input() override {
        {
            std::lock_guard lock(call_->mutex);
            if (call_->terminal)
                return call_->result;
            call_->snapshot.state = SessionState::finalizing;
        }
        return prefix_wire::status(
            worker_->rpc({{"op", "eof"}, {"call_id", call_->config.call_id}}, worker_->total_ms)
                .at("status"));
    }
    Status cancel(CancelReason reason) override {
        {
            std::lock_guard lock(call_->mutex);
            if (call_->terminal)
                return {};
        }
        return prefix_wire::status(
            worker_
                ->rpc({{"op", "cancel"}, {"call_id", call_->config.call_id}, {"reason", int(reason)}},
                      worker_->total_ms)
                .at("status"));
    }
    SessionSnapshot snapshot() const override {
        std::lock_guard lock(call_->mutex);
        return call_->snapshot;
    }
};
} // namespace
PrefixProcessPool::PrefixProcessPool(const std::filesystem::path &exe, std::string model, int workers,
                                     int slots, int preview, int threads, int idle, int total, int decode,
                                     std::string scheduler)
    : scheduler_(std::move(scheduler)) {
    if (workers < 1 || workers > 4 || slots < 1 || slots > 8 ||
        (scheduler_ != "least_active" && scheduler_ != "round_robin"))
        throw std::invalid_argument("invalid prefix pool layout");
    for (int i = 0; i < workers; ++i)
        workers_.push_back(
            std::make_shared<PrefixProcess>(exe, model, i, slots, preview, threads, idle, total, decode));
}
PrefixProcessPool::~PrefixProcessPool() = default;
EngineCapabilities PrefixProcessPool::capabilities() const {
    EngineCapabilities c;
    c.engine_id = "qwen_prefix_process_pool";
    c.revision = "prefix_pool_v1";
    c.streaming_kind = "causal_prefix_redecode";
    c.is_mock = false;
    c.precision = "bf16_weights";
    c.concurrent_sessions = true;
    c.cooperative_cancellation = true;
    c.transcript_semantics = "revisable_full_snapshot";
    return c;
}
void PrefixProcessPool::begin_draining() {
    std::lock_guard lock(mutex_);
    draining_ = true;
    for (auto &w : workers_)
        w->rpc({{"op", "drain"}});
}
std::vector<PrefixPoolWorkerStatus> PrefixProcessPool::workers() {
    std::vector<PrefixPoolWorkerStatus> result;
    for (auto &w : workers_) {
        auto r = w->rpc({{"op", "status"}});
        PrefixPoolWorkerStatus s;
        s.worker_id = w->id;
        s.process_id = w->pid;
        s.healthy = w->healthy;
        if (prefix_wire::status(r.at("status")))
            s.status = prefix_wire::worker_status(r.at("value"));
        else {
            s.status.max_calls = w->slots;
            s.status.last_error = prefix_wire::status(r.at("status")).message;
            s.status.failures = 1;
        }
        result.push_back(std::move(s));
    }
    return result;
}
Result<std::unique_ptr<IASRSession>>
PrefixProcessPool::create_session(const SessionConfig &config, IRecognitionSink &sink, IClock &clock) {
    if (config.call_id.empty() || config.run_id.empty() || config.max_chunk_samples < 1 ||
        config.max_chunk_samples > 16000)
        return {{ErrorCode::invalid_input, "invalid prefix pool session identity or chunk limit"}, nullptr};
    if (config.sample_rate_hz != 16000 ||
        (config.language != "en" && config.language != "id" && config.language != "zh"))
        return {{ErrorCode::unsupported, "prefix pool supports en/id/zh at 16kHz"}, nullptr};
    if (clock.domain() != "host_steady")
        return {{ErrorCode::unsupported, "prefix pool requires host steady clock"}, nullptr};
    std::unique_lock pool_lock(mutex_);
    if (draining_)
        return {{ErrorCode::invalid_state, "prefix pool draining"}, nullptr};
    std::shared_ptr<PrefixProcess> selected;
    std::size_t best = SIZE_MAX, chosen = 0;
    for (std::size_t n = 0; n < workers_.size(); ++n) {
        auto index = (tie_break_ + n) % workers_.size();
        auto &w = workers_[index];
        std::lock_guard lock(w->mutex);
        if (w->calls.contains(config.call_id))
            return {{ErrorCode::invalid_input, "duplicate active call ID"}, nullptr};
        if (w->healthy && w->calls.size() < static_cast<std::size_t>(w->slots)) {
            auto score = scheduler_ == "least_active" ? w->calls.size() : n;
            if (score < best) {
                best = score;
                selected = w;
                chosen = index;
            }
        }
    }
    if (!selected)
        return {{ErrorCode::resource_exhausted, "all healthy prefix worker slots occupied"}, nullptr};
    auto call = std::make_shared<RemoteCall>();
    call->config = config;
    call->sink = &sink;
    call->snapshot.worker_id = selected->id;
    {
        std::lock_guard lock(selected->mutex);
        selected->calls.emplace(config.call_id, call);
    }
    tie_break_ = (chosen + 1) % workers_.size();
    // Reservation is visible before IPC; release pool lock so unrelated admissions proceed.
    pool_lock.unlock();
    auto response = selected->rpc({{"op", "create"},
                                   {"call_id", config.call_id},
                                   {"run_id", config.run_id},
                                   {"language", config.language},
                                   {"max_chunk_samples", config.max_chunk_samples},
                                   {"sample_rate_hz", config.sample_rate_hz}});
    auto status = prefix_wire::status(response.at("status"));
    if (!status) {
        std::lock_guard lock(selected->mutex);
        selected->calls.erase(config.call_id);
        return {status, nullptr};
    }
    return {{}, std::make_unique<RemoteSession>(selected, call)};
}
} // namespace asr
