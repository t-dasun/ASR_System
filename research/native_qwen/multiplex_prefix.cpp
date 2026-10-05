// Research probe: one loaded Qwen context, two live call timelines, serial prefix redecoding.
// This is not the native live API or a production IASREngine implementation.
extern "C" {
#include "qwen_asr.h"
#include "qwen_asr_audio.h"
#include "qwen_asr_kernels.h"
}
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

struct Call {
    std::string id, language, path;
    std::unique_ptr<float, decltype(&std::free)> samples{nullptr, std::free};
    int total_samples = 0;
    bool preview_done = false, final_done = false;
    Json events = Json::array();
};

static double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

static long rss_kib() {
    std::ifstream file("/proc/self/status");
    std::string key;
    while (file >> key) {
        if (key == "VmRSS:") {
            long value = -1;
            file >> value;
            return value;
        }
        std::string rest;
        std::getline(file, rest);
    }
    return -1;
}

static void infer(qwen_ctx_t *model, Call &call, int prefix_samples, bool final,
                  Clock::time_point start) {
    if (qwen_set_force_language(model, call.language.c_str()) != 0)
        throw std::runtime_error("language rejected for " + call.id);
    const double begin_ms = elapsed_ms(start);
    std::unique_ptr<char, decltype(&std::free)> text(
        qwen_transcribe_audio(model, call.samples.get(), prefix_samples), std::free);
    if (!text)
        throw std::runtime_error("native inference failed for " + call.id);
    const double end_ms = elapsed_ms(start);
    const bool before_eof = !final && end_ms < 1000.0 * call.total_samples / QWEN_SAMPLE_RATE;
    call.events.push_back({
        {"kind", final ? "final" : "prefix_preview"},
        {"prefix_samples", prefix_samples},
        {"begin_ms", begin_ms},
        {"end_ms", end_ms},
        {"inference_ms", end_ms - begin_ms},
        {"text", text.get()},
        {"text_before_eof", before_eof && text.get()[0] != '\0'},
    });
    if (final) call.final_done = true;
    else call.preview_done = true;
}

int main(int argc, char **argv) {
    if (argc != 7) {
        std::cerr << "Usage: asr-native-prefix-multiplex MODEL EN_WAV EN_LANGUAGE ID_WAV ID_LANGUAGE PREVIEW_MS\n";
        return 2;
    }
    try {
        const int preview_ms = std::stoi(argv[6]);
        if (preview_ms < 1000 || preview_ms > 20000)
            throw std::invalid_argument("preview_ms must be 1000..20000");
        qwen_verbose = 0;
        qwen_set_threads(4);
        std::array<Call, 2> calls{{
            {.id = "call_0", .language = argv[3], .path = argv[2]},
            {.id = "call_1", .language = argv[5], .path = argv[4]},
        }};
        for (auto &call : calls) {
            call.samples.reset(qwen_load_wav(call.path.c_str(), &call.total_samples));
            if (!call.samples || call.total_samples < preview_ms * QWEN_SAMPLE_RATE / 1000)
                throw std::runtime_error("WAV missing or shorter than preview: " + call.path);
        }
        const auto load_start = Clock::now();
        std::unique_ptr<qwen_ctx_t, decltype(&qwen_free)> model(qwen_load(argv[1]), qwen_free);
        if (!model) throw std::runtime_error("model loading failed");
        const double load_ms = elapsed_ms(load_start);
        const long loaded_rss_kib = rss_kib();
        const auto start = Clock::now();
        const int preview_samples = preview_ms * QWEN_SAMPLE_RATE / 1000;

        while (!calls[0].final_done || !calls[1].final_done) {
            bool worked = false;
            // Both call timelines start together. Inference is deliberately serial on one context.
            for (auto &call : calls) {
                const double now = elapsed_ms(start);
                const double duration_ms = 1000.0 * call.total_samples / QWEN_SAMPLE_RATE;
                if (!call.preview_done && now >= preview_ms && now < duration_ms) {
                    infer(model.get(), call, preview_samples, false, start);
                    worked = true;
                } else if (!call.final_done && now >= duration_ms) {
                    infer(model.get(), call, call.total_samples, true, start);
                    worked = true;
                }
            }
            if (!worked) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Json output = {
            {"schema_version", 1},
            {"method", "one_model_serial_prefix_redecode"},
            {"causal_input", "only the scheduled sample prefix is passed before EOF"},
            {"native_live_api_used", false},
            {"active_calls", 2},
            {"model_load_ms", load_ms},
            {"rss_after_load_kib", loaded_rss_kib},
            {"rss_after_calls_kib", rss_kib()},
            {"calls", Json::array()},
        };
        for (const auto &call : calls) {
            output["calls"].push_back({
                {"id", call.id}, {"language", call.language}, {"path", call.path},
                {"audio_seconds", double(call.total_samples) / QWEN_SAMPLE_RATE},
                {"events", call.events},
            });
        }
        std::cout << output.dump(2) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
