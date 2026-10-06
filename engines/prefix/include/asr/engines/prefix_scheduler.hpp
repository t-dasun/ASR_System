#pragma once
#include <cstdint>
#include <optional>
#include <vector>
namespace asr {
struct PrefixReadyJob {
    std::size_t index;
    std::int64_t ready_ns;
    bool final;
    bool expired;
};
// Oldest ready job within a class. At most two previews may bypass a waiting
// EOF job. Expired calls are retired before allocating expensive decode work.
inline std::optional<PrefixReadyJob> select_prefix_job(const std::vector<PrefixReadyJob> &jobs,
                                                       unsigned previews_since_final) {
    std::optional<PrefixReadyJob> preview, final, expired;
    for (const auto &job : jobs) {
        auto &candidate = job.expired ? expired : job.final ? final : preview;
        if (!candidate || job.ready_ns < candidate->ready_ns)
            candidate = job;
    }
    if (expired)
        return expired;
    if (preview && (!final || previews_since_final < 2))
        return preview;
    return final;
}
} // namespace asr
