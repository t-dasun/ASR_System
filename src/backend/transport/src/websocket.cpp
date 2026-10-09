#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <asr/backend/websocket.hpp>
#include <asr/core/clock.hpp>
#include <asr/observability/metrics.hpp>
#include <asr/storage/repository.hpp>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <random>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace asr {
class ObservationHub {
  public:
    struct Feed {
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<nlohmann::json> messages;
        std::uint64_t next = 1, dropped = 0;
        bool active = false, terminal = false;
    };
    std::shared_ptr<Feed> get(const std::string &key) {
        std::lock_guard lock(mutex_);
        const auto existing = feeds_.find(key);
        if (existing != feeds_.end())
            return existing->second;
        if (feeds_.size() >= 128) {
            for (auto it = order_.begin(); it != order_.end(); ++it) {
                const auto found = feeds_.find(*it);
                if (found != feeds_.end()) {
                    std::lock_guard feed_lock(found->second->mutex);
                    if (found->second->terminal) {
                        feeds_.erase(found);
                        order_.erase(it);
                        break;
                    }
                }
            }
        }
        if (feeds_.size() >= 128)
            throw std::runtime_error("observation feed capacity exhausted");
        auto feed = std::make_shared<Feed>();
        feeds_.emplace(key, feed);
        order_.push_back(key);
        return feed;
    }
    void publish(const std::shared_ptr<Feed> &feed, nlohmann::json value) {
        std::lock_guard lock(feed->mutex);
        value["delivery_sequence"] = feed->next++;
        if (feed->messages.size() == 128) {
            feed->messages.pop_front();
            ++feed->dropped;
        }
        feed->messages.push_back(std::move(value));
        feed->changed.notify_all();
    }
    void close(const std::shared_ptr<Feed> &feed) {
        std::lock_guard lock(feed->mutex);
        feed->active = false;
        feed->terminal = true;
        feed->changed.notify_all();
    }
    bool activate(const std::shared_ptr<Feed> &feed) {
        std::lock_guard lock(feed->mutex);
        if (feed->active || feed->terminal)
            return false;
        feed->active = true;
        return true;
    }

