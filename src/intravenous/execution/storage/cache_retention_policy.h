#pragma once

#include <intravenous/execution/storage/buffered_storage_limits.h>
#include <intravenous/ports.h>

namespace iv::persisted_data_store_detail {

struct CacheUse {
    OutputRealization realization = OutputRealization::buffered;
    std::uint64_t last_use = 0;
    std::uint64_t uses = 0;

    // Repeated reads justify retaining computed results.
    [[nodiscard]] bool discard_on_first_use() const noexcept
    {
        return realization == OutputRealization::computed && uses < 2;
    }
};

struct CacheRetentionPolicy {
    BufferedStorageLimits const& limits;
    std::size_t incoming_bytes;

    [[nodiscard]] bool needs_reclamation(
        BufferedStorageUsage const& usage, std::size_t cached_bytes) const noexcept
    {
        return cached_bytes > limits.max_cached_payload_bytes
            || incoming_bytes > limits.max_backing_bytes
            || usage.live_page_bytes + usage.event_bytes > limits.max_backing_bytes - incoming_bytes
            || usage.reserved_mapping_bytes > limits.max_total_mapping_bytes - limits.max_total_mapping_bytes / 4
            || usage.mapped_runs > limits.max_mapping_runs - limits.max_mapping_runs / 4;
    }

    [[nodiscard]] static bool reclaim_before(
        CacheUse const& left, std::size_t left_bytes,
        CacheUse const& right, std::size_t right_bytes) noexcept
    {
        if (left.realization != right.realization) return left.realization == OutputRealization::computed;
        if (left.last_use != right.last_use) return left.last_use < right.last_use;
        if (left.uses != right.uses) return left.uses < right.uses;
        return left_bytes > right_bytes;
    }
};

} // namespace iv::persisted_data_store_detail
