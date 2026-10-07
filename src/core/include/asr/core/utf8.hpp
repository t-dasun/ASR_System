#pragma once
#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace asr {
// Only a trailing incomplete codepoint may be held for the next update.
inline std::size_t complete_utf8_prefix(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        const std::size_t length = c >= 0xc2 && c <= 0xdf   ? 2
                                   : c >= 0xe0 && c <= 0xef ? 3
                                   : c >= 0xf0 && c <= 0xf4 ? 4
                                                            : 0;
        if (!length)
            throw std::runtime_error("invalid UTF-8 stream token");
        const auto available = text.size() - i;
        for (std::size_t j = 1; j < std::min(length, available); ++j)
            if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80)
                throw std::runtime_error("invalid UTF-8 stream continuation");
        if (available > 1) {
            const auto second = static_cast<unsigned char>(text[i + 1]);
            if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second > 0x9f) ||
                (c == 0xf0 && second < 0x90) || (c == 0xf4 && second > 0x8f))
                throw std::runtime_error("invalid UTF-8 stream codepoint");
        }
        if (available < length)
            return i;
        i += length;
    }
    return i;
}
} // namespace asr
