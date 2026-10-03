#include <asr/core/clock.hpp>
#include <asr/observability/system_sampler.hpp>
#include <fstream>
#include <set>
#include <sstream>
#include <unistd.h>

namespace asr {
namespace {
using Json = nlohmann::json;
std::string read(const std::string &path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
}
Json memory_fields(const std::string &path) {
    Json result = Json::object();
    std::istringstream lines(read(path));
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string key, unit;
        std::int64_t value;
        if (fields >> key >> value >> unit && unit == "kB")
            result[key.substr(0, key.size() - 1)] = value * 1024;
    }
    return result;
}
Json process(int pid) {
    const auto root = "/proc/" + std::to_string(pid) + "/";
    const auto stat = read(root + "stat");
    const auto end = stat.rfind(')');
    if (end == std::string::npos)
        return nullptr;
    std::istringstream fields(stat.substr(end + 2));
    std::vector<std::string> values;
    for (std::string value; fields >> value;)
        values.push_back(value);
    if (values.size() < 22)
        return nullptr;
    const auto status = read(root + "status");
    const auto smaps = memory_fields(root + "smaps_rollup");
    Json result{{"pid", pid},
                {"start_ticks", std::stoll(values[19])},
                {"cpu_ticks", std::stoll(values[11]) + std::stoll(values[12])},
                {"threads", std::stoi(values[17])},
                {"rss_bytes", std::stoll(values[21]) * sysconf(_SC_PAGESIZE)},
                {"pss_bytes", smaps.value("Pss", Json(nullptr))},
                {"uss_bytes", nullptr},
                {"cpu_core_equivalents", nullptr}};
    if (smaps.contains("Private_Clean") && smaps.contains("Private_Dirty"))
        result["uss_bytes"] =
            smaps["Private_Clean"].get<std::int64_t>() + smaps["Private_Dirty"].get<std::int64_t>();
    const auto position = status.find("Cpus_allowed_list:");
    result["allowed_cpus"] = position == std::string::npos
                                 ? "unknown"
                                 : status.substr(position + 18, status.find('\n', position) - position - 18);
    return result;
}
} // namespace
Json LinuxSystemSampler::sample() {
    SteadyClock clock;
    Json result{{"timestamp_ns", clock.now_ns()},
                {"clock_domain", "host_steady"},
                {"ticks_per_second", sysconf(_SC_CLK_TCK)},
                {"processes", Json::array()},
                {"host_memory_bytes", memory_fields("/proc/meminfo")},
                {"cpu", Json::object()}};
    std::istringstream stats(read("/proc/stat"));
    std::string line;
    while (std::getline(stats, line)) {
        std::istringstream fields(line);
        std::string name;
        fields >> name;
        if (!name.starts_with("cpu"))
            continue;
        std::int64_t total = 0, idle = 0, value;
        for (int i = 0; i < 8 && fields >> value; ++i) {
            total += value; // guest time already included in user/nice; don't double count.
            if (i == 3 || i == 4)
                idle += value;
        }
        result["cpu"][name] = {
            {"total_ticks", total}, {"idle_ticks", idle}, {"utilization_percent", nullptr}};
        if (previous_.contains("cpu") && previous_["cpu"].contains(name)) {
            const auto &before = previous_["cpu"][name];
            auto delta = total - before["total_ticks"].get<std::int64_t>();
            if (delta > 0)
                result["cpu"][name]["utilization_percent"] =
                    100.0 * (delta - idle + before["idle_ticks"].get<std::int64_t>()) / delta;
        }
    }
    std::istringstream vm(read("/proc/vmstat"));
    std::string key;
    std::int64_t value;
    while (vm >> key >> value)
        if (key == "pswpin" || key == "pswpout")
            result["swap_pages"][key] = value;
    std::set<int> seen;
    std::vector<int> pending{static_cast<int>(getpid())};
    while (!pending.empty() && seen.size() < 256) {
        const auto pid = pending.back();
        pending.pop_back();
        if (!seen.insert(pid).second)
            continue;
        auto item = process(pid);
        if (item.is_null())
            continue; // Process may have exited since enumeration.
        if (previous_.contains("processes")) {
            const auto elapsed =
                result["timestamp_ns"].get<std::int64_t>() - previous_["timestamp_ns"].get<std::int64_t>();
            for (const auto &before : previous_["processes"])
                if (before["pid"] == item["pid"] && before["start_ticks"] == item["start_ticks"] &&
                    elapsed > 0)
                    item["cpu_core_equivalents"] =
                        (item["cpu_ticks"].get<double>() - before["cpu_ticks"].get<double>()) /
                        sysconf(_SC_CLK_TCK) / (elapsed / 1e9);
        }
        result["processes"].push_back(std::move(item));
        std::istringstream children(
            read("/proc/" + std::to_string(pid) + "/task/" + std::to_string(pid) + "/children"));
        for (int child; children >> child;)
            pending.push_back(child);
    }
    if (result["cpu"].empty() || result["processes"].empty() || result["host_memory_bytes"].empty())
        throw std::runtime_error("required /proc telemetry unavailable");
    previous_ = result;
    return result;
}
ResourceMonitor::ResourceMonitor(std::unique_ptr<ISystemSampler> sampler, int interval)
    : sampler_(std::move(sampler)), interval_ms_(interval) {
    if (!sampler_ || interval < 50 || interval > 5000)
        throw std::invalid_argument("invalid resource sampler");
    thread_ = std::thread([this] {
        try {
            while (true) {
                if (samples_.size() >= 20000)
                    throw std::runtime_error("resource sample capacity exceeded");
                samples_.push_back(sampler_->sample());
                std::unique_lock lock(mutex_);
                if (condition_.wait_for(lock, std::chrono::milliseconds(interval_ms_),
                                        [&] { return stopped_; }))
                    break;
            }
        } catch (const std::exception &error) {
            error_ = error.what();
        }
    });
}
ResourceMonitor::~ResourceMonitor() { stop(); }
void ResourceMonitor::stop() {
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    condition_.notify_all();
    if (thread_.joinable())
        thread_.join();
}
Json ResourceMonitor::summary() const {
    std::int64_t peak = 0, peak_pss = 0;
    bool pss_available = false;
    double max_cpu = 0;
    bool cpu_available = false;
    int peak_worker_threads = 0;
    for (const auto &sample : samples_) {
        std::int64_t rss = 0, pss = 0;
        bool all_pss = true, any_cpu = false;
        double cpu = 0;
        for (const auto &item : sample["processes"]) {
            if (item["pid"].get<int>() != getpid())
                peak_worker_threads = std::max(peak_worker_threads, item["threads"].get<int>());
            rss += item["rss_bytes"].get<std::int64_t>();
            if (item["pss_bytes"].is_null())
                all_pss = false;
            else
                pss += item["pss_bytes"].get<std::int64_t>();
            if (!item["cpu_core_equivalents"].is_null()) {
                cpu += item["cpu_core_equivalents"].get<double>();
                any_cpu = true;
            }
        }
        peak = std::max(peak, rss);
        if (all_pss) {
            peak_pss = std::max(peak_pss, pss);
            pss_available = true;
        }
        if (any_cpu) {
            max_cpu = std::max(max_cpu, cpu);
            cpu_available = true;
        }
    }
    return {{"enabled", true},
            {"sample_count", samples_.size()},
            {"requested_interval_ms", interval_ms_},
            {"measurement_valid", error_.empty() && !samples_.empty()},
            {"error", error_},
            {"sampled_peak_worker_threads", peak_worker_threads},
            {"sampled_peak_tree_rss_bytes", peak},
            {"sampled_peak_tree_pss_bytes", pss_available ? Json(peak_pss) : Json(nullptr)},
            {"max_tree_cpu_core_equivalents", cpu_available ? Json(max_cpu) : Json(nullptr)},
            {"scope", "controller and descendants, including startup; RSS sum can double-count shared pages"},
            {"cpu_definition", "process CPU seconds / measured wall interval; 1.0 means one logical CPU. "
                               "Host/per-core utilization 0..100 percent."},
            {"sampling_note",
             "sampled peaks are lower bounds; short-lived processes and between-sample peaks may be missed"}};
}
} // namespace asr
