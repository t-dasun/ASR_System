#pragma once
#include <asr/core/types.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace asr {
struct WorkerSnapshot {
    std::string worker_id, call_id, language;
    std::uint64_t generation = 0, failures = 0;
    bool occupied = false, healthy = true;
    SessionState state = SessionState::ready;
    std::string last_error;
    int process_id = 0, active_sessions = 0, inference_slots = 1, model_instances = 1;
    int runtime_threads = 1;
    std::int64_t assigned_ns = 0;
};
class IWorkerScheduler {
  public:
    virtual ~IWorkerScheduler() = default;
    virtual std::optional<std::size_t> select(const std::vector<WorkerSnapshot> &workers) = 0;
    virtual std::string name() const = 0;
};
class LeastActiveScheduler final : public IWorkerScheduler {
  public:
    std::optional<std::size_t> select(const std::vector<WorkerSnapshot> &workers) override;
    std::string name() const override { return "least_active"; }
};
class RoundRobinScheduler final : public IWorkerScheduler {
    std::size_t cursor_ = 0;

  public:
    std::optional<std::size_t> select(const std::vector<WorkerSnapshot> &workers) override;
    std::string name() const override { return "round_robin"; }
};
} // namespace asr
