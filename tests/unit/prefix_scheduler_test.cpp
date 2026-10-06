#include <asr/engines/prefix_scheduler.hpp>
#include <stdexcept>
int main() {
    using namespace asr;
    auto check = [](bool v) {
        if (!v)
            throw std::runtime_error("prefix priority/fairness contract failed");
    };
    std::vector<PrefixReadyJob> jobs = {{0, 10, true, false}, {1, 20, false, false}, {2, 15, false, false}};
    check(select_prefix_job(jobs, 0)->index == 2);
    check(select_prefix_job(jobs, 1)->index == 2);
    check(select_prefix_job(jobs, 2)->index == 0);
    jobs.push_back({3, 30, false, true});
    check(select_prefix_job(jobs, 0)->index == 3);
    check(!select_prefix_job({}, 0));
    check(select_prefix_job({{1, 10, true, false}}, 0)->index == 1);
}
