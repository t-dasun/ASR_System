#include <algorithm>
#include <asr/backend/api_service.hpp>
#include <asr/storage/repository.hpp>
#include <fstream>
#include <set>
#include <stdexcept>

namespace asr {
namespace {
using Json = nlohmann::json;
HttpResponse json_response(int code, const Json &value) { return {code, "application/json", value.dump()}; }
bool safe_id(const std::string &value) {
    return !value.empty() && value.size() <= 128 &&
           std::all_of(value.begin(), value.end(),
                       [](unsigned char ch) { return std::isalnum(ch) || ch == '_' || ch == '-'; });
}
Json read_json_file(const std::filesystem::path &path) {
    if (!std::filesystem::is_regular_file(path) || std::filesystem::is_symlink(path) ||
        std::filesystem::file_size(path) > 4 * 1024 * 1024)
        throw std::invalid_argument("artifact unavailable");
    std::ifstream input(path);
    return Json::parse(input);
}
std::vector<std::string> requested_overrides(const Json &body) {
    if (!body.is_object() || !body.value("overrides", Json::array()).is_array())
        throw std::invalid_argument("expected an object with an overrides array");
    const auto array = body.value("overrides", Json::array());
    if (array.size() > 32)
        throw std::invalid_argument("too many overrides");
    std::vector<std::string> result;
    for (const auto &item : array) {
        if (!item.is_string())
            throw std::invalid_argument("override must be a string");
        const auto value = item.get<std::string>();
        if (value.size() > 256 || value.starts_with("output.") || value.starts_with("audio.path=") ||
            value.starts_with("model.path="))
            throw std::invalid_argument("file paths cannot be set over HTTP");
        result.push_back(value);
    }
    return result;
}
} // namespace
ApiService::ApiService(std::filesystem::path config_path, std::vector<std::string> base_overrides,
                       std::filesystem::path output_root, Json capabilities, SuiteExecutor execute,
                       ServiceAdmissionGate *gate, RuntimeSnapshot runtime)
    : config_path_(std::move(config_path)), output_root_(std::move(output_root)),
      base_overrides_(std::move(base_overrides)), capabilities_(std::move(capabilities)),
      execute_(std::move(execute)), runtime_(std::move(runtime)), gate_(gate) {
    if (!execute_)
        throw std::invalid_argument("API suite executor is required");
}
ApiService::~ApiService() {
    std::vector<std::shared_ptr<Job>> jobs;
    {
        std::lock_guard lock(mutex_);
        for (const auto &[id, job] : jobs_) {
            (void)id;
            job->cancel = true;
            jobs.push_back(job);
        }
    }
    for (const auto &job : jobs)
        if (job->thread.joinable())
            job->thread.join();
}
HttpResponse ApiService::handle(const HttpRequest &request) {
    try {
        if (request.method == "GET" && request.target == "/v1/capabilities")
            return json_response(200, capabilities_);
        if (request.method == "GET" && request.target == "/v1/runtime")
            return runtime_
                       ? json_response(200, runtime_())
                       : json_response(
                             200, {{"schema_version", 1}, {"workers", Json::array()}, {"system", nullptr}});
        if (request.method == "GET" && request.target == "/v1/jobs") {
            Json items = Json::array();
            std::lock_guard lock(mutex_);
            for (const auto &[id, job] : jobs_) {
                (void)id;
                items.push_back({{"job_id", job->id}, {"status", job->state.value("status", "UNKNOWN")}});
            }
            return json_response(200, {{"schema_version", 1}, {"items", items}});
        }
        if (request.method == "POST" && request.target == "/v1/config/resolve") {
            auto body = Json::parse(request.body);
            auto overrides = base_overrides_;
            const auto requested = requested_overrides(body);
            overrides.insert(overrides.end(), requested.begin(), requested.end());
            const auto config = resolve_config(config_path_, overrides);
            return json_response(200, {{"schema_version", 1}, {"resolved", config.resolved}});
        }
        if (request.method == "GET" && request.target == "/v1/history") {
            Json items = Json::array();
            if (std::filesystem::is_directory(output_root_))
                for (const auto &entry : std::filesystem::directory_iterator(output_root_)) {
                    const auto id = entry.path().filename().string();
                    if (!safe_id(id) || !entry.is_directory() || entry.is_symlink())
                        continue;
                    const auto summary = entry.path() / "summary.json";
                    const auto status = entry.path() / "status.json";
                    if (!std::filesystem::is_regular_file(summary) &&
                        !std::filesystem::is_regular_file(status))
                        continue;
                    Json item = {{"id", id}};
                    try {
                        if (std::filesystem::exists(status))
                            item["status"] = read_json_file(status);
                    } catch (const std::exception &) {
                        item["status"] = {{"status", "UNREADABLE"}};
                    }
                    items.push_back(std::move(item));
                    if (items.size() >= 100)
                        break;
                }
            return json_response(200, {{"schema_version", 1}, {"items", items}});
        }
        if (request.method == "GET" && request.target.starts_with("/v1/history/")) {
            const auto id = request.target.substr(std::string("/v1/history/").size());
            if (!safe_id(id))
                return json_response(404, {{"error", "not found"}});
            const auto directory = output_root_ / id;
            if (!std::filesystem::is_directory(directory) || std::filesystem::is_symlink(directory))
                return json_response(404, {{"error", "not found"}});
            if (!std::filesystem::is_regular_file(directory / "summary.json"))
                return json_response(404, {{"error", "not found"}});
            return json_response(200, read_json_file(directory / "summary.json"));
        }
        if (request.method == "GET" && request.target.starts_with("/v1/artifacts/")) {
            const auto rest = request.target.substr(std::string("/v1/artifacts/").size());
            const auto slash = rest.find('/');
            if (slash == std::string::npos)
                return json_response(404, {{"error", "not found"}});
            const auto id = rest.substr(0, slash);
            const auto remainder = rest.substr(slash + 1);
            const auto nested = remainder.find('/');
            const auto call_id = nested == std::string::npos ? std::string{} : remainder.substr(0, nested);
            const auto file = nested == std::string::npos ? remainder : remainder.substr(nested + 1);
            static const std::set<std::string> allowed{
                "summary.json",         "status.json",  "plan.json",          "config.json",
                "environment.json",     "events.jsonl", "audio_timing.jsonl", "runtime_timing.jsonl",
                "system_metrics.jsonl", "calls.jsonl",  "workers.jsonl",      "errors.jsonl",
                "report.json"};
            if (!safe_id(id) || (!call_id.empty() && !safe_id(call_id)) || !allowed.contains(file))
                return json_response(404, {{"error", "not found"}});
            const auto suite_directory = output_root_ / id;
            if (!std::filesystem::is_directory(suite_directory) ||
                std::filesystem::is_symlink(suite_directory))
                return json_response(404, {{"error", "not found"}});
            const auto directory = call_id.empty() ? suite_directory : suite_directory / call_id;
            if (!std::filesystem::is_directory(directory) || std::filesystem::is_symlink(directory))
                return json_response(404, {{"error", "not found"}});
            const auto path = directory / file;
            if (!std::filesystem::is_regular_file(path) || std::filesystem::is_symlink(path) ||
                std::filesystem::file_size(path) > 4 * 1024 * 1024)
                return json_response(404, {{"error", "not found"}});
            std::ifstream input(path, std::ios::binary);
            const std::string content((std::istreambuf_iterator<char>(input)),
                                      std::istreambuf_iterator<char>());
            return {200, file.ends_with(".jsonl") ? "application/x-ndjson" : "application/json", content};
        }
        if (request.method == "POST" &&
            (request.target == "/v1/suites/dry-run" || request.target == "/v1/suites")) {
            const auto body = Json::parse(request.body);
            (void)requested_overrides(body);
            if (request.target.ends_with("dry-run")) {
                const std::atomic<bool> cancelled{false};
                return json_response(200, execute_(body, true, cancelled));
            }
            const auto key = body.value("idempotency_key", "");
            if (!key.empty() && !safe_id(key))
                throw std::invalid_argument("invalid idempotency key");
            std::shared_ptr<Job> job;
            {
                std::lock_guard lock(mutex_);
                if (!key.empty()) {
                    const auto prior = idempotency_.find(key);
                    if (prior != idempotency_.end()) {
                        if (idempotency_bodies_.at(key) != body.dump())
                            return json_response(409,
                                                 {{"error", "idempotency key reused with another request"}});
                        return json_response(202, {{"job_id", prior->second}, {"idempotent", true}});
                    }
                }
                if (jobs_.size() >= 128) {
                    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
                        if (it->second->state.value("status", "") == "RUNNING")
                            continue;
                        if (it->second->thread.joinable())
                            it->second->thread.join();
                        for (auto key_it = idempotency_.begin(); key_it != idempotency_.end();) {
                            if (key_it->second == it->first) {
                                idempotency_bodies_.erase(key_it->first);
                                key_it = idempotency_.erase(key_it);
                            } else
                                ++key_it;
                        }
                        jobs_.erase(it);
                        break;
                    }
                }
                if (jobs_.size() >= 128)
                    return json_response(429, {{"error", "job history capacity reached"}});
                for (const auto &[id, existing] : jobs_) {
                    (void)id;
                    if (existing->state.value("status", "") == "RUNNING")
                        return json_response(409, {{"error", "one suite is already running"}});
                }
                job = std::make_shared<Job>();
                job->id = new_run_id("job");
                job->state = {{"job_id", job->id}, {"status", "RUNNING"}};
                if (gate_ && !gate_->enter_suite())
                    return json_response(409, {{"error", "interactive call is active"}});
                jobs_.emplace(job->id, job);
                if (!key.empty())
                    idempotency_.emplace(key, job->id);
                if (!key.empty())
                    idempotency_bodies_.emplace(key, body.dump());
            }
            try {
                job->thread = std::thread([this, job, body] {
                    Json state;
                    try {
                        auto result = execute_(body, false, job->cancel);
                        state = {{"job_id", job->id},
                                 {"status", job->cancel ? "STOPPED" : result.value("status", "FAILED")},
                                 {"result", std::move(result)}};
                    } catch (const std::exception &error) {
                        state = {{"job_id", job->id}, {"status", "FAILED"}, {"error", error.what()}};
                    }
                    if (gate_)
                        gate_->leave_suite();
                    std::lock_guard lock(mutex_);
                    job->state = std::move(state);
                });
            } catch (...) {
                std::lock_guard lock(mutex_);
                jobs_.erase(job->id);
                if (!key.empty()) {
                    idempotency_.erase(key);
                    idempotency_bodies_.erase(key);
                }
                if (gate_)
                    gate_->leave_suite();
                throw;
            }
            return json_response(202, {{"job_id", job->id}, {"status", "RUNNING"}});
        }
        if (request.target.starts_with("/v1/jobs/")) {
            auto rest = request.target.substr(std::string("/v1/jobs/").size());
            const bool stop = rest.ends_with("/stop");
            if (stop)
                rest.resize(rest.size() - 5);
            if (!safe_id(rest))
                return json_response(404, {{"error", "not found"}});
            std::lock_guard lock(mutex_);
            const auto found = jobs_.find(rest);
            if (found == jobs_.end())
                return json_response(404, {{"error", "not found"}});
            if (request.method == "POST" && stop) {
                found->second->cancel = true;
                return json_response(202, {{"job_id", rest}, {"stop_requested", true}});
            }
            if (request.method == "GET" && !stop)
                return json_response(200, found->second->state);
        }
        return json_response(404, {{"error", "not found"}});
    } catch (const std::invalid_argument &error) {
        return json_response(400, {{"error", error.what()}});
    } catch (const Json::exception &error) {
        return json_response(400, {{"error", error.what()}});
    } catch (const std::exception &) {
        return json_response(500, {{"error", "internal server error"}});
    }
}
} // namespace asr
