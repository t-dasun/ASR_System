#include <asr/audio/wav.hpp>
extern "C" {
#include "qwen_asr.h"
#include "qwen_asr_kernels.h"
}
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
using Stream = std::unique_ptr<qwen_resumable_state, decltype(&qwen_resumable_destroy)>;
std::vector<float> audio(const char *path) {
    asr::SincResampler resampler;
    auto input = asr::prepare_wav(path, asr::ChannelMix::reject, resampler);
    std::vector<float> samples;
    for (auto x : *input.pcm)
        samples.push_back(float(x) / 32768.0f);
    return samples;
}
void no_tokens(const char *, void *) {}
int interrupt(void *) { return 0; }
Stream create(qwen_ctx_t *model, const char *language) {
    Stream state(qwen_resumable_create(model, language, 2000, 32, 0), qwen_resumable_destroy);
    check(bool(state), "stream state allocation failed");
    return state;
}
bool advance(qwen_resumable_state *state, const std::vector<float> &pcm) {
    const auto before = qwen_resumable_cursor(state);
    const int available = std::min<std::int64_t>(pcm.size(), before + 32000);
    const int result = qwen_resumable_step(state, pcm.data(), available, available == int(pcm.size()));
    check(result == 1 || result == 2, "stream step failed");
    check(qwen_resumable_cursor(state) == available, "step consumed an incorrect sample boundary");
    return result == 2;
}
} // namespace

// Explicit real-model check, not part of fast/model-free CTest.
int main(int argc, char **argv) {
    try {
        check(argc == 4, "usage: resumable-state-test MODEL EN_WAV ID_WAV");
        qwen_set_threads(4);
        std::unique_ptr<qwen_ctx_t, decltype(&qwen_free)> model(qwen_load(argv[1]), qwen_free);
        check(bool(model), "model load failed");
        auto en = audio(argv[2]), id = audio(argv[3]);
        auto a = create(model.get(), "English"), b = create(model.get(), "Indonesian");
        check(qwen_resumable_step(a.get(), en.data(), 1000, 0) == 0, "insufficient audio decoded");
        bool done_a = false, done_b = false;
        while (!done_a || !done_b) {
            if (!done_a)
                done_a = advance(a.get(), en);
            if (!done_b)
                done_b = advance(b.get(), id);
        }
        const std::string interleaved_a = qwen_resumable_text(a.get());
        const std::string interleaved_b = qwen_resumable_text(b.get());
        check(!interleaved_a.empty() && !interleaved_b.empty(), "interleaved streams had empty finals");
        check(qwen_resumable_reused_prefill(a.get()) > 0 && qwen_resumable_reused_prefill(b.get()) > 0,
              "decoder prefill cache was not reused across steps");
        model->stream_chunk_sec = 2;
        model->stream_max_new_tokens = 32;
        model->stream_unfixed_chunks = 0;
        model->past_text_conditioning = 1;
        qwen_set_token_callback(model.get(), no_tokens, nullptr); // Avoid upstream offline shortcut.
        check(qwen_set_force_language(model.get(), "English") == 0, "invalid English language");
        std::unique_ptr<char, decltype(&std::free)> native_a(
            qwen_transcribe_stream(model.get(), en.data(), int(en.size())), std::free);
        check(qwen_set_force_language(model.get(), "Indonesian") == 0, "invalid Indonesian language");
        std::unique_ptr<char, decltype(&std::free)> native_b(
            qwen_transcribe_stream(model.get(), id.data(), int(id.size())), std::free);
        check(native_a && interleaved_a == native_a.get(), "English differed from the native streaming loop");
        check(native_b && interleaved_b == native_b.get(),
              "Indonesian differed from the native streaming loop");
        auto cancelled = create(model.get(), "English");
        model->offline_decode_guard = interrupt;
        check(qwen_resumable_step(cancelled.get(), en.data(), 32000, 0) == -2,
              "stream step did not honor cooperative cancellation");
        model->offline_decode_guard = nullptr;
        cancelled.reset();
        auto recovered = create(model.get(), "English");
        while (!advance(recovered.get(), en)) {
        }
        check(interleaved_a == qwen_resumable_text(recovered.get()), "cancelled call contaminated new state");
        std::cout << "PASS: two interleaved languages match native streaming; cached state reused; "
                     "cancellation isolated\n";
        std::cout << "EN steps=" << qwen_resumable_steps(a.get())
                  << " reused_prefill=" << qwen_resumable_reused_prefill(a.get()) << '\n';
        std::cout << "ID steps=" << qwen_resumable_steps(b.get())
                  << " reused_prefill=" << qwen_resumable_reused_prefill(b.get()) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
