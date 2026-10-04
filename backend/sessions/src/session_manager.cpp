#include <algorithm>
#include <asr/backend/session_manager.hpp>
#include <condition_variable>
#include <stdexcept>
#include <thread>
#include <utility>

namespace asr {
namespace {
bool terminal(SessionState state) {
    return state == SessionState::completed || state == SessionState::stopped ||
           state == SessionState::failed;
}
} // namespace
class SessionManager::ManagedSession final : public IASRSession {
    class ForwardSink final : public IRecognitionSink {
        IRecognitionSink &downstream_;
        std::shared_ptr<Pool> pool_;
        std::size_t slot_;
        std::string worker_id_;
        std::uint64_t finals_ = 0;

      public:
        ForwardSink(IRecognitionSink &downstream, std::shared_ptr<Pool> pool, std::size_t slot,
                    std::string worker_id)
            : downstream_(downstream), pool_(std::move(pool)), slot_(slot), worker_id_(std::move(worker_id)) {
        }
        void on_event(const RecognitionEvent &original) override {
            auto event = original;
            event.worker_id = worker_id_;
            {
                std::lock_guard lock(pool_->mutex);
                auto &worker = pool_->workers.at(slot_);
                if (worker.worker_id != worker_id_)
                    throw std::logic_error("stale worker callback");
                if (event.kind == EventKind::final) {
                    ++finals_;
                    worker.state = SessionState::completed;
                } else if (event.kind == EventKind::failed) {
                    worker.state = SessionState::failed;
                    worker.healthy = false;
                    worker.last_error = event.status.message;
                    ++worker.failures;
                } else if (event.kind == EventKind::stopped)
                    worker.state = SessionState::stopped;
                else if (worker.state == SessionState::ready)
                    worker.state = SessionState::streaming;
            }
            downstream_.on_event(event);
        }
        void on_observation(const RuntimeObservation &original) override {
            auto event = original;
            event.worker_id = worker_id_;
            if (event.stage == "worker_started") {
                std::lock_guard lock(pool_->mutex);
                auto &worker = pool_->workers.at(slot_);
                if (worker.worker_id == worker_id_)
                    worker.process_id = event.process_id;
            }
            downstream_.on_observation(event);
        }
        std::uint64_t finals() const { return finals_; }
    };
    std::shared_ptr<Pool> pool_;
    std::size_t slot_;
    std::string worker_id_, call_id_;
    IClock &clock_;
    std::unique_ptr<IASREngine> engine_;
    ForwardSink sink_;
    std::unique_ptr<IASRSession> inner_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread watchdog_;
    bool stopping_watchdog_ = false;
    SessionState state_ = SessionState::creating;
    std::int64_t assigned_ns_, last_activity_ns_;
    int idle_timeout_ms_, total_timeout_ms_;
    bool eof_ = false;

    void set_state(SessionState state) {
        state_ = state;
        std::lock_guard lock(pool_->mutex);
        auto &worker = pool_->workers.at(slot_);
        if (worker.worker_id == worker_id_)
            worker.state = state;
    }
    Status deadline() {
        const auto now = clock_.now_ns();
        if (now - assigned_ns_ <= std::int64_t(total_timeout_ms_) * 1000000 &&
            now - last_activity_ns_ <= std::int64_t(idle_timeout_ms_) * 1000000)
            return {};
        if (inner_ && !terminal(state_))
            (void)inner_->cancel(CancelReason::deadline);
        set_state(SessionState::stopped);
        return {ErrorCode::deadline_expired, "session idle or total deadline expired"};
    }
    void release() {
        {
            std::lock_guard lock(mutex_);
            stopping_watchdog_ = true;
        }
        wake_.notify_all();
        if (watchdog_.joinable())
            watchdog_.join();
        // Destroy vendor/session resources before the slot can be admitted again.
        if (inner_ && !terminal(state_)) {
            try {
                (void)inner_->cancel(CancelReason::shutdown);
            } catch (...) {
                // Destruction still owns the worker and must reclaim its slot.
            }
        }
        inner_.reset();
        engine_.reset();
        std::lock_guard lock(pool_->mutex);
        auto &worker = pool_->workers.at(slot_);
        if (worker.worker_id == worker_id_) {
            worker.occupied = false;
            worker.worker_id = "worker_" + std::to_string(slot_);
            worker.healthy = true; // Next assignment launches a fresh process/context.
            worker.process_id = 0;
            worker.active_sessions = 0;
            worker.call_id.clear();
            worker.language.clear();
            worker.state = SessionState::ready;
        }
    }

