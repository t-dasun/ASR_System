// M0 diagnostic only. Production engine/session integration belongs to M3.
#include <asr/core/chunk_schedule.hpp>
extern "C" {
#include "qwen_asr.h"
#include "qwen_asr_audio.h"
#include "qwen_asr_kernels.h"
}
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
struct Event { double milliseconds; std::int64_t delivered_samples; bool before_eof; std::string text; };
struct State {
    Clock::time_point start;
    std::int64_t total_samples = 0;
    std::atomic<std::int64_t> delivered{0};
    std::atomic<bool> eof{false};
    std::vector<Event> events; // only the inference thread writes this
};
static double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
static void token(const char* text, void* opaque) {
    auto& state = *static_cast<State*>(opaque);
    const auto delivered = state.delivered.load();
    state.events.push_back({elapsed(state.start), delivered,
                           !state.eof.load() && delivered < state.total_samples, text});
}
static bool complete_utf8(const std::string& value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c < 0x80) { ++i; continue; }
        int length = 0;
        if (c >= 0xc2 && c <= 0xdf) length = 2;
        else if (c >= 0xe0 && c <= 0xef) length = 3;
        else if (c >= 0xf0 && c <= 0xf4) length = 4;
        else return false;
        if (i + length > value.size()) return false;
        for (int j = 1; j < length; ++j) {
            if ((static_cast<unsigned char>(value[i + j]) & 0xc0) != 0x80) return false;
        }
        const auto second = static_cast<unsigned char>(value[i + 1]);
        if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second > 0x9f) ||
            (c == 0xf0 && second < 0x90) || (c == 0xf4 && second > 0x8f)) return false;
        i += length;
    }
    return true;
}
static std::string quote(const std::string& value) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    const bool utf8_ok = complete_utf8(value);
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32 || (!utf8_ok && c >= 128)) {
            result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15];
        }
        else result += static_cast<char>(c);
    }
    return result + '"';
}
int main(int argc, char** argv) {
    if (argc != 5 && argc != 6 && argc != 7 && argc != 8) {
        std::cerr << "Usage: asr-native-probe MODEL_DIR WAV THREADS CHUNK_MS [LANGUAGE [MAX_NEW_TOKENS [refine]]]\n"
                     "Paced CPU-only live-input diagnostic; use an external timeout.\n";
        return 2;
    }
    try {
        const auto parse_integer = [](const char* text) {
            std::size_t used = 0;
            int value = std::stoi(text, &used);
            if (used != std::strlen(text)) throw std::invalid_argument("invalid integer");
            return value;
        };
        const int threads = parse_integer(argv[3]);
        const int chunk = asr::chunk_samples(parse_integer(argv[4]), 16000);
        const int max_new_tokens = argc >= 7 ? parse_integer(argv[6]) : 32;
        const bool refine_final = argc == 8;
        if (refine_final && std::strcmp(argv[7], "refine") != 0) throw std::invalid_argument("invalid finalization mode");
        if (threads < 1 || threads > 16) throw std::invalid_argument("runtime supports 1..16 custom threads");
        if (max_new_tokens < 1 || max_new_tokens > 256) throw std::invalid_argument("max new tokens must be 1..256");
        int count = 0;
        std::unique_ptr<float, decltype(&std::free)> samples(qwen_load_wav(argv[2], &count), std::free);
        if (!samples || count <= 0 || count > 16000 * 300) throw std::runtime_error("invalid WAV or exceeds 5-minute probe limit");
        qwen_verbose = 0; // logging disabled
        qwen_set_threads(threads);
        const auto load_start = Clock::now();
        std::unique_ptr<qwen_ctx_t, decltype(&qwen_free)> model(qwen_load(argv[1]), qwen_free);
        if (!model) throw std::runtime_error("model loading failed");
        const auto load_ms = elapsed(load_start);
        if (argc >= 6 && std::strcmp(argv[5], "auto") != 0 && qwen_set_force_language(model.get(), argv[5]) != 0) {
            throw std::invalid_argument("unsupported language hint");
        }
        model->past_text_conditioning = 1;
        model->stream_chunk_sec = 2.0f;
        model->stream_max_new_tokens = max_new_tokens;
        qwen_live_audio_t live{};
        live.capacity = 16000 * 20; // bounded staging; fail rather than stretch source time
        std::vector<float> buffer(static_cast<std::size_t>(live.capacity));
        live.samples = buffer.data(); // assign underline buffer pointer to live.samples
        if (pthread_mutex_init(&live.mutex, nullptr) != 0) throw std::runtime_error("mutex init failed");
        if (pthread_cond_init(&live.cond, nullptr) != 0) {
            pthread_mutex_destroy(&live.mutex);
            throw std::runtime_error("condition init failed");
        }
        State state;
        state.total_samples = count;
        state.events.reserve(8192);
        qwen_set_token_callback(model.get(), token, &state);
        state.start = Clock::now();
        std::atomic<bool> overflow{false};
        std::atomic<bool> consumer_done{false};
        double max_send_lag_ms = 0;
        std::jthread producer([&] {
            for (int offset = 0; offset < count && !consumer_done.load();) {
                const int n = std::min(chunk, count - offset);
                const auto deadline = state.start + asr::audio_offset(offset + n, 16000);
                std::this_thread::sleep_until(deadline); //deadline is the time when the next chunk should be sent (sampling rates sample pulse time)
                if (consumer_done.load()) break;
                max_send_lag_ms = std::max(max_send_lag_ms, std::chrono::duration<double, std::milli>(Clock::now() - deadline).count());
                pthread_mutex_lock(&live.mutex);
                if (live.n_samples + n > live.capacity) {
                    overflow = true;
                    pthread_mutex_unlock(&live.mutex);
                    break;
                }
                std::copy_n(samples.get() + offset, n, live.samples + live.n_samples);
                live.n_samples += n;
                offset += n;
                state.delivered = offset;
                pthread_cond_signal(&live.cond);
                pthread_mutex_unlock(&live.mutex);
            }
            pthread_mutex_lock(&live.mutex);
            state.eof = true;
            live.eof = 1;
            pthread_cond_signal(&live.cond);
            pthread_mutex_unlock(&live.mutex);
        });
        std::unique_ptr<char, decltype(&std::free)> transcript(qwen_transcribe_stream_live(model.get(), &live), std::free);
        const auto completion_ms = elapsed(state.start);
        consumer_done = true;
        producer.join();
        pthread_cond_destroy(&live.cond);
        pthread_mutex_destroy(&live.mutex);
        std::cout << std::fixed << std::setprecision(3);
        bool pre_eof = false;
        for (const auto& event : state.events) {
            pre_eof = pre_eof || (event.before_eof && event.text.find_first_not_of(" \t\r\n") != std::string::npos);
            std::cout << "{\"type\":\"token\",\"elapsed_ms\":" << event.milliseconds
                      << ",\"delivered_samples\":" << event.delivered_samples
                      << ",\"before_eof\":" << (event.before_eof ? "true" : "false")
                      << ",\"text_utf8_complete\":" << (complete_utf8(event.text) ? "true" : "false")
                      << ",\"text\":" << quote(event.text) << "}\n";
        }
        const std::string stream_transcript = transcript ? transcript.get() : "";
        double final_refine_ms = 0;
        if (refine_final && transcript && !overflow) {
            qwen_set_token_callback(model.get(), nullptr, nullptr);
            const auto refine_start = Clock::now();
            transcript.reset(qwen_transcribe_audio(model.get(), samples.get(), count));
            final_refine_ms = elapsed(refine_start);
        }
        const bool ok = transcript && !overflow && state.delivered == count;
        std::cout << "{\"type\":\"summary\",\"success\":" << (ok ? "true" : "false")
                  << ",\"audio_seconds\":" << count / 16000.0 << ",\"model_load_ms\":" << load_ms
                  << ",\"completion_ms\":" << completion_ms << ",\"final_refine_ms\":" << final_refine_ms
                  << ",\"final_completion_ms\":" << elapsed(state.start)
                  << ",\"max_send_lag_ms\":" << max_send_lag_ms
                  << ",\"text_before_eof\":" << (pre_eof ? "true" : "false")
                  << ",\"overflow\":" << (overflow ? "true" : "false")
                  << ",\"custom_threads\":" << threads << ",\"decode_step_ms\":2000"
                  << ",\"max_new_tokens\":" << max_new_tokens
                  << ",\"language_hint\":" << quote(argc >= 6 ? argv[5] : "auto")
                  << ",\"final_refine\":" << (refine_final ? "true" : "false")
                  << ",\"has_text\":" << ((transcript && transcript.get()[0] != '\0') ? "true" : "false")
                  << ",\"stream_transcript\":" << quote(stream_transcript)
                  << ",\"transcript\":" << quote(transcript ? transcript.get() : "") << "}\n";
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
