#include "wire.hpp"
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
void send_json(const nlohmann::json &record) {
    const auto data = record.dump() + "\n";
    const char *cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining) {
        const auto count = ::write(1, cursor, remaining);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            std::_Exit(2);
        cursor += count;
        remaining -= count;
    }
}
bool read_exact(void *destination, std::size_t count) {
    auto *cursor = static_cast<char *>(destination);
    while (count) {
        const auto amount = ::read(3, cursor, count);
        if (amount < 0 && errno == EINTR)
            continue;
        if (amount <= 0)
            return false;
        cursor += amount;
        count -= amount;
    }
    return true;
}
std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 9 || !std::getenv("OMP_NUM_THREADS") || !std::getenv("OPENBLAS_NUM_THREADS") ||
        std::string(std::getenv("OMP_NUM_THREADS")) != argv[3] ||
        std::string(std::getenv("OPENBLAS_NUM_THREADS")) != argv[3])
        return 19;
    const auto *mode = std::getenv("ASR_NATIVE_STUB_MODE");
    const std::string behavior = mode ? mode : "normal";
    if (behavior == "expect_step" && std::string(argv[4]) != "4000")
        return 20;
    const std::string language = argv[2];
    send_json({{"type", "ready"}, {"produced_ns", now_ns()}});
    std::uint64_t sequence = 0;
    std::int64_t samples = 0;
    while (true) {
        asr::native_wire::Header header;
        if (!read_exact(&header, sizeof(header)))
            return 3;
        if (header.protocol != asr::native_wire::magic || header.sequence != sequence ||
            header.first_sample != samples)
            return 4;
        if (header.type == asr::native_wire::eof) {
            if (behavior == "hang")
                std::this_thread::sleep_for(std::chrono::seconds(3));
            send_json({{"type", "final"},
                       {"text", "stub final " + language},
                       {"produced_ns", now_ns()},
                       {"consumed_samples", samples}});
            return 0;
        }
        if (header.type != asr::native_wire::audio || !header.sample_count)
            return 5;
        std::vector<std::int16_t> pcm(header.sample_count);
        if (!read_exact(pcm.data(), pcm.size() * sizeof(std::int16_t)))
            return 6;
        samples += header.sample_count;
        ++sequence;
        if (behavior == "crash")
            return 17;
        send_json({{"type", "partial"},
                   {"text", "stub partial " + language + " " + std::to_string(samples)},
                   {"produced_ns", now_ns()},
                   {"consumed_samples", samples},
                   {"before_eof", true}});
    }
}
