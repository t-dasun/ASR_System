#include <asr/backend/scheduler.hpp>

namespace asr {
std::optional<std::size_t> LeastActiveScheduler::select(const std::vector<WorkerSnapshot> &workers) {
    std::optional<std::size_t> selected;
    for (std::size_t i = 0; i < workers.size(); ++i) {
        const auto &worker = workers[i];
        if (!worker.healthy || worker.occupied || worker.active_sessions >= worker.inference_slots)
            continue;
        if (!selected || worker.active_sessions < workers[*selected].active_sessions)
            selected = i;
    }
    return selected;
}
std::optional<std::size_t> RoundRobinScheduler::select(const std::vector<WorkerSnapshot> &workers) {
    if (workers.empty())
        return std::nullopt;
    for (std::size_t n = 0; n < workers.size(); ++n) {
        const auto i = (cursor_ + n) % workers.size();
        const auto &worker = workers[i];
        if (worker.healthy && !worker.occupied && worker.active_sessions < worker.inference_slots) {
            cursor_ = (i + 1) % workers.size();
            return i;
        }
    }
    return std::nullopt;
}
} // namespace asr
