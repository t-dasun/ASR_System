#include <asr/core/clock.hpp>
#include <asr/storage/repository.hpp>
#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <sys/utsname.h>
#include <thread>

namespace asr {
namespace {
void write_json(const std::filesystem::path &path, const nlohmann::json &value) {
    std::ofstream stream(path);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << value.dump(2) << '\n';
    stream.close();
}
void active(bool started, bool sealed) {
    if (!started || sealed)
        throw std::logic_error("repository is not active");
}
const char *kind_name(EventKind kind) {
    switch (kind) {
    case EventKind::partial:
        return "partial";
    case EventKind::final:
        return "final";
    case EventKind::stopped:
        return "stopped";
    case EventKind::failed:
        return "failed";
    }
    return "unknown";
}
const char *error_name(ErrorCode code) {
    switch (code) {
    case ErrorCode::none:
        return "none";
    case ErrorCode::invalid_input:
        return "invalid_input";
    case ErrorCode::unsupported:
        return "unsupported";
    case ErrorCode::invalid_state:
        return "invalid_state";
    case ErrorCode::resource_exhausted:
        return "resource_exhausted";
    case ErrorCode::deadline_expired:
        return "deadline_expired";
    case ErrorCode::runtime_failure:
        return "runtime_failure";
    case ErrorCode::cancelled:
        return "cancelled";
    }
    return "unknown";
}
} // namespace
nlohmann::json event_json(const RecognitionEvent &event) {
    nlohmann::json value{
        {"schema_version", event.schema_version},
        {"run_id", event.run_id},
        {"call_id", event.call_id},
        {"producer_id", event.producer_id},
        {"sequence", event.sequence},
        {"revision", event.revision},
        {"produced_ns", event.produced_ns},
        {"clock_domain", event.clock_domain},
        {"kind", kind_name(event.kind)},
        {"before_eof", event.before_eof},
        {"consumed_samples", event.consumed_samples},
        {"text", event.text},
        {"error", {{"code", error_name(event.status.code)}, {"message", event.status.message}}}};
    value["utc"] = event.utc ? nlohmann::json(*event.utc) : nlohmann::json(nullptr);
    value["published_ns"] =
        event.published_ns ? nlohmann::json(*event.published_ns) : nlohmann::json(nullptr);
    return value;
}
nlohmann::json environment_json(const std::string &engine) {
    utsname info{};
    const bool available = uname(&info) == 0;
    return {{"schema_version", 1},
            {"captured_at", utc_timestamp()},
            {"os", available ? info.sysname : "unknown"},
            {"kernel", available ? info.release : "unknown"},
            {"architecture", available ? info.machine : "unknown"},
            {"logical_cpus", std::thread::hardware_concurrency()},
            {"compiler", __VERSION__},
            {"engine", engine},
            {"purpose", engine == "mock" ? "MOCK foundation run" : "native single-call diagnostic"}};
}
std::string new_run_id(const std::string &prefix) {
    if (prefix.empty() ||
        prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
        throw std::invalid_argument("invalid run ID prefix");
    std::random_device random;
    std::ostringstream value;
    value << prefix << '_' << std::hex << std::chrono::system_clock::now().time_since_epoch().count() << '_';
    for (int i = 0; i < 4; ++i)
        value << std::setw(8) << std::setfill('0') << random();
    return value.str();
}
void MemoryResultRepository::begin(const std::string &id, const nlohmann::json &cfg,
                                   const nlohmann::json &env) {
    if (started_)
        throw std::logic_error("repository already started");
    run_id = id;
    config = cfg;
    environment = env;
    status = "RUNNING";
    started_ = true;
}
void MemoryResultRepository::append(const RecognitionEvent &event) {
    active(started_, sealed_);
    if (event.run_id != run_id)
        throw std::invalid_argument("event run ID mismatch");
    events.push_back(event);
}
void MemoryResultRepository::append_audio(const nlohmann::json &timing) {
    active(started_, sealed_);
    audio_timings.push_back(timing);
}
void MemoryResultRepository::append_record(const std::string &stream, const nlohmann::json &record) {
    active(started_, sealed_);
    records[stream].push_back(record);
}
void MemoryResultRepository::finish(const std::string &state, const nlohmann::json &result) {
    active(started_, sealed_);
    status = state;
    summary = result;
    sealed_ = true;
}
void FileResultRepository::write_status(const std::string &status) {
    const auto temporary = directory_ / "status.json.tmp";
    write_json(temporary,
               {{"schema_version", 1}, {"run_id", run_id_}, {"status", status}, {"is_mock", is_mock_}});
    std::filesystem::rename(temporary, directory_ / "status.json");
}
void FileResultRepository::begin(const std::string &id, const nlohmann::json &cfg,
                                 const nlohmann::json &env) {
    if (started_)
        throw std::logic_error("repository already started");
    if (id.empty() || id.size() > 128 ||
        id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
            std::string::npos)
        throw std::invalid_argument("invalid run ID");
    std::filesystem::create_directories(root_);
    directory_ = root_ / id;
    if (!std::filesystem::create_directory(directory_))
        throw std::runtime_error("run directory already exists");
    run_id_ = id;
    is_mock_ = !cfg.contains("model") || cfg["model"].value("runtime", "mock") == "mock";
    started_ = true;
    write_status("RUNNING");
    write_json(directory_ / "config.json", cfg);
    write_json(directory_ / "environment.json", env);
    events_.exceptions(std::ios::badbit | std::ios::failbit);
    events_.open(directory_ / "events.jsonl");
    audio_timings_.exceptions(std::ios::badbit | std::ios::failbit);
    audio_timings_.open(directory_ / "audio_timing.jsonl");
    for (const auto *name : {"runtime_timing", "system_metrics", "calls", "workers", "errors"}) {
        auto &stream = records_[name];
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.open(directory_ / (std::string(name) + ".jsonl"));
    }
}
void FileResultRepository::append(const RecognitionEvent &event) {
    active(started_, sealed_);
    if (event.run_id != run_id_)
        throw std::invalid_argument("event run ID mismatch");
    events_ << event_json(event).dump() << '\n';
    events_.flush();
}
void FileResultRepository::append_audio(const nlohmann::json &timing) {
    active(started_, sealed_);
    audio_timings_ << timing.dump() << '\n';
    audio_timings_.flush();
}
void FileResultRepository::append_record(const std::string &stream, const nlohmann::json &record) {
    active(started_, sealed_);
    if (!records_.contains(stream))
        throw std::invalid_argument("unknown artifact stream: " + stream);
    records_.at(stream) << record.dump() << '\n';
}
void FileResultRepository::finish(const std::string &status, const nlohmann::json &summary) {
    active(started_, sealed_);
    events_.flush();
    audio_timings_.flush();
    for (auto &[name, stream] : records_)
        stream.flush();
    write_json(directory_ / "summary.json", summary);
    write_status(status);
    sealed_ = true;
}
} // namespace asr