  public:
    ManagedSession(std::shared_ptr<Pool> pool, std::size_t slot, std::string worker_id, std::string call_id,
                   IClock &clock, std::unique_ptr<IASREngine> engine, IRecognitionSink &sink,
                   std::int64_t assigned_ns, int idle_timeout_ms, int total_timeout_ms)
        : pool_(std::move(pool)), slot_(slot), worker_id_(std::move(worker_id)), call_id_(std::move(call_id)),
          clock_(clock), engine_(std::move(engine)), sink_(sink, pool_, slot_, worker_id_),
          assigned_ns_(assigned_ns), last_activity_ns_(clock.now_ns()), idle_timeout_ms_(idle_timeout_ms),
          total_timeout_ms_(total_timeout_ms) {}
    ~ManagedSession() override { release(); }
    Status initialize(const SessionConfig &config) {
        auto created = engine_->create_session(config, sink_, clock_);
        if (!created)
            return created.status;
        if (!created.value)
            return {ErrorCode::runtime_failure, "executor returned no ASR session"};
        inner_ = std::move(created.value);
        last_activity_ns_ = clock_.now_ns();
        set_state(SessionState::ready);
        if (clock_.domain() == "host_steady")
            watchdog_ = std::thread([this] {
                std::unique_lock lock(mutex_);
                while (!stopping_watchdog_ && !terminal(state_)) {
                    const auto now = clock_.now_ns();
                    const auto total_due = assigned_ns_ + std::int64_t(total_timeout_ms_) * 1000000;
                    const auto idle_due = last_activity_ns_ + std::int64_t(idle_timeout_ms_) * 1000000;
                    const auto due =
                        state_ == SessionState::finalizing ? total_due : std::min(total_due, idle_due);
                    if (now >= due) {
                        try {
                            (void)deadline();
                        } catch (...) {
                            set_state(SessionState::failed);
                        }
                        break;
                    }
                    wake_.wait_for(lock,
                                   std::chrono::nanoseconds(std::min<std::int64_t>(due - now, 50000000)));
                }
            });
        return {};
    }
    void announce_assignment() {
        RuntimeObservation observation;
        observation.stage = "worker_assigned";
        observation.timestamp_ns = assigned_ns_;
        sink_.on_observation(observation);
    }
    bool belongs_to(const std::shared_ptr<Pool> &pool) const { return pool_ == pool; }
    const std::string &call_id() const { return call_id_; }
    Status submit(AudioChunk chunk) override {
        std::lock_guard lock(mutex_);
        if (terminal(state_) || eof_)
            return {ErrorCode::invalid_state, "audio after EOF or terminal state"};
        auto expired = deadline();
        if (!expired)
            return expired;
        auto result = inner_->submit(std::move(chunk));
        if (result) {
            last_activity_ns_ = clock_.now_ns();
            set_state(SessionState::streaming);
            wake_.notify_all();
        } else if (inner_->snapshot().state == SessionState::failed)
            set_state(SessionState::failed);
        return result;
    }
    Status finish_input() override {
        std::lock_guard lock(mutex_);
        if (state_ == SessionState::completed)
            return {};
        if (terminal(state_))
            return {ErrorCode::invalid_state, "EOF after stopped or failed call"};
        auto expired = deadline();
        if (!expired)
            return expired;
        eof_ = true;
        set_state(SessionState::finalizing);
        wake_.notify_all();
        auto result = inner_->finish_input();
        if (!result) {
            set_state(inner_->snapshot().state == SessionState::failed ? SessionState::failed
                                                                       : SessionState::stopped);
            return result;
        }
        if (inner_->snapshot().state != SessionState::completed || sink_.finals() != 1) {
            set_state(SessionState::failed);
            return {ErrorCode::runtime_failure, "session finished without exactly one final"};
        }
        set_state(SessionState::completed);
        return {};
    }
    Status cancel(CancelReason reason) override {
        std::lock_guard lock(mutex_);
        if (state_ == SessionState::stopped || state_ == SessionState::completed)
            return {};
        if (state_ == SessionState::failed)
            return {ErrorCode::invalid_state, "cannot cancel failed call"};
        auto result = inner_->cancel(reason);
        if (result)
            set_state(SessionState::stopped);
        return result;
    }
    SessionSnapshot snapshot() const override {
        std::lock_guard lock(mutex_);
        auto result = inner_->snapshot();
        result.state = state_;
        result.worker_id = worker_id_;
        return result;
    }
};

SessionManager::SessionManager(WorkerLayout layout, std::unique_ptr<IWorkerExecutor> executor,
                               std::unique_ptr<IWorkerScheduler> scheduler)
    : pool_(std::make_shared<Pool>()), layout_(std::move(layout)), executor_(std::move(executor)),
      scheduler_(std::move(scheduler)) {
    if (!executor_ || !scheduler_ || layout_.processes < 1 || layout_.processes > 16 ||
        layout_.inference_slots_per_process != 1 || layout_.max_sessions_per_process != 1 ||
        layout_.model_instances_per_process != 1 || layout_.runtime_threads < 1 ||
        layout_.runtime_threads > 16 || layout_.idle_timeout_ms < 1 || layout_.total_timeout_ms < 1)
        throw std::invalid_argument(
            "M5 requires one isolated context/slot/session per worker, valid budgets");
    auto probe = executor_->make_engine();
    capabilities_ = probe->capabilities();
    if ((!capabilities_.is_mock && executor_->kind() != "process") ||
        (capabilities_.is_mock && executor_->kind() != "in_process"))
        throw std::invalid_argument("executor kind does not match engine isolation");
    capabilities_.concurrent_sessions = layout_.processes > 1;
    for (int i = 0; i < layout_.processes; ++i) {
        WorkerSnapshot worker;
        worker.worker_id = "worker_" + std::to_string(i);
        worker.runtime_threads = layout_.runtime_threads;
        pool_->workers.push_back(std::move(worker));
    }
}
EngineCapabilities SessionManager::capabilities() const { return capabilities_; }
Result<std::unique_ptr<IASRSession>> SessionManager::create_session(const SessionConfig &config,
                                                                    IRecognitionSink &sink, IClock &clock) {
    if (config.run_id.empty() || config.call_id.empty())
        return {{ErrorCode::invalid_input, "run and call IDs required"}, nullptr};
    std::size_t slot;
    std::string worker_id;
    std::int64_t assigned_ns;
    {
        std::lock_guard lock(pool_->mutex);
        if (pool_->draining)
            return {{ErrorCode::invalid_state, "manager is draining"}, nullptr};
        if (pool_->used_call_ids.contains(config.call_id))
            return {{ErrorCode::invalid_input, "call ID already used in this manager"}, nullptr};
        if (pool_->used_call_ids.size() >= 100000)
            return {{ErrorCode::resource_exhausted, "manager call-ID history capacity exceeded"}, nullptr};
        const auto selected = scheduler_->select(pool_->workers);
        if (!selected || *selected >= pool_->workers.size() || pool_->workers[*selected].occupied ||
            !pool_->workers[*selected].healthy)
            return {{ErrorCode::resource_exhausted, "no isolated worker slot available"}, nullptr};
        slot = *selected;
        auto &worker = pool_->workers[slot];
        ++worker.generation;
        worker_id = "worker_" + std::to_string(slot) + "_g" + std::to_string(worker.generation);
        worker.worker_id = worker_id;
        worker.occupied = true;
        worker.active_sessions = 1;
        worker.state = SessionState::creating;
        worker.call_id = config.call_id;
        worker.language = config.language;
        assigned_ns = worker.assigned_ns = clock.now_ns();
        pool_->used_call_ids.insert(config.call_id);
    }
    try {
        auto engine = executor_->make_engine();
        if (engine->capabilities().engine_id != capabilities_.engine_id ||
            engine->capabilities().is_mock != capabilities_.is_mock)
            throw std::runtime_error("executor changed engine type after manager initialization");
        auto managed = std::make_unique<ManagedSession>(pool_, slot, worker_id, config.call_id, clock,
                                                        std::move(engine), sink, assigned_ns,
                                                        layout_.idle_timeout_ms, layout_.total_timeout_ms);
        managed->announce_assignment();
        auto status = managed->initialize(config);
        if (!status)
            return {status, nullptr};
        return {{}, std::move(managed)};
    } catch (const std::exception &error) {
        std::lock_guard lock(pool_->mutex);
        auto &worker = pool_->workers[slot];
        if (worker.worker_id == worker_id) {
            worker.occupied = false;
            worker.worker_id = "worker_" + std::to_string(slot);
            worker.healthy = true;
            worker.active_sessions = 0;
            worker.call_id.clear();
            worker.language.clear();
            worker.state = SessionState::ready;
        }
        return {{ErrorCode::runtime_failure, error.what()}, nullptr};
    }
}
void SessionManager::begin_draining() {
    std::lock_guard lock(pool_->mutex);
    pool_->draining = true;
}
bool SessionManager::draining() const {
    std::lock_guard lock(pool_->mutex);
    return pool_->draining;
}
std::vector<WorkerSnapshot> SessionManager::workers() const {
    std::lock_guard lock(pool_->mutex);
    return pool_->workers;
}
Result<std::unique_ptr<IASRSession>> SessionManager::reset(std::unique_ptr<IASRSession> &current,
                                                           const SessionConfig &next, IRecognitionSink &sink,
                                                           IClock &clock) {
    auto *managed = dynamic_cast<ManagedSession *>(current.get());
    if (!managed || !managed->belongs_to(pool_))
        return {{ErrorCode::invalid_input, "reset needs a session owned by this manager"}, nullptr};
    if (next.call_id == managed->call_id())
        return {{ErrorCode::invalid_input, "reset requires a new call ID"}, nullptr};
    {
        std::lock_guard lock(pool_->mutex);
        if (pool_->draining || pool_->used_call_ids.contains(next.call_id))
            return {{ErrorCode::invalid_state, "reset target is unavailable"}, nullptr};
    }
    (void)current->cancel(CancelReason::user_request);
    current.reset();
    return create_session(next, sink, clock);
}
} // namespace asr
