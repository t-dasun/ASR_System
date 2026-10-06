#pragma once
#include <asr/engines/prefix_engine.hpp>
#include <cerrno>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <unistd.h>

namespace asr::prefix_wire {
using Json = nlohmann::json;
inline bool transfer(int fd, void *buffer, std::size_t size, bool sending) {
    auto *p = static_cast<char *>(buffer);
    while (size) {
        auto n = sending ? ::send(fd, p, size, MSG_NOSIGNAL) : ::recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}
inline bool send(int fd, const Json &record) {
    auto text = record.dump();
    std::uint32_t length = static_cast<std::uint32_t>(text.size());
    return length <= 1024 * 1024 && transfer(fd, &length, sizeof(length), true) &&
           transfer(fd, text.data(), text.size(), true);
}
inline bool receive(int fd, Json &record) {
    std::uint32_t length = 0;
    if (!transfer(fd, &length, sizeof(length), false))
        return false;
    if (!length || length > 1024 * 1024)
        throw std::runtime_error("invalid prefix IPC frame size");
    std::string text(length, '\0');
    if (!transfer(fd, text.data(), length, false))
        return false;
    record = Json::parse(text);
    return true;
}
inline Json status(const Status &s) { return {{"code", int(s.code)}, {"message", s.message}}; }
inline Status status(const Json &j) {
    return {static_cast<ErrorCode>(j.at("code").get<int>()), j.at("message")};
}
inline Json optional(const std::optional<std::int64_t> &v) { return v ? Json(*v) : Json(nullptr); }
inline Json event(const RecognitionEvent &e) {
    return {{"type", "event"},
            {"call_id", e.call_id},
            {"run_id", e.run_id},
            {"producer_id", e.producer_id},
            {"worker_id", e.worker_id},
            {"sequence", e.sequence},
            {"revision", e.revision},
            {"produced_ns", e.produced_ns},
            {"consumed_samples", e.consumed_samples},
            {"clock_domain", e.clock_domain},
            {"utc", e.utc ? Json(*e.utc) : Json(nullptr)},
            {"kind", int(e.kind)},
            {"before_eof", e.before_eof},
            {"text", e.text},
            {"status", status(e.status)}};
}
inline RecognitionEvent event(const Json &j) {
    RecognitionEvent e;
    e.call_id = j.at("call_id");
    e.run_id = j.at("run_id");
    e.producer_id = j.at("producer_id");
    e.worker_id = j.at("worker_id");
    e.sequence = j.at("sequence");
    e.revision = j.at("revision");
    e.produced_ns = j.at("produced_ns");
    e.consumed_samples = j.at("consumed_samples");
    e.clock_domain = j.at("clock_domain");
    if (!j.at("utc").is_null())
        e.utc = j.at("utc").get<std::string>();
    e.kind = static_cast<EventKind>(j.at("kind").get<int>());
    e.before_eof = j.at("before_eof");
    e.text = j.at("text");
    e.status = status(j.at("status"));
    return e;
}
inline Json observation(const RuntimeObservation &o, const std::string &call) {
    return {{"type", "observation"},
            {"call_id", call},
            {"stage", o.stage},
            {"worker_id", o.worker_id},
            {"process_id", o.process_id},
            {"timestamp_ns", o.timestamp_ns},
            {"duration_ns", optional(o.duration_ns)},
            {"sequence", optional(o.sequence)},
            {"buffered_samples", optional(o.buffered_samples)},
            {"cpu_ns", optional(o.cpu_ns)},
            {"peak_rss_bytes", optional(o.peak_rss_bytes)}};
}
inline RuntimeObservation observation(const Json &j) {
    RuntimeObservation o;
    o.stage = j.at("stage");
    o.worker_id = j.at("worker_id");
    o.process_id = j.at("process_id");
    o.timestamp_ns = j.at("timestamp_ns");
    auto get = [&](const char *k, std::optional<std::int64_t> &v) {
        if (!j.at(k).is_null())
            v = j.at(k).get<std::int64_t>();
    };
    get("duration_ns", o.duration_ns);
    get("sequence", o.sequence);
    get("buffered_samples", o.buffered_samples);
    get("cpu_ns", o.cpu_ns);
    get("peak_rss_bytes", o.peak_rss_bytes);
    return o;
}
inline Json worker_status(const PrefixWorkerStatus &s) {
    Json calls = Json::array();
    for (auto &c : s.calls)
        calls.push_back({{"call_id", c.call_id},
                         {"language", c.language},
                         {"state", int(c.state)},
                         {"buffered_samples", c.buffered_samples},
                         {"decoding", c.decoding}});
    return {{"max_calls", s.max_calls},       {"runtime_threads", s.runtime_threads},
            {"blas_threads", s.blas_threads}, {"queued_jobs", s.queued_jobs},
            {"draining", s.draining},         {"failures", s.failures},
            {"last_error", s.last_error},     {"calls", calls}};
}
inline PrefixWorkerStatus worker_status(const Json &j) {
    PrefixWorkerStatus s;
    s.max_calls = j.at("max_calls");
    s.runtime_threads = j.at("runtime_threads");
    s.blas_threads = j.at("blas_threads");
    s.queued_jobs = j.at("queued_jobs");
    s.draining = j.at("draining");
    s.failures = j.at("failures");
    s.last_error = j.at("last_error");
    for (auto &c : j.at("calls"))
        s.calls.push_back({c.at("call_id"), c.at("language"),
                           static_cast<SessionState>(c.at("state").get<int>()), c.at("buffered_samples"),
                           c.at("decoding")});
    return s;
}
} // namespace asr::prefix_wire
