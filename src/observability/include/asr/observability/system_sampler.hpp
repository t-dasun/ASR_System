#pragma once
#include <condition_variable>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

namespace asr {
class ISystemSampler {
  public:
    virtual ~ISystemSampler() = default;
    virtual nlohmann::json sample() = 0;
};
class LinuxSystemSampler final : public ISystemSampler {
    nlohmann::json previous_;

  public:
    nlohmann::json sample() override;
};
// Sampling and disk writes stay off the audio delivery thread. Bounded, loss is explicit.
class ResourceMonitor {
    std::unique_ptr<ISystemSampler> sampler_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopped_ = false;
    int interval_ms_;
    std::vector<nlohmann::json> samples_;
    std::string error_;

  public:
    ResourceMonitor(std::unique_ptr<ISystemSampler>, int interval_ms);
    ~ResourceMonitor();
    void stop();
    const auto &samples() const { return samples_; } // Only after stop().
    nlohmann::json summary() const;
};
} // namespace asr