  private:
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Feed>> feeds_;
    std::deque<std::string> order_;
};
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_header = 8192, max_frame = 65536;
bool allowed_origin(const std::string &origin, std::uint16_t service_port) {
    if (origin == "http://127.0.0.1" || origin == "http://localhost")
        return true;
    for (const auto *host : {"http://127.0.0.1:", "http://localhost:"}) {
        const std::string prefix(host);
        if (!origin.starts_with(prefix))
            continue;
        const auto port = origin.substr(prefix.size());
        if (port == "5173" || port == "4173" || port == std::to_string(service_port))
            return true;
    }
    return false;
}
void send_all(int fd, const void *data, std::size_t size) {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    while (size) {
        const auto sent = ::send(fd, bytes, size, MSG_NOSIGNAL);
        if (sent <= 0)
            throw std::runtime_error("WebSocket send failed");
        bytes += sent;
        size -= std::size_t(sent);
    }
}
void recv_all(int fd, void *data, std::size_t size) {
    auto *bytes = static_cast<std::uint8_t *>(data);
    while (size) {
        const auto received = ::recv(fd, bytes, size, 0);
        if (received <= 0)
            throw std::runtime_error("WebSocket peer disconnected or timed out");
        bytes += received;
        size -= std::size_t(received);
    }
}
std::string read_header(int fd) {
    std::string header;
    while (header.size() < max_header) {
        char byte;
        recv_all(fd, &byte, 1);
        header.push_back(byte);
        if (header.ends_with("\r\n\r\n"))
            return header;
    }
    throw std::runtime_error("WebSocket HTTP header exceeds 8 KiB");
}
std::string header_value(const std::string &header, std::string_view name) {
    auto lower = header;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    const auto key = std::string("\r\n") + std::string(name) + ":";
    const auto found = lower.find(key);
    if (found == std::string::npos)
        return {};
    auto begin = found + key.size();
    while (begin < header.size() && header[begin] == ' ')
        ++begin;
    const auto end = header.find("\r\n", begin);
    return header.substr(begin, end - begin);
}
std::string base64(const unsigned char *bytes, int count) {
    std::string encoded(4 * ((count + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()), bytes, count);
    return encoded;
}
std::string accept_key(const std::string &key) {
    const auto source = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(source.data(), source.size(), digest, &length, EVP_sha1(), nullptr) != 1)
        throw std::runtime_error("WebSocket handshake hash failed");
    return base64(digest, int(length));
}
struct Frame {
    std::uint8_t opcode = 0;
    std::vector<std::uint8_t> payload;
};
void write_frame(int fd, std::uint8_t opcode, const std::vector<std::uint8_t> &payload, bool mask) {
    if (payload.size() > max_frame)
        throw std::invalid_argument("WebSocket frame exceeds 64 KiB");
    std::array<std::uint8_t, 8> header{};
    header[0] = 0x80 | opcode;
    std::size_t header_size = 2;
    if (payload.size() < 126)
        header[1] = std::uint8_t(payload.size());
    else {
        header[1] = 126;
        header[2] = std::uint8_t(payload.size() >> 8);
        header[3] = std::uint8_t(payload.size());
        header_size = 4;
    }
    if (mask)
        header[1] |= 0x80;
    send_all(fd, header.data(), header_size);
    if (mask) {
        std::random_device random;
        std::array<std::uint8_t, 4> key{};
        for (auto &byte : key)
            byte = std::uint8_t(random());
        send_all(fd, key.data(), key.size());
        auto encoded = payload;
        for (std::size_t i = 0; i < encoded.size(); ++i)
            encoded[i] ^= key[i % 4];
        send_all(fd, encoded.data(), encoded.size());
    } else
        send_all(fd, payload.data(), payload.size());
}
void write_text(int fd, const Json &value, bool mask) {
    const auto text = value.dump();
    write_frame(fd, 1, {text.begin(), text.end()}, mask);
}
Frame read_frame(int fd, bool client_frame) {
    std::uint8_t header[2];
    recv_all(fd, header, sizeof(header));
    if ((header[0] & 0x80) == 0 || (header[0] & 0x70) != 0)
        throw std::runtime_error("fragmented or extended WebSocket frame unsupported");
    const bool masked = (header[1] & 0x80) != 0;
    if (masked != client_frame)
        throw std::runtime_error("WebSocket mask direction invalid");
    std::uint64_t size = header[1] & 0x7f;
    if (size == 127)
        throw std::runtime_error("oversized WebSocket frame");
    if (size == 126) {
        std::uint8_t length[2];
        recv_all(fd, length, 2);
        size = (std::uint64_t(length[0]) << 8) | length[1];
    }
    if (size > max_frame)
        throw std::runtime_error("WebSocket frame exceeds 64 KiB");
    std::array<std::uint8_t, 4> key{};
    if (masked)
        recv_all(fd, key.data(), key.size());
    Frame frame;
    frame.opcode = header[0] & 0x0f;
    frame.payload.resize(size);
    if (size)
        recv_all(fd, frame.payload.data(), size);
    if (masked)
        for (std::size_t i = 0; i < frame.payload.size(); ++i)
            frame.payload[i] ^= key[i % 4];
    return frame;
}
Json read_json_frame(int fd, bool client_frame) {
    auto frame = read_frame(fd, client_frame);
    if (frame.opcode != 1 || frame.payload.size() > 16384)
        throw std::runtime_error("expected bounded WebSocket JSON text frame");
    return Json::parse(frame.payload.begin(), frame.payload.end());
}
Status status_from_json(const Json &value) {
    const auto code = value.value("code", "none");
    const auto message = value.value("message", "");
    if (code == "none")
        return {};
    if (code == "invalid_input")
        return {ErrorCode::invalid_input, message};
    if (code == "unsupported")
        return {ErrorCode::unsupported, message};
    if (code == "invalid_state")
        return {ErrorCode::invalid_state, message};
    if (code == "resource_exhausted")
        return {ErrorCode::resource_exhausted, message};
    if (code == "deadline_expired")
        return {ErrorCode::deadline_expired, message};
    if (code == "cancelled")
        return {ErrorCode::cancelled, message};
    return {ErrorCode::runtime_failure, message};
}
Json status_json(Status status) {
    std::string name = "none";
    switch (status.code) {
    case ErrorCode::none:
        break;
    case ErrorCode::invalid_input:
        name = "invalid_input";
        break;
    case ErrorCode::unsupported:
        name = "unsupported";
        break;
    case ErrorCode::invalid_state:
        name = "invalid_state";
        break;
    case ErrorCode::resource_exhausted:
        name = "resource_exhausted";
        break;
    case ErrorCode::deadline_expired:
        name = "deadline_expired";
        break;
    case ErrorCode::runtime_failure:
        name = "runtime_failure";
        break;
    case ErrorCode::cancelled:
        name = "cancelled";
        break;
    }
    return {{"code", name}, {"message", status.message}};
}
RecognitionEvent parse_event(const Json &value) {
    RecognitionEvent event;
    event.schema_version = value.value("schema_version", 1);
    event.run_id = value.value("run_id", "");
    event.call_id = value.value("call_id", "");
    event.producer_id = value.value("producer_id", "");
    event.worker_id = value.value("worker_id", "");
    event.sequence = value.value("sequence", 0ULL);
    event.revision = value.value("revision", 0ULL);
    event.produced_ns = value.value("produced_ns", 0LL);
    event.consumed_samples = value.value("consumed_samples", 0LL);
    event.clock_domain = value.value("clock_domain", "");
    event.before_eof = value.value("before_eof", false);
    event.text = value.value("text", "");
    const auto kind = value.value("kind", "partial");
    event.kind = kind == "final"     ? EventKind::final
                 : kind == "stopped" ? EventKind::stopped
                 : kind == "failed"  ? EventKind::failed
                                     : EventKind::partial;
    event.status = status_from_json(value.value("error", Json::object()));
    if (value.contains("utc") && !value["utc"].is_null())
        event.utc = value["utc"].get<std::string>();
    return event;
}
RuntimeObservation parse_observation(const Json &value) {
    RuntimeObservation result;
    result.stage = value.value("stage", "");
    result.worker_id = value.value("worker_id", "");
    result.timestamp_ns = value.value("timestamp_ns", 0LL);
    result.process_id = value.value("process_id", 0);
    const auto optional = [&](const char *key, auto &target) {
        if (value.contains(key) && !value[key].is_null())
            target = value[key].get<std::int64_t>();
    };
    optional("duration_ns", result.duration_ns);
    optional("counter_value", result.counter_value);
    optional("sequence", result.sequence);
    optional("buffered_samples", result.buffered_samples);
    optional("cpu_ns", result.cpu_ns);
    optional("peak_rss_bytes", result.peak_rss_bytes);
    return result;
}
class ServerSink final : public IRecognitionSink {
    int fd_;
    std::mutex mutex_;
    ObservationHub &hub_;
    std::shared_ptr<ObservationHub::Feed> feed_;

