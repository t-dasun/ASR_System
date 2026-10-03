#include <asr/audio/delivery.hpp>
#include <chrono>
#include <iostream>

int main() {
    try {
        asr::SteadyClock clock;
        asr::SyntheticPcmSource source(60 * 16000, 42);
        asr::PacedAudioStream stream(source, clock, "timing", "60s", 60 * 16000, 200,
                                     asr::DeliveryLimits{8, 1600, 2000, 5});
        const auto start = clock.now_ns();
        while (stream.state() == asr::DeliveryState::running ||
               stream.state() == asr::DeliveryState::draining) {
            if (stream.state() == asr::DeliveryState::running && stream.stats().produced_samples < 960000)
                clock.sleep_until_ns(stream.next_deadline_ns());
            auto status = stream.produce_ready();
            if (!status)
                throw std::runtime_error(status.message);
            if (stream.state() == asr::DeliveryState::running && stream.stats().produced_samples == 960000) {
                status = stream.finish_input(300, 960000);
                if (!status)
                    throw std::runtime_error(status.message);
            }
            auto chunk = stream.pop();
            if (!chunk)
                throw std::runtime_error(chunk.status.message);
        }
        const auto elapsed = (clock.now_ns() - start) / 1e9;
        const auto &stats = stream.stats();
        std::cout << "elapsed_seconds=" << elapsed << " samples=" << stats.delivered_samples
                  << " chunks=" << stats.delivered_chunks << " late_chunks=" << stats.late_chunks
                  << " max_lag_ms=" << stats.max_lag_ns / 1e6 << '\n';
        // Linux laptop under ordinary load; scheduler noise allowance ±0.5 s.
        if (elapsed < 59.5 || elapsed > 60.5 || stats.delivered_samples != 960000 ||
            stats.delivered_chunks != 300 || stats.overflows != 0)
            return 1;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
