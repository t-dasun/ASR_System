// M0 diagnostic: sequential language changes and repeated model create/free.
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
extern "C" {
#include "qwen_asr.h"
#include "qwen_asr_kernels.h"
}

static long rss_kb() {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            long value = 0;
            status >> value;
            return value;
        }
        std::string rest;
        std::getline(status, rest);
    }
    return -1;
}

static std::string transcribe(qwen_ctx_t* model, const char* language, const char* wav) {
    if (qwen_set_force_language(model, language) != 0)
        throw std::runtime_error("language hint rejected");
    std::unique_ptr<char, decltype(&std::free)> result(qwen_transcribe(model, wav), std::free);
    if (!result) throw std::runtime_error("transcription failed");
    return result.get();
}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "Usage: asr-native-lifecycle MODEL_DIR EN_WAV ID_WAV ZH_WAV SILENCE_WAV\n";
        return 2;
    }
    try {
        qwen_verbose = 0;
        qwen_set_threads(4);
        std::array<long, 3> after_free{};
        std::string first_en;
        std::string first_silence;
        bool language_roundtrip = false;
        bool silence_consistent = true;
        for (int cycle = 0; cycle < 3; ++cycle) {
            {
                std::unique_ptr<qwen_ctx_t, decltype(&qwen_free)> model(qwen_load(argv[1]), qwen_free);
                if (!model) throw std::runtime_error("model loading failed");
                if (cycle == 0) {
                    first_en = transcribe(model.get(), "English", argv[2]);
                    const auto indonesian = transcribe(model.get(), "Indonesian", argv[3]);
                    const auto chinese = transcribe(model.get(), "Chinese", argv[4]);
                    const auto repeated_en = transcribe(model.get(), "English", argv[2]);
                    language_roundtrip = !first_en.empty() && !indonesian.empty() &&
                                         !chinese.empty() && repeated_en == first_en;
                }
                const auto silence = transcribe(model.get(), "English", argv[5]);
                if (cycle == 0) first_silence = silence;
                else silence_consistent = silence_consistent && silence == first_silence;
            }
            after_free[cycle] = rss_kb();
        }
        const bool stable_rss = after_free[1] > 0 && after_free[2] > 0 &&
                                after_free[2] - after_free[1] < 128 * 1024;
        const bool passed = language_roundtrip && silence_consistent && stable_rss;
        std::cout << "{\"language_roundtrip\":" << (language_roundtrip ? "true" : "false")
                  << ",\"silence_consistent\":" << (silence_consistent ? "true" : "false")
                  << ",\"silence_hallucination\":" << (!first_silence.empty() ? "true" : "false")
                  << ",\"rss_after_free_kb\":[" << after_free[0] << ',' << after_free[1]
                  << ',' << after_free[2] << "]"
                  << ",\"rss_growth_last_cycle_kb\":" << after_free[2] - after_free[1]
                  << ",\"passed\":" << (passed ? "true" : "false") << "}\n";
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