  public:
    ServerSink(int fd, ObservationHub &hub, std::shared_ptr<ObservationHub::Feed> feed)
        : fd_(fd), hub_(hub), feed_(std::move(feed)) {}
    void send(const Json &value) {
        std::lock_guard lock(mutex_);
        write_text(fd_, value, false);
    }
    void on_event(const RecognitionEvent &event) override {
        Json message = {{"v", 1}, {"type", "event"}, {"event", event_json(event)}};
        hub_.publish(feed_, message);
        send(message);
    }
    void on_observation(const RuntimeObservation &observation) override {
        Json message = {{"v", 1}, {"type", "observation"}, {"observation", observation_json(observation)}};
        hub_.publish(feed_, message);
        send(message);
    }
};
class RemoteSession final : public IASRSession {
    int fd_;
    IRecognitionSink &sink_;
    std::thread reader_;
    mutable std::mutex mutex_;
    std::mutex write_mutex_;
    std::condition_variable wake_;
    SessionSnapshot snapshot_;
    std::uint64_t ack_sequence_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t next_sequence_ = 0;
    std::int64_t next_sample_ = 0;
    Status ack_status_, done_status_;
    bool done_ = false, broken_ = false;
    std::string failure_;
    void read_loop() {
        try {
            while (true) {
                const auto frame = read_json_frame(fd_, false);
                if (frame.value("v", 0) != 1)
                    throw std::runtime_error("WebSocket protocol version mismatch");
                const auto type = frame.value("type", "");
                if (type == "event") {
                    auto event = parse_event(frame.at("event"));
                    {
                        std::lock_guard lock(mutex_);
                        snapshot_.worker_id = event.worker_id;
                        snapshot_.text = event.text;
                        snapshot_.revision = event.revision;
                        snapshot_.consumed_samples = event.consumed_samples;
                        if (event.kind == EventKind::final)
                            snapshot_.state = SessionState::completed;
                        else if (event.kind == EventKind::failed)
                            snapshot_.state = SessionState::failed;
                        else if (event.kind == EventKind::stopped)
                            snapshot_.state = SessionState::stopped;
                        else
                            snapshot_.state = SessionState::streaming;
                    }
                    sink_.on_event(event);
                } else if (type == "observation")
                    sink_.on_observation(parse_observation(frame.at("observation")));
                else if (type == "ack") {
                    std::lock_guard lock(mutex_);
                    ack_sequence_ = frame.at("sequence").get<std::uint64_t>();
                    ack_status_ = status_from_json(frame.at("status"));
                    if (frame.value("credits", 0) != 1 && ack_status_)
                        throw std::runtime_error("invalid WebSocket flow-control credit");
                    wake_.notify_all();
                } else if (type == "done") {
                    std::lock_guard lock(mutex_);
                    done_status_ = status_from_json(frame.at("status"));
                    done_ = true;
                    wake_.notify_all();
                    return;
                } else
                    throw std::runtime_error("unknown WebSocket message type");
            }
        } catch (const std::exception &error) {
            std::lock_guard lock(mutex_);
            broken_ = true;
            failure_ = error.what();
            if (snapshot_.state != SessionState::completed && snapshot_.state != SessionState::stopped)
                snapshot_.state = SessionState::failed;
            wake_.notify_all();
        }
    }
    Status send_control(const Json &value) {
        try {
            std::lock_guard lock(write_mutex_);
            write_text(fd_, value, true);
            return {};
        } catch (const std::exception &error) {
            return {ErrorCode::runtime_failure, error.what()};
        }
    }

