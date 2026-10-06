#pragma once
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace asr {
class IClock {
  public:
    virtual ~IClock() = default;
    virtual std::int64_t now_ns() const = 0;
    virtual void sleep_until_ns(std::int64_t deadline) = 0;
    virtual std::string domain() const = 0;
    virtual std::optional<std::string> utc_now() const = 0;
};
inline std::string utc_timestamp() {
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm result{};
    gmtime_r(&time, &result);
    std::ostringstream out;
    out << std::put_time(&result, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}
class SteadyClock final : public IClock {
  public:
    std::int64_t now_ns() const override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    void sleep_until_ns(std::int64_t deadline) override {
        std::this_thread::sleep_until(
            std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline)));
    }
    std::string domain() const override { return "host_steady"; }
    std::optional<std::string> utc_now() const override { return utc_timestamp(); }
};
// Single-threaded test clock: deadlines advance simulated time without sleeping.
class FakeClock final : public IClock {
    std::int64_t now_ = 0;

  public:
    std::int64_t now_ns() const override { return now_; }
    void sleep_until_ns(std::int64_t deadline) override {
        if (deadline < 0)
            throw std::invalid_argument("negative clock deadline");
        if (deadline > now_)
            now_ = deadline;
    }
    std::string domain() const override { return "simulated"; }
    std::optional<std::string> utc_now() const override { return std::nullopt; }
};
} // namespace asr
