#include <asr/config/config.hpp>
#include <charconv>
#include <fstream>
#include <set>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

namespace asr {
namespace {
using Json = nlohmann::json;
[[noreturn]] void invalid(const std::string &key, const std::string &message) {
    throw std::invalid_argument(key + ": " + message);
}
void check_tag(const YAML::Node &node, const std::string &path) {
    if (!node.Tag().empty() && node.Tag() != "?" && node.Tag() != "!")
        invalid(path, "explicit YAML tags are unsupported");
}
void merge(Json &target, const YAML::Node &node, const std::string &path, unsigned depth = 0) {
    if (depth > 12)
        invalid(path, "YAML nesting limit exceeded");
    check_tag(node, path);
    if (target.is_object()) {
        if (!node.IsMap())
            invalid(path, "expected a mapping");
        std::set<std::string> seen;
        for (const auto &entry : node) {
            if (!entry.first.IsScalar())
                invalid(path, "mapping keys must be strings");
            check_tag(entry.first, path);
            const auto key = entry.first.Scalar();
            if (!seen.insert(key).second)
                invalid(path + "." + key, "duplicate key");
            if (!target.contains(key))
                invalid(path + "." + key, "unknown setting");
            merge(target[key], entry.second, path + "." + key, depth + 1);
        }
        return;
    }
    if (!node.IsScalar())
        invalid(path, "expected a scalar");
    const auto value = node.Scalar();
    if (target.is_boolean()) {
        if (value != "true" && value != "false")
            invalid(path, "expected true or false");
        target = value == "true";
    } else if (target.is_number_integer()) {
        std::int64_t number = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
            invalid(path, "expected a decimal integer");
        target = number;
    } else {
        target = value;
    }
}
Json defaults() {
    return {{"schema_version", 1},
            {"experiment", {{"name", "mock_baseline"}, {"seed", 42}}},
            {"model",
             {{"runtime", "mock"},
              {"device", "cpu"},
              {"path", ""},
              {"threads", 4},
              {"decode_step_ms", 2000},
              {"max_new_tokens", 32},
              {"timeout_ms", 45000},
              {"refine_final", true}}},
            {"audio",
             {{"source", "synthetic"},
              {"path", ""},
              {"channel_mix", "reject"},
              {"sample_rate_hz", 16000},
              {"channels", 1},
              {"chunk_ms", 200},
              {"duration_ms", 1200},
              {"queue_max_chunks", 8},
              {"queue_max_ms", 1000},
              {"queue_max_bytes", 32000},
              {"max_lag_ms", 1000},
              {"late_tolerance_ms", 5},
              {"realtime_pacing", false}}},
            {"dataset", {{"language", "en"}}},
            {"metrics", {{"resource_sampling", true}, {"sample_interval_ms", 200}}},
            {"mock", {{"partial_every_ms", 400}}},
            {"output", {{"directory", "results"}}}};
}
void range(const Json &value, std::int64_t low, std::int64_t high, const std::string &path) {
    const auto number = value.get<std::int64_t>();
    if (number < low || number > high)
        invalid(path, "outside allowed range " + std::to_string(low) + ".." + std::to_string(high));
}
} // namespace
RunConfig resolve_config(const std::filesystem::path &yaml_file, const std::vector<std::string> &overrides) {
    auto root = defaults();
    auto base = std::filesystem::current_path();
    if (!yaml_file.empty()) {
        if (std::filesystem::file_size(yaml_file) > 65536)
            invalid("config", "file exceeds 64 KiB");
        const auto documents = YAML::LoadAllFromFile(yaml_file.string());
        if (documents.size() != 1)
            invalid("config", "expected exactly one YAML document");
        merge(root, documents.front(), "config");
        base = std::filesystem::absolute(yaml_file).parent_path();
    }
    for (const auto &override_value : overrides) {
        const auto separator = override_value.find('=');
        if (separator == std::string::npos || separator == 0)
            invalid("override", "expected section.key=value");
        const auto path = override_value.substr(0, separator);
        auto *target = &root;
        std::size_t start = 0;
        while (true) {
            const auto end = path.find('.', start);
            const auto key = path.substr(start, end == std::string::npos ? end : end - start);
            if (!target->is_object() || !target->contains(key))
                invalid(path, "unknown override setting");
            target = &(*target)[key];
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        if (target->is_object())
            invalid(path, "override must name a scalar setting");
        // Values are literal CLI strings, interpreted according to the schema.
        merge(*target, YAML::Node(override_value.substr(separator + 1)), path);
    }
    if (root["schema_version"] != 1)
        invalid("schema_version", "only version 1 is supported");
    if (root["model"]["device"] != "cpu")
        invalid("model.device", "CPU only");
    const auto runtime = root["model"]["runtime"].get<std::string>();
    if (runtime != "mock" && runtime != "qwen_native")
        invalid("model.runtime", "expected mock or qwen_native");
    range(root["model"]["threads"], 1, 16, "model.threads");
    range(root["model"]["decode_step_ms"], 1000, 8000, "model.decode_step_ms");
    range(root["model"]["max_new_tokens"], 1, 256, "model.max_new_tokens");
    range(root["model"]["timeout_ms"], 1000, 600000, "model.timeout_ms");
    const auto model_path = root["model"]["path"].get<std::string>();
    if ((runtime == "mock" && !model_path.empty()) || (runtime == "qwen_native" && model_path.empty()))
        invalid("model.path", "mock requires no model path; qwen_native requires one");
    if (runtime == "qwen_native" && !root["audio"]["realtime_pacing"].get<bool>())
        invalid("audio.realtime_pacing", "native run requires true");
    if (root["audio"]["sample_rate_hz"] != 16000 || root["audio"]["channels"] != 1)
        invalid("audio", "engine input must be mono 16 kHz PCM");
    const auto audio_source = root["audio"]["source"].get<std::string>();
    const auto wav_path = root["audio"]["path"].get<std::string>();
    const auto channel_mix = root["audio"]["channel_mix"].get<std::string>();
    if ((audio_source != "synthetic" && audio_source != "wav") ||
        (audio_source == "wav" && wav_path.empty()) || (audio_source == "synthetic" && !wav_path.empty()))
        invalid("audio.source", "expected synthetic without path, or wav with path");
    if (channel_mix != "reject" && channel_mix != "average" && channel_mix != "left")
        invalid("audio.channel_mix", "expected reject, average, or left");
    range(root["audio"]["chunk_ms"], 1, 1000, "audio.chunk_ms");
    range(root["audio"]["duration_ms"], 0, 600000, "audio.duration_ms");
    range(root["audio"]["queue_max_chunks"], 1, 10000, "audio.queue_max_chunks");
    range(root["audio"]["queue_max_ms"], 1, 60000, "audio.queue_max_ms");
    range(root["audio"]["queue_max_bytes"], 2, 2000000, "audio.queue_max_bytes");
    range(root["audio"]["max_lag_ms"], 0, 60000, "audio.max_lag_ms");
    range(root["audio"]["late_tolerance_ms"], 0, 60000, "audio.late_tolerance_ms");
    if (root["audio"]["queue_max_ms"].get<int>() < root["audio"]["chunk_ms"].get<int>() ||
        root["audio"]["queue_max_bytes"].get<int>() < root["audio"]["chunk_ms"].get<int>() * 32 ||
        root["audio"]["late_tolerance_ms"].get<int>() > root["audio"]["max_lag_ms"].get<int>())
        invalid("audio", "queue ms/bytes must cover one chunk; late tolerance must not exceed max lag");
    range(root["mock"]["partial_every_ms"], 1, 60000, "mock.partial_every_ms");
    range(root["experiment"]["seed"], 0, 4294967295LL, "experiment.seed");
    const auto name = root["experiment"]["name"].get<std::string>();
    if (name.empty() || name.size() > 128)
        invalid("experiment.name", "must contain 1..128 bytes");
    const auto language = root["dataset"]["language"].get<std::string>();
    if (language != "en" && language != "id" && language != "zh")
        invalid("dataset.language", "expected en, id, or zh");
    const auto directory = root["output"]["directory"].get<std::string>();
    if (directory.empty() || directory.find('\0') != std::string::npos)
        invalid("output.directory", "invalid path");
    RunConfig config;
    range(root["metrics"]["sample_interval_ms"], 50, 5000, "metrics.sample_interval_ms");
    config.resource_sampling = root["metrics"]["resource_sampling"].get<bool>();
    config.sample_interval_ms = root["metrics"]["sample_interval_ms"].get<int>();
    config.name = name;
    config.runtime = runtime;
    config.native_threads = root["model"]["threads"].get<int>();
    config.decode_step_ms = root["model"]["decode_step_ms"].get<int>();
    config.max_new_tokens = root["model"]["max_new_tokens"].get<int>();
    config.timeout_ms = root["model"]["timeout_ms"].get<int>();
    config.refine_final = root["model"]["refine_final"].get<bool>();
    if (runtime == "qwen_native") {
        config.model_path = (base / model_path).lexically_normal();
        root["model"]["path"] = config.model_path.string();
    }
    config.language = language;
    config.seed = root["experiment"]["seed"].get<std::uint32_t>();
    config.chunk_ms = root["audio"]["chunk_ms"].get<int>();
    config.duration_ms = root["audio"]["duration_ms"].get<int>();
    config.queue_max_chunks = root["audio"]["queue_max_chunks"].get<int>();
    config.queue_max_ms = root["audio"]["queue_max_ms"].get<int>();
    config.queue_max_bytes = root["audio"]["queue_max_bytes"].get<int>();
    config.max_lag_ms = root["audio"]["max_lag_ms"].get<int>();
    config.late_tolerance_ms = root["audio"]["late_tolerance_ms"].get<int>();
    config.partial_every_ms = root["mock"]["partial_every_ms"].get<int>();
    config.realtime = root["audio"]["realtime_pacing"].get<bool>();
    config.audio_source = audio_source;
    config.channel_mix = channel_mix;
    if (audio_source == "wav") {
        config.wav_path = (base / wav_path).lexically_normal();
        root["audio"]["path"] = config.wav_path.string();
    }
    config.output_directory = (base / directory).lexically_normal();
    root["output"]["directory"] = config.output_directory.string();
    config.resolved = std::move(root);
    return config;
}
} // namespace asr