  public:
    RemoteSession(int fd, IRecognitionSink &sink, std::string worker_id) : fd_(fd), sink_(sink) {
        snapshot_.state = SessionState::ready;
        snapshot_.worker_id = std::move(worker_id);
        reader_ = std::thread([this] { read_loop(); });
    }
    ~RemoteSession() override {
        ::shutdown(fd_, SHUT_RDWR);
        if (reader_.joinable())
            reader_.join();
        ::close(fd_);
    }
    Status submit(AudioChunk chunk) override {
        if (!chunk.pcm || chunk.pcm->size() > 32000)
            return {ErrorCode::invalid_input, "invalid WebSocket audio chunk"};
        {
            std::lock_guard lock(mutex_);
            if (done_ || broken_)
                return {ErrorCode::invalid_state, "network session terminal"};
        }
        try {
            std::lock_guard lock(write_mutex_);
            write_text(fd_,
                       {{"v", 1},
                        {"type", "chunk"},
                        {"sequence", chunk.sequence},
                        {"first_sample", chunk.first_sample},
                        {"sample_count", chunk.pcm->size()},
                        {"scheduled_ready_ns", chunk.scheduled_ready_ns},
                        {"sent_ns", chunk.sent_ns}},
                       true);
            std::vector<std::uint8_t> pcm(chunk.pcm->size() * 2);
            for (std::size_t i = 0; i < chunk.pcm->size(); ++i) {
                const auto sample = std::uint16_t((*chunk.pcm)[i]);
                pcm[2 * i] = std::uint8_t(sample);
                pcm[2 * i + 1] = std::uint8_t(sample >> 8);
            }
            write_frame(fd_, 2, pcm, true);
        } catch (const std::exception &error) {
            return {ErrorCode::runtime_failure, error.what()};
        }
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return ack_sequence_ == chunk.sequence || broken_ || done_; });
        if (broken_)
            return {ErrorCode::runtime_failure, failure_};
        if (ack_sequence_ != chunk.sequence)
            return {ErrorCode::runtime_failure, "missing chunk acknowledgment"};
        if (ack_status_) {
            next_sequence_ = chunk.sequence + 1;
            next_sample_ = chunk.first_sample + std::int64_t(chunk.pcm->size());
        }
        return ack_status_;
    }
    Status finish_input() override {
        std::uint64_t sequence;
        std::int64_t samples;
        {
            std::lock_guard lock(mutex_);
            sequence = next_sequence_;
            samples = next_sample_;
        }
        const auto sent = send_control(
            {{"v", 1}, {"type", "eof"}, {"expected_next_sequence", sequence}, {"total_samples", samples}});
        if (!sent)
            return sent;
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return done_ || broken_; });
        return broken_ ? Status{ErrorCode::runtime_failure, failure_} : done_status_;
    }
    Status cancel(CancelReason) override {
        const auto sent = send_control({{"v", 1}, {"type", "cancel"}});
        if (!sent)
            return sent;
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return done_ || broken_; });
        return broken_ ? Status{ErrorCode::runtime_failure, failure_} : done_status_;
    }
    SessionSnapshot snapshot() const override {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }
};
} // namespace

