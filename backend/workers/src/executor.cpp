#include <asr/backend/executor.hpp>
#include <stdexcept>

namespace asr {
FactoryWorkerExecutor::FactoryWorkerExecutor(std::string kind,
                                             std::function<std::unique_ptr<IASREngine>()> factory)
    : factory_(std::move(factory)), kind_(std::move(kind)) {
    if (!factory_ || (kind_ != "process" && kind_ != "in_process"))
        throw std::invalid_argument("invalid worker executor factory or kind");
}
std::unique_ptr<IASREngine> FactoryWorkerExecutor::make_engine() const {
    auto engine = factory_();
    if (!engine)
        throw std::runtime_error("worker executor returned no engine");
    return engine;
}
} // namespace asr
