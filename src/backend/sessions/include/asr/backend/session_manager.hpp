#pragma once
#include <asr/backend/executor.hpp>
#include <asr/backend/scheduler.hpp>
#include <asr/core/clock.hpp>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace asr {
struct WorkerLayout {
    int processes = 1, inference_slots_per_process = 1, max_sessions_per_process = 1;
    int model_instances_per_process = 1, runtime_threads = 4;
    int idle_timeout_ms = 30000, total_timeout_ms = 600000;
    std::vector<int> cpu_cores;
};
class SessionManager final : public IASREngine {
    class ManagedSession;
    struct Pool {
        mutable std::mutex mutex;
        std::vector<WorkerSnapshot> workers;
        std::unordered_set<std::string> used_call_ids;
        bool draining = false;
    };
    std::shared_ptr<Pool> pool_;
    WorkerLayout layout_;
    std::unique_ptr<IWorkerExecutor> executor_;
    std::unique_ptr<IWorkerScheduler> scheduler_;
    EngineCapabilities capabilities_;

  public:
    SessionManager(WorkerLayout layout, std::unique_ptr<IWorkerExecutor> executor,
                   std::unique_ptr<IWorkerScheduler> scheduler);
    EngineCapabilities capabilities() const override;
    Result<std::unique_ptr<IASRSession>> create_session(const SessionConfig &, IRecognitionSink &,
                                                        IClock &) override;
    // Existing calls may finish normally; admission closes immediately.
    void begin_draining();
    bool draining() const;
    std::vector<WorkerSnapshot> workers() const;
    const WorkerLayout &layout() const { return layout_; }
    std::string scheduler_name() const { return scheduler_->name(); }
    std::string executor_kind() const { return executor_->kind(); }
    // Stop old generation and release its slot before opening the new call ID.
    Result<std::unique_ptr<IASRSession>> reset(std::unique_ptr<IASRSession> &current,
                                               const SessionConfig &next, IRecognitionSink &, IClock &);
};
} // namespace asr