WebSocketServer::WebSocketServer(IASREngine &engine, std::uint16_t port, IHttpHandler *http,
                                 ServiceAdmissionGate *gate)
    : engine_(engine), http_(http), gate_(gate), observations_(std::make_unique<ObservationHub>()) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0)
        throw std::runtime_error("cannot open WebSocket listener");
    int reuse = 1;
    (void)setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(listener_, 16) != 0) {
        ::close(listener_);
        throw std::runtime_error("cannot bind WebSocket loopback listener");
    }
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
        ::close(listener_);
        throw std::runtime_error("cannot inspect WebSocket listener");
    }
    port_ = ntohs(address.sin_port);
    accept_thread_ = std::thread([this] { accept_loop(); });
}
WebSocketServer::~WebSocketServer() {
    stopping_ = true;
    ::shutdown(listener_, SHUT_RDWR);
    ::close(listener_);
    if (accept_thread_.joinable())
        accept_thread_.join();
    {
        std::lock_guard lock(mutex_);
        for (const auto fd : clients_)
            ::shutdown(fd, SHUT_RDWR);
    }
    for (auto &handler : handlers_)
        if (handler.thread.joinable())
            handler.thread.join();
}
void WebSocketServer::accept_loop() {
    while (!stopping_) {
        const auto fd = ::accept(listener_, nullptr, nullptr);
        if (fd < 0) {
            if (stopping_)
                break;
            continue;
        }
        timeval timeout{65, 0};
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::lock_guard lock(mutex_);
        // A long load suite may open thousands of short-lived sessions. Reap
        // finished pthreads on each accept instead of retaining their stacks
        // until the server itself is destroyed.
        for (auto it = handlers_.begin(); it != handlers_.end();) {
            if (it->finished->load()) {
                if (it->thread.joinable())
                    it->thread.join();
                it = handlers_.erase(it);
            } else {
                ++it;
            }
        }
        clients_.push_back(fd);
        auto finished = std::make_shared<std::atomic<bool>>(false);
        handlers_.push_back({std::thread([this, fd, finished] {
                                 try {
                                     handle_client(fd);
                                 } catch (...) {
                                 }
                                 {
                                     std::lock_guard lock(mutex_);
                                     std::erase(clients_, fd);
                                 }
                                 ::shutdown(fd, SHUT_RDWR);
                                 ::close(fd);
                                 finished->store(true);
                             }),
                             finished});
    }
}
void WebSocketServer::handle_observer(int fd, const std::string &request) {
    const auto end = request.find(" HTTP/1.1\r\n");
    if (end == std::string::npos || !request.starts_with("GET /v1/observe?"))
        throw std::runtime_error("invalid observer request");
    constexpr auto prefix_size = sizeof("GET /v1/observe?") - 1;
    const auto target = request.substr(prefix_size, end - prefix_size);
    std::map<std::string, std::string> query;
    std::size_t begin = 0;
    while (begin < target.size()) {
        const auto end_pair = target.find('&', begin);
        const auto pair =
            target.substr(begin, end_pair == std::string::npos ? std::string::npos : end_pair - begin);
        const auto equal = pair.find('=');
        if (equal == std::string::npos ||
            !query.emplace(pair.substr(0, equal), pair.substr(equal + 1)).second)
            throw std::runtime_error("invalid observer query");
        if (end_pair == std::string::npos)
            break;
        begin = end_pair + 1;
    }
    const auto safe_id = [](const std::string &value) {
        return !value.empty() && value.size() <= 128 &&
               std::all_of(value.begin(), value.end(),
                           [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
    };
    const auto run_id = query["run_id"], call_id = query["call_id"];
    if (!safe_id(run_id) || !safe_id(call_id) || query.size() > 3 ||
        !allowed_origin(header_value(request, "origin"), port_) ||
        header_value(request, "sec-websocket-version") != "13")
        throw std::runtime_error("observer request rejected");
    std::uint64_t cursor = 0;
    if (query.contains("since")) {
        std::size_t used = 0;
        cursor = std::stoull(query["since"], &used);
        if (used != query["since"].size() || cursor == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("invalid observer cursor");
    }
    const auto key = header_value(request, "sec-websocket-key");
    if (key.size() < 16 || key.size() > 64)
        throw std::runtime_error("invalid observer WebSocket key");
    auto feed = observations_->get(run_id + "/" + call_id);
    const auto response = std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n") +
                          "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept_key(key) + "\r\n\r\n";
    send_all(fd, response.data(), response.size());
    timeval timeout{2, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    while (!stopping_) {
        std::vector<Json> batch;
        bool terminal;
        {
            std::unique_lock lock(feed->mutex);
            feed->changed.wait_for(lock, std::chrono::milliseconds(250),
                                   [&] { return feed->next > cursor + 1 || feed->terminal || stopping_; });
            if (stopping_)
                break;
            if (!feed->messages.empty()) {
                const auto oldest = feed->messages.front()["delivery_sequence"].get<std::uint64_t>();
                if (oldest > cursor + 1)
                    batch.push_back({{"v", 1},
                                     {"type", "gap"},
                                     {"first_available_sequence", oldest},
                                     {"dropped_total", feed->dropped}});
                for (const auto &message : feed->messages)
                    if (message["delivery_sequence"].get<std::uint64_t>() > cursor)
                        batch.push_back(message);
            }
            terminal = feed->terminal;
        }
        for (const auto &message : batch) {
            write_text(fd, message, false);
            if (message.contains("delivery_sequence"))
                cursor = message["delivery_sequence"].get<std::uint64_t>();
        }
        if (terminal) {
            write_text(fd, {{"v", 1}, {"type", "observer_done"}, {"last_sequence", cursor}}, false);
            return;
        }
    }
}
void WebSocketServer::handle_client(int fd) {
    const auto request = read_header(fd);
    if (request.starts_with("GET /v1/observe?")) {
        handle_observer(fd, request);
        return;
    }
    if (header_value(request, "upgrade") != "websocket") {
        if (!http_)
            throw std::runtime_error("HTTP API not enabled");
        const auto first_line_end = request.find("\r\n");
        const auto first_line = request.substr(0, first_line_end);
        const auto first_space = first_line.find(' ');
        const auto last_space = first_line.rfind(' ');
        HttpResponse response;
        try {
            if (first_space == std::string::npos || last_space == first_space ||
                first_line.substr(last_space + 1) != "HTTP/1.1")
                throw std::invalid_argument("invalid HTTP request line");
            HttpRequest input{first_line.substr(0, first_space),
                              first_line.substr(first_space + 1, last_space - first_space - 1),
                              header_value(request, "origin"), ""};
            if (!input.origin.empty() && !allowed_origin(input.origin, port_))
                throw std::invalid_argument("origin is not allowed");
            if (input.method == "OPTIONS") {
                if (header_value(request, "access-control-request-headers") != "content-type")
                    throw std::invalid_argument("unsupported CORS request headers");
                response = {204, "application/json", ""};
            } else {
                if (!header_value(request, "transfer-encoding").empty())
                    throw std::invalid_argument("chunked HTTP requests are unsupported");
                const auto length_text = header_value(request, "content-length");
                if (!length_text.empty()) {
                    std::size_t used = 0;
                    const auto length = std::stoull(length_text, &used);
                    if (used != length_text.size() || length > 65536)
                        throw std::invalid_argument("HTTP body exceeds 64 KiB");
                    input.body.resize(length);
                    if (length)
                        recv_all(fd, input.body.data(), length);
                } else if (input.method == "POST")
                    throw std::invalid_argument("POST requires Content-Length");
                response = http_->handle(input);
            }
        } catch (const std::invalid_argument &error) {
            response = {400, "application/json", Json({{"error", error.what()}}).dump()};
        } catch (const std::exception &) {
            response = {500, "application/json", R"({"error":"internal server error"})"};
        }
        const auto reason = response.status == 200   ? "OK"
                            : response.status == 202 ? "Accepted"
                            : response.status == 204 ? "No Content"
                            : response.status == 400 ? "Bad Request"
                            : response.status == 404 ? "Not Found"
                            : response.status == 409 ? "Conflict"
                            : response.status == 413 ? "Payload Too Large"
                            : response.status == 429 ? "Too Many Requests"
                                                     : "Internal Server Error";
        const auto origin = header_value(request, "origin");
        const auto cors = allowed_origin(origin, port_)
                              ? "Access-Control-Allow-Origin: " + origin +
                                    "\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                                    "Access-Control-Allow-Headers: Content-Type\r\nVary: Origin\r\n"
                              : std::string{};
        const auto header =
            "HTTP/1.1 " + std::to_string(response.status) + " " + reason +
            "\r\nContent-Type: " + response.content_type +
            "\r\nContent-Length: " + std::to_string(response.body.size()) + "\r\n" + cors +
            "Connection: close\r\nX-Content-Type-Options: nosniff\r\nCache-Control: no-store\r\n\r\n";
        send_all(fd, header.data(), header.size());
        send_all(fd, response.body.data(), response.body.size());
        return;
    }
    if (!request.starts_with("GET /v1/asr HTTP/1.1\r\n") || header_value(request, "upgrade") != "websocket" ||
        header_value(request, "sec-websocket-version") != "13" ||
        !allowed_origin(header_value(request, "origin"), port_))
        throw std::runtime_error("invalid WebSocket upgrade request");
    const auto key = header_value(request, "sec-websocket-key");
    if (key.size() < 16 || key.size() > 64)
        throw std::runtime_error("invalid WebSocket key");
    const auto response = std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n") +
                          "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept_key(key) + "\r\n\r\n";
    send_all(fd, response.data(), response.size());
    const auto start = read_json_frame(fd, true);
    if (start.value("v", 0) != 1 || start.value("type", "") != "start")
        throw std::runtime_error("expected v1 start frame");
    SessionConfig config;
    config.run_id = start.at("run_id").get<std::string>();
    config.call_id = start.at("call_id").get<std::string>();
    config.language = start.at("language").get<std::string>();
    config.seed = start.at("seed").get<std::uint32_t>();
    config.max_chunk_samples = start.at("max_chunk_samples").get<std::int64_t>();
    config.partial_every_ms = start.at("partial_every_ms").get<int>();
    config.sample_rate_hz = start.at("sample_rate_hz").get<int>();
    config.decode_step_ms = start.value("decode_step_ms", 0);
    config.prefix_preview_ms = start.value("prefix_preview_ms", 0);
    const int minimum_step_ms =
        engine_.capabilities().streaming_kind == "resumable_windowed_streaming" ? 500 : 1000;
    if ((config.decode_step_ms != 0 &&
         (config.decode_step_ms < minimum_step_ms || config.decode_step_ms > 8000)) ||
        (config.prefix_preview_ms != 0 &&
         (config.prefix_preview_ms < 1000 || config.prefix_preview_ms > 20000)))
        throw std::invalid_argument("invalid decode step or prefix preview interval");
    const auto safe_identity = [](const std::string &value) {
        return !value.empty() && value.size() <= 128 &&
               std::all_of(value.begin(), value.end(),
                           [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
    };
    if (config.run_id.size() > 128 || config.call_id.size() > 128 || config.max_chunk_samples < 1 ||
        config.max_chunk_samples > 32000 || config.sample_rate_hz != 16000 || !safe_identity(config.run_id) ||
        !safe_identity(config.call_id))
        throw std::runtime_error("invalid WebSocket session configuration");
    if (gate_ && !gate_->enter_live()) {
        write_text(fd,
                   {{"v", 1},
                    {"type", "ready"},
                    {"status", status_json({ErrorCode::resource_exhausted,
                                            "benchmark suite has exclusive service admission"})},
                    {"worker_id", ""},
                    {"credits", 0}},
                   false);
        return;
    }
    struct LiveGuard {
        ServiceAdmissionGate *gate;
        ~LiveGuard() {
            if (gate)
                gate->leave_live();
        }
    } live_guard{gate_};
    const auto feed = observations_->get(config.run_id + "/" + config.call_id);
    if (!observations_->activate(feed))
        throw std::runtime_error("duplicate WebSocket call identity");
    struct CloseFeed {
        ObservationHub &hub;
        std::shared_ptr<ObservationHub::Feed> feed;
        ~CloseFeed() { hub.close(feed); }
    } close_feed{*observations_, feed};
    SteadyClock clock;
    ServerSink sink(fd, *observations_, feed);
    auto created = engine_.create_session(config, sink, clock);
    if (!created || !created.value) {
        sink.send({{"v", 1},
                   {"type", "ready"},
                   {"status", status_json(created.status)},
                   {"worker_id", ""},
                   {"credits", 0}});
        return;
    }
    auto session = std::move(created.value);
    sink.send({{"v", 1},
               {"type", "ready"},
               {"status", status_json({})},
               {"worker_id", session->snapshot().worker_id},
               {"credits", 1}});
    std::uint64_t next_sequence = 0;
    std::int64_t next_sample = 0;
    while (true) {
        const auto message = read_json_frame(fd, true);
        if (message.value("v", 0) != 1)
            throw std::runtime_error("WebSocket protocol version mismatch");
        const auto type = message.value("type", "");
        if (type == "chunk") {
            const auto sequence = message.at("sequence").get<std::uint64_t>();
            const auto first = message.at("first_sample").get<std::int64_t>();
            const auto count = message.at("sample_count").get<std::size_t>();
            if (sequence != next_sequence || first != next_sample || count == 0 ||
                count > std::size_t(config.max_chunk_samples))
                throw std::runtime_error("WebSocket audio sequence/count invalid");
            const auto frame = read_frame(fd, true);
            if (frame.opcode != 2 || frame.payload.size() != count * 2)
                throw std::runtime_error("WebSocket PCM frame length invalid");
            auto pcm = std::make_shared<std::vector<std::int16_t>>(count);
            for (std::size_t i = 0; i < count; ++i)
                (*pcm)[i] = std::int16_t(std::uint16_t(frame.payload[2 * i]) |
                                         (std::uint16_t(frame.payload[2 * i + 1]) << 8));
            AudioChunk chunk{config.run_id,
                             config.call_id,
                             sequence,
                             first,
                             16000,
                             1,
                             message.value("scheduled_ready_ns", 0LL),
                             message.value("sent_ns", 0LL),
                             pcm};
            const auto status = session->submit(std::move(chunk));
            sink.send({{"v", 1},
                       {"type", "ack"},
                       {"sequence", sequence},
                       {"status", status_json(status)},
                       {"credits", status ? 1 : 0}});
            if (!status)
                break;
            ++next_sequence;
            next_sample += count;
        } else if (type == "eof" || type == "cancel") {
            if (type == "eof" && (message.value("expected_next_sequence",
                                                std::numeric_limits<std::uint64_t>::max()) != next_sequence ||
                                  message.value("total_samples", std::int64_t{-1}) != next_sample))
                throw std::runtime_error("WebSocket EOF sequence/sample mismatch");
            const auto status =
                type == "eof" ? session->finish_input() : session->cancel(CancelReason::user_request);
            sink.send({{"v", 1}, {"type", "done"}, {"status", status_json(status)}});
            return;
        } else
            throw std::runtime_error("unknown WebSocket control frame");
    }
}
WebSocketEngine::WebSocketEngine(std::uint16_t port, EngineCapabilities capabilities)
    : port_(port), capabilities_(std::move(capabilities)) {
    if (!port_)
        throw std::invalid_argument("WebSocket engine requires a listening port");
}
Result<std::unique_ptr<IASRSession>> WebSocketEngine::create_session(const SessionConfig &config,
                                                                     IRecognitionSink &sink, IClock &clock) {
    if (clock.domain() != "host_steady")
        return {{ErrorCode::unsupported, "WebSocket load requires host steady clock"}, nullptr};
    const auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return {{ErrorCode::runtime_failure, "cannot create WebSocket client socket"}, nullptr};
    try {
        timeval timeout{65, 0};
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port_);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
            throw std::runtime_error("cannot connect WebSocket loopback server");
        std::array<unsigned char, 16> random_bytes{};
        std::random_device random;
        for (auto &byte : random_bytes)
            byte = std::uint8_t(random());
        const auto key = base64(random_bytes.data(), random_bytes.size());
        const auto request = std::string("GET /v1/asr HTTP/1.1\r\nHost: 127.0.0.1\r\n") +
                             "Upgrade: websocket\r\nConnection: Upgrade\r\n" +
                             "Sec-WebSocket-Version: 13\r\nOrigin: http://127.0.0.1\r\n" +
                             "Sec-WebSocket-Key: " + key + "\r\n\r\n";
        send_all(fd, request.data(), request.size());
        const auto response = read_header(fd);
        if (!response.starts_with("HTTP/1.1 101 ") ||
            header_value(response, "sec-websocket-accept") != accept_key(key))
            throw std::runtime_error("WebSocket upgrade rejected");
        write_text(fd,
                   {{"v", 1},
                    {"type", "start"},
                    {"run_id", config.run_id},
                    {"call_id", config.call_id},
                    {"language", config.language},
                    {"seed", config.seed},
                    {"max_chunk_samples", config.max_chunk_samples},
                    {"partial_every_ms", config.partial_every_ms},
                    {"decode_step_ms", config.decode_step_ms},
                    {"prefix_preview_ms", config.prefix_preview_ms},
                    {"sample_rate_hz", config.sample_rate_hz}},
                   true);
        while (true) {
            const auto frame = read_json_frame(fd, false);
            if (frame.value("v", 0) != 1)
                throw std::runtime_error("WebSocket protocol version mismatch");
            const auto type = frame.value("type", "");
            if (type == "observation")
                sink.on_observation(parse_observation(frame.at("observation")));
            else if (type == "event")
                sink.on_event(parse_event(frame.at("event")));
            else if (type == "ready") {
                const auto status = status_from_json(frame.at("status"));
                if (!status) {
                    ::close(fd);
                    return {status, nullptr};
                }
                if (frame.value("credits", 0) != 1)
                    throw std::runtime_error("WebSocket ready did not grant one credit");
                return {{}, std::make_unique<RemoteSession>(fd, sink, frame.value("worker_id", ""))};
            } else
                throw std::runtime_error("unexpected WebSocket startup frame");
        }
    } catch (const std::exception &error) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        return {{ErrorCode::runtime_failure, error.what()}, nullptr};
    }
}
} // namespace asr
