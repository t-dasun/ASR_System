#pragma once
#include <mutex>

namespace asr {
// Prevent a local API benchmark from competing with interactive calls that
// use a different SessionManager and would defeat the memory preflight.
class ServiceAdmissionGate {
    std::mutex mutex_;
    int live_calls_ = 0;
    bool suite_ = false;

  public:
    bool enter_live() {
        std::lock_guard lock(mutex_);
        if (suite_)
            return false;
        ++live_calls_;
        return true;
    }
    void leave_live() {
        std::lock_guard lock(mutex_);
        --live_calls_;
    }
    bool enter_suite() {
        std::lock_guard lock(mutex_);
        if (suite_ || live_calls_)
            return false;
        suite_ = true;
        return true;
    }
    void leave_suite() {
        std::lock_guard lock(mutex_);
        suite_ = false;
    }
};
} // namespace asr
