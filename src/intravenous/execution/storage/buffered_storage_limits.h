#pragma once

#include <cstddef>
#include <cstdint>

namespace iv {

// Executor resources, not authored port semantics. Defaults are provisional
// limits for the initial shared backend and remain configurable by its owner.
struct BufferedStorageLimits {
    std::size_t max_contiguous_mapping_bytes = std::size_t{1} << 30;
    std::size_t max_total_mapping_bytes = std::size_t{4} << 30;
    std::size_t max_backing_bytes = std::size_t{1} << 30;
    std::size_t max_mapping_runs = 16384;
    std::size_t process_vma_headroom = 512;
    std::size_t max_resident_bytes = std::size_t{64} << 20;
    // Optional retained results share the hard backing budget with recordings
    // and pinned readers. This additional bound cannot evict either owner.
    std::size_t max_cached_payload_bytes = std::size_t{256} << 20;
};

struct BufferedStorageUsage {
    std::size_t page_size = 0;
    // Physical sample backing; event_bytes is charged alongside it.
    std::size_t backing_bytes = 0;
    // Logical file extent includes punched, reusable slots.
    std::size_t file_extent_bytes = 0;
    std::size_t live_page_bytes = 0;
    std::size_t event_bytes = 0;
    // All free offsets, including slots without physical backing.
    std::size_t reusable_page_bytes = 0;
    std::size_t cached_page_bytes = 0;
    std::size_t reserved_mapping_bytes = 0;
    std::size_t mapped_runs = 0;
    std::size_t resident_bytes = 0;
    std::uint64_t reused_pages = 0;
    std::uint64_t mapping_calls = 0;
    std::uint64_t cache_evictions = 0;
};

} // namespace iv
