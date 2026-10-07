#include "engine_runtime.hpp"
#include <asr/backend/session_manager.hpp>
#include <asr/engines/mock_engine.hpp>
#include <unistd.h>
#ifdef ASR_HAS_QWEN_NATIVE
#include <asr/engines/native_engine.hpp>
#include <asr/engines/prefix_engine.hpp>
#include <asr/engines/prefix_pool.hpp>
#endif

namespace asr::app {
namespace {
const char *state_name(asr::SessionState state) {
    switch (state) {
    case asr::SessionState::creating:
        return "creating";
    case asr::SessionState::ready:
        return "ready";
    case asr::SessionState::streaming:
        return "streaming";
    case asr::SessionState::finalizing:
        return "finalizing";
    case asr::SessionState::completed:
        return "completed";
    case asr::SessionState::stopped:
        return "stopped";
    case asr::SessionState::failed:
        return "failed";
    }
    return "unknown";
}
} // namespace

std::unique_ptr<asr::IASREngine> make_engine(const asr::RunConfig &config) {
#ifdef ASR_HAS_QWEN_NATIVE
    const SharedStreamOptions streaming{config.runtime == "qwen_stream", config.decode_step_ms,
                                        config.max_new_tokens, config.stream_unfixed_chunks,
                                        config.refine_final};
    if ((config.runtime == "qwen_prefix" || config.runtime == "qwen_stream") && config.worker_processes > 1)
        return std::make_unique<asr::PrefixProcessPool>(
            std::filesystem::read_symlink("/proc/self/exe").parent_path() / "asr-prefix-worker",
            config.model_path.string(), config.worker_processes, config.max_sessions_per_process,
            config.prefix_preview_ms, config.native_threads, config.idle_timeout_ms, config.total_timeout_ms,
            config.timeout_ms, config.scheduler, streaming);
    if (config.runtime == "qwen_prefix" || config.runtime == "qwen_stream")
        return std::make_unique<asr::PrefixMultiplexEngine>(
            config.model_path.string(), config.max_sessions_per_process, config.prefix_preview_ms,
            config.native_threads, config.idle_timeout_ms, config.total_timeout_ms, config.timeout_ms,
            "prefix_shared_0", streaming);
#endif
    std::unique_ptr<asr::IWorkerExecutor> executor;
    if (config.runtime == "mock")
        executor = std::make_unique<asr::FactoryWorkerExecutor>(
            "in_process", [] { return std::make_unique<asr::MockEngine>(); });
#ifdef ASR_HAS_QWEN_NATIVE
    else {
        asr::NativeQwenOptions options;
        options.worker_executable =
            std::filesystem::read_symlink("/proc/self/exe").parent_path() / "asr-native-worker";
        options.model_directory = config.model_path;
        options.threads = config.native_threads;
        options.decode_step_ms = config.decode_step_ms;
        options.max_new_tokens = config.max_new_tokens;
        options.timeout_ms = config.timeout_ms;
        options.refine_final = config.refine_final;
        options.cpu_cores = config.cpu_cores;
        executor = std::make_unique<asr::FactoryWorkerExecutor>(
            "process", [options] { return std::make_unique<asr::NativeQwenEngine>(options); });
    }
#else
    else
        throw std::invalid_argument("this build has no native Qwen worker; use release-cpu preset");
#endif
    std::unique_ptr<asr::IWorkerScheduler> scheduler;
    if (config.scheduler == "round_robin")
        scheduler = std::make_unique<asr::RoundRobinScheduler>();
    else
        scheduler = std::make_unique<asr::LeastActiveScheduler>();
    asr::WorkerLayout layout;
    layout.processes = config.worker_processes;
    layout.runtime_threads = config.native_threads;
    layout.idle_timeout_ms = config.idle_timeout_ms;
    layout.total_timeout_ms = config.total_timeout_ms;
    layout.cpu_cores = config.cpu_cores;
    return std::make_unique<asr::SessionManager>(layout, std::move(executor), std::move(scheduler));
}
void drain_engine(asr::IASREngine &engine) {
    if (auto *manager = dynamic_cast<asr::SessionManager *>(&engine))
        manager->begin_draining();
#ifdef ASR_HAS_QWEN_NATIVE
    if (auto *prefix = dynamic_cast<asr::PrefixMultiplexEngine *>(&engine))
        prefix->begin_draining();
    if (auto *pool = dynamic_cast<asr::PrefixProcessPool *>(&engine))
        pool->begin_draining();
#endif
}
nlohmann::json workers_json(asr::IASREngine &engine) {
    auto workers = nlohmann::json::array();
    if (auto *manager = dynamic_cast<asr::SessionManager *>(&engine))
        for (const auto &worker : manager->workers())
            workers.push_back({{"worker_id", worker.worker_id},
                               {"call_id", worker.call_id},
                               {"language", worker.language},
                               {"state", state_name(worker.state)},
                               {"occupied", worker.occupied},
                               {"healthy", worker.healthy},
                               {"active_sessions", worker.active_sessions},
                               {"runtime_threads", worker.runtime_threads},
                               {"process_id", worker.process_id},
                               {"failures", worker.failures},
                               {"last_error", worker.last_error},
                               {"server_queue_depth", nullptr}});
#ifdef ASR_HAS_QWEN_NATIVE
    std::vector<asr::PrefixPoolWorkerStatus> prefix_workers;
    if (auto *prefix = dynamic_cast<asr::PrefixMultiplexEngine *>(&engine))
        prefix_workers.push_back({"prefix_shared_0", getpid(), true, prefix->worker_status()});
    if (auto *pool = dynamic_cast<asr::PrefixProcessPool *>(&engine))
        prefix_workers = pool->workers();
    for (const auto &worker : prefix_workers) {
        const auto &status = worker.status;
        auto calls = nlohmann::json::array();
        bool busy = false;
        for (const auto &call : status.calls) {
            busy |= call.decoding;
            calls.push_back({{"call_id", call.call_id},
                             {"language", call.language},
                             {"state", state_name(call.state)},
                             {"buffered_samples", call.buffered_samples},
                             {"decoding", call.decoding}});
        }
        workers.push_back({{"worker_id", worker.worker_id},
                           {"call_id", ""},
                           {"language", "mixed"},
                           {"state", busy ? "streaming" : "ready"},
                           {"occupied", !status.calls.empty()},
                           {"healthy", worker.healthy},
                           {"active_sessions", status.calls.size()},
                           {"max_sessions", status.max_calls},
                           {"runtime_threads", status.runtime_threads},
                           {"blas_threads", status.blas_threads},
                           {"process_id", worker.process_id},
                           {"failures", status.failures},
                           {"last_error", status.last_error},
                           {"server_queue_depth", status.queued_jobs},
                           {"draining", status.draining},
                           {"calls", calls}});
    }
#endif
    return workers;
}
void validate_shared_suite(const asr::RunConfig &startup, const asr::RunConfig &selected) {
    if (startup.runtime != "qwen_prefix" && selected.runtime != "qwen_prefix" &&
        startup.runtime != "qwen_stream" && selected.runtime != "qwen_stream")
        return;
    if (startup.runtime != selected.runtime || startup.model_path != selected.model_path ||
        startup.native_threads != selected.native_threads ||
        startup.stream_unfixed_chunks != selected.stream_unfixed_chunks ||
        startup.max_new_tokens != selected.max_new_tokens || startup.refine_final != selected.refine_final ||
        startup.max_sessions_per_process != selected.max_sessions_per_process ||
        startup.worker_processes != selected.worker_processes || startup.scheduler != selected.scheduler ||
        startup.idle_timeout_ms != selected.idle_timeout_ms ||
        startup.total_timeout_ms != selected.total_timeout_ms || startup.timeout_ms != selected.timeout_ms)
        throw std::invalid_argument("shared-model API suites must retain startup runtime, model, threads, "
                                    "slots, and deadlines; use CLI sweeps for runtime changes");
}
} // namespace asr::app
