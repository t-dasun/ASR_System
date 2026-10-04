#pragma once
#include <asr/engines/engine.hpp>
#include <functional>

namespace asr {
// Each launch creates an independent engine owner. A native engine launches its
// already-versioned, bounded PCM/JSONL worker process; a mock runs in-process.
class IWorkerExecutor {
  public:
    virtual ~IWorkerExecutor() = default;
    virtual std::unique_ptr<IASREngine> make_engine() const = 0;
    virtual std::string kind() const = 0;
};
class FactoryWorkerExecutor final : public IWorkerExecutor {
    std::function<std::unique_ptr<IASREngine>()> factory_;
    std::string kind_;

  public:
    FactoryWorkerExecutor(std::string kind, std::function<std::unique_ptr<IASREngine>()> factory);
    std::unique_ptr<IASREngine> make_engine() const override;
    std::string kind() const override { return kind_; }
};
} // namespace asr
