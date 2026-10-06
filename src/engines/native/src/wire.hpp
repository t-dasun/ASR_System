#pragma once
#include <cstdint>

namespace asr::native_wire {
constexpr std::uint32_t magic = 0x41535231U; // ASR1, same-host process protocol v1.
enum : std::uint32_t { audio = 1, eof = 2 };
struct Header {
    std::uint32_t protocol = magic, type = 0;
    std::uint64_t sequence = 0;
    std::int64_t first_sample = 0;
    std::uint32_t sample_count = 0, reserved = 0;
};
static_assert(sizeof(Header) == 32);
} // namespace asr::native_wire
