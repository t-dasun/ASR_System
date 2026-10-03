#include "wire.hpp"
extern "C" {
#include "qwen_asr.h"
#include "qwen_asr_kernels.h"
}
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Json = nlohmann::json;
bool write_all(const std::string &message) {
    const char *data = message.data();
    std::size_t remaining = message.size();
    while (remaining) {
        const auto count = ::write(STDOUT_FILENO, data, remaining);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        data += count;
        remaining -= count;
    }
    return true;
}
std::mutex output_mutex;
void output(const Json &value) {
    std::lock_guard lock(output_mutex);
    if (!write_all(value.dump() + "\n"))
        throw std::runtime_error("worker telemetry write failed");
}
bool read_exact(void *destination, std::size_t count) {
    auto *data = static_cast<char *>(destination);
    while (count) {
        const auto received = ::read(3, data, count);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0)
            return false;
        data += received;
        count -= received;
    }
    return true;
}
std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
bool complete_utf8(const std::string &text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        int length = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!length || i + length > text.size())
            return false;
        for (int j = 1; j < length; ++j)
            if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80)
                return false;
        const auto second = static_cast<unsigned char>(text[i + 1]);
        if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second > 0x9f) || (c == 0xf0 && second < 0x90) ||
            (c == 0xf4 && second > 0x8f))
            return false;
        i += length;
    }
    return true;
}
int parse(const char *text, int minimum, int maximum) {
    int value = 0;
    const auto end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, value);
    if (result.ec != std::errc{} || result.ptr != end || value < minimum || value > maximum)
        throw std::invalid_argument("invalid worker integer argument");
    return value;
}
struct LiveBuffer {
    qwen_live_audio_t value{};
    explicit LiveBuffer(std::int64_t capacity) {
        value.capacity = capacity;
        value.samples = static_cast<float *>(std::malloc(static_cast<std::size_t>(capacity) * sizeof(float)));
        if (!value.samples)
            throw std::runtime_error("live buffer allocation failed");
        if (pthread_mutex_init(&value.mutex, nullptr) != 0) {
            std::free(value.samples);
            throw std::runtime_error("live mutex init failed");
        }
        if (pthread_cond_init(&value.cond, nullptr) != 0) {
            pthread_mutex_destroy(&value.mutex);
            std::free(value.samples);
            throw std::runtime_error("live condition init failed");
        }
    }
    ~LiveBuffer() {
        pthread_cond_destroy(&value.cond);
        pthread_mutex_destroy(&value.mutex);
        std::free(value.samples);
    }
};
struct LiveLock {
    pthread_mutex_t &mutex;
    explicit LiveLock(pthread_mutex_t &value) : mutex(value) { pthread_mutex_lock(&mutex); }
    ~LiveLock() { pthread_mutex_unlock(&mutex); }
};
struct Context {
    LiveBuffer &live;
    std::vector<float> full_audio;
    std::atomic<std::int64_t> delivered{0};
    std::atomic<bool> eof{false};
    std::string error, text, pending;
    bool all_zero = true, invalid_utf8 = false;
    explicit Context(LiveBuffer &buffer) : live(buffer) {}
    void fail(std::string message) {
        error = std::move(message);
        eof = true;
        pthread_mutex_lock(&live.value.mutex);
        live.value.eof = 1;
        pthread_cond_signal(&live.value.cond);
        pthread_mutex_unlock(&live.value.mutex);
    }
};
void token_callback(const char *piece, void *opaque) {
    auto &context = *static_cast<Context *>(opaque);
    if (!piece || context.invalid_utf8)
        return;
    context.pending += piece;
    if (!complete_utf8(context.pending)) {
        if (context.pending.size() > 32)
            context.invalid_utf8 = true;
        return;
    }
    if (context.pending.empty())
        return;
    context.text += context.pending;
    context.pending.clear();
    output({{"type", "partial"},
            {"text", context.text},
            {"produced_ns", now_ns()},
            {"consumed_samples", context.delivered.load()},
            {"before_eof", !context.eof.load()}});
}
void read_audio(Context &context) {
    std::uint64_t next_sequence = 0;
    std::int64_t next_sample = 0;
    while (true) {
        asr::native_wire::Header header;
        if (!read_exact(&header, sizeof(header))) {
            context.fail("worker input closed before EOF");
            return;
        }
        if (header.protocol != asr::native_wire::magic || header.reserved ||
            header.sequence != next_sequence || header.first_sample != next_sample) {
            context.fail("invalid worker frame sequence or protocol");
            return;
        }
        if (header.type == asr::native_wire::eof) {
            if (header.sample_count) {
                context.fail("EOF carried audio");
                return;
            }
            const auto eof_received = now_ns();
            context.eof = true;
            pthread_mutex_lock(&context.live.value.mutex);
            context.live.value.eof = 1;
            pthread_cond_signal(&context.live.value.cond);
            pthread_mutex_unlock(&context.live.value.mutex);
            output({{"type", "timing"},
                    {"stage", "worker_eof_received"},
                    {"timestamp_ns", eof_received},
                    {"process_id", getpid()}});
            return;
        }
        if (header.type != asr::native_wire::audio || header.sample_count < 1 ||
            header.sample_count > 16000 || next_sample + header.sample_count > 16000 * 600) {
            context.fail("invalid worker audio size or type");
            return;
        }
        std::vector<std::int16_t> pcm(header.sample_count);
        if (!read_exact(pcm.data(), pcm.size() * sizeof(std::int16_t))) {
            context.fail("truncated worker PCM");
            return;
        }
        const auto received_ns = now_ns();
        bool overflow = false;
        std::int64_t enqueued_ns = 0, buffered_samples = 0;
        {
            LiveLock lock(context.live.value.mutex);
            overflow = context.live.value.n_samples + header.sample_count > context.live.value.capacity;
            if (!overflow) {
                for (std::size_t i = 0; i < pcm.size(); ++i) {
                    if (pcm[i])
                        context.all_zero = false;
                    const auto sample = static_cast<float>(pcm[i]) / 32768.0F;
                    context.live.value.samples[context.live.value.n_samples + i] = sample;
                    context.full_audio.push_back(sample);
                }
                context.live.value.n_samples += header.sample_count;
                next_sample += header.sample_count;
                ++next_sequence;
                context.delivered = next_sample;
                enqueued_ns = now_ns();
                buffered_samples = context.live.value.n_samples;
                pthread_cond_signal(&context.live.value.cond);
            }
        }
        if (overflow) {
            context.fail("native staging buffer overflow");
            return;
        }
        output({{"type", "timing"},
                {"stage", "worker_received"},
                {"timestamp_ns", received_ns},
                {"sequence", header.sequence},
                {"process_id", getpid()}});
        output({{"type", "timing"},
                {"stage", "runtime_enqueued"},
                {"timestamp_ns", enqueued_ns},
                {"sequence", header.sequence},
                {"buffered_samples", buffered_samples},
                {"process_id", getpid()}});
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 9)
            throw std::invalid_argument("worker requires model, language, threads, step, tokens, refine, "
                                        "staging_seconds, parent_pid");
        const int parent_pid = parse(argv[8], 1, INT_MAX);
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent_pid)
            throw std::runtime_error("worker parent ownership changed");
        const int threads = parse(argv[3], 1, 16);
        const int step = parse(argv[4], 1000, 8000);
        const int tokens = parse(argv[5], 1, 256);
        const bool refine = parse(argv[6], 0, 1) != 0;
        const int staging = parse(argv[7], 1, 20);
        setenv("OMP_NUM_THREADS", argv[3], 1);
        setenv("OPENBLAS_NUM_THREADS", argv[3], 1);
        setenv("OMP_DYNAMIC", "FALSE", 1);
        setenv("QWEN_BF16_CACHE_MB", "0", 1);
        qwen_verbose = 0;
        qwen_set_threads(threads);
        const auto load_started = now_ns();
        std::unique_ptr<qwen_ctx_t, decltype(&qwen_free)> model(qwen_load(argv[1]), qwen_free);
        if (!model)
            throw std::runtime_error("native model loading failed");
        output({{"type", "timing"},
                {"stage", "model_load"},
                {"timestamp_ns", now_ns()},
                {"duration_ns", now_ns() - load_started},
                {"process_id", getpid()}});
        if (qwen_set_force_language(model.get(), argv[2]) != 0)
            throw std::invalid_argument("unsupported native language");
        model->past_text_conditioning = 1;
        model->stream_chunk_sec = static_cast<float>(step) / 1000.0F;
        model->stream_max_new_tokens = tokens;
        LiveBuffer live(16000 * staging);
        Context context(live);
        qwen_set_token_callback(model.get(), token_callback, &context);
        std::thread reader([&] {
            try {
                read_audio(context);
            } catch (const std::exception &error) {
                context.fail(error.what());
            }
        });
        output({{"type", "ready"}, {"produced_ns", now_ns()}});
        const auto live_started = now_ns();
        std::unique_ptr<char, decltype(&std::free)> stream(
            qwen_transcribe_stream_live(model.get(), &live.value), std::free);
        const auto live_finished = now_ns();
        reader.join();
        output({{"type", "timing"},
                {"stage", "live_invocation"},
                {"timestamp_ns", live_finished},
                {"duration_ns", live_finished - live_started},
                {"process_id", getpid()}});
        if (!context.error.empty() || !stream || context.invalid_utf8 || !context.pending.empty())
            throw std::runtime_error(context.error.empty() ? "native stream or UTF-8 callback failed"
                                                           : context.error);
        std::string final_text = stream.get();
        if (refine && !context.full_audio.empty()) {
            qwen_set_token_callback(model.get(), nullptr, nullptr);
            const auto refine_started = now_ns();
            std::unique_ptr<char, decltype(&std::free)> refined(
                qwen_transcribe_audio(model.get(), context.full_audio.data(),
                                      static_cast<int>(context.full_audio.size())),
                std::free);
            if (!refined)
                throw std::runtime_error("native EOF refinement failed");
            final_text = refined.get();
            output({{"type", "timing"},
                    {"stage", "eof_refinement"},
                    {"timestamp_ns", now_ns()},
                    {"duration_ns", now_ns() - refine_started},
                    {"process_id", getpid()}});
        }
        if (context.all_zero)
            final_text.clear(); // Exact digital silence only; quiet speech is preserved.
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) == 0)
            output({{"type", "timing"},
                    {"stage", "worker_usage"},
                    {"timestamp_ns", now_ns()},
                    {"process_id", getpid()},
                    {"peak_rss_bytes", usage.ru_maxrss * 1024LL},
                    {"cpu_ns", (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000000LL +
                                   (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1000LL}});
        output({{"type", "final"},
                {"text", final_text},
                {"produced_ns", now_ns()},
                {"consumed_samples", context.delivered.load()},
                {"silence_policy", "exact_digital_zero"},
                {"refined", refine}});
        return 0;
    } catch (const std::exception &error) {
        output({{"type", "failed"}, {"message", error.what()}, {"produced_ns", now_ns()}});
        return 1;
    }
}
