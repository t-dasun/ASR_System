#include <asr/core/chunk_schedule.hpp>
#include <algorithm>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        auto require = [](bool ok) { if (!ok) throw std::runtime_error("schedule assertion failed"); };
        for (int ms : {100, 200, 500, 1000}) {
            const auto size = asr::chunk_samples(ms, 16000);
            std::int64_t covered = 0;
            const std::int64_t total = 16000 * 60 + 123;
            auto previous = std::chrono::nanoseconds::zero();
            while (covered < total) {
                covered += std::min<std::int64_t>(size, total - covered);
                const auto deadline = asr::audio_offset(covered, 16000);
                require(deadline > previous); // no chunk available at t=0
                previous = deadline;
            }
            require(covered == total);
            require(previous.count() == 60'007'687'500LL); // exact tail, no pacing drift
        }
        for (int invalid : {0, -1, 1001}) {
            bool rejected = false;
            try { (void)asr::chunk_samples(invalid, 16000); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected);
        }
        bool rejected = false;
        try { (void)asr::audio_offset(-1, 16000); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected);
        std::cout << "sample deadlines, tail handling, and invalid settings passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
