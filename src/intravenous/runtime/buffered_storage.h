#pragma once

#include <intravenous/runtime/persisted_data_store.h>
#include <map>
#include <mutex>
#include <optional>

namespace iv::persisted_data_store_detail {

// Share a process mapping count across buffered operations on this thread.
// The outermost scope samples on demand and discards the estimate on exit.
class ProcessVmaBudget {
    static thread_local ProcessVmaBudget* active_;
    std::optional<std::size_t> available_;

public:
    enum class Operation { mapping, residency };

    ProcessVmaBudget() noexcept;
    ~ProcessVmaBudget();
    ProcessVmaBudget(ProcessVmaBudget const&) = delete;
    ProcessVmaBudget& operator=(ProcessVmaBudget const&) = delete;

    static void reserve(std::size_t headroom, std::size_t growth, Operation operation);
    // Removing or unlocking part of a VMA can split it. Charge the worst
    // case without acquiring a fresh count or throwing during cleanup.
    static void account_for_growth(std::size_t growth) noexcept;
};

// Shared physical backing and resource accounting. All mutation, mapping and
// lease destruction are background/control work; no authored node owns this.
struct BufferedPage {
    std::uint64_t file_offset = 0;
    BufferedPage* free_left = nullptr;
    BufferedPage* free_right = nullptr;
    std::uint32_t free_height = 1;
};

class BufferedPageAllocator : public std::enable_shared_from_this<BufferedPageAllocator> {
    int file_ = -1;
    std::size_t page_size_ = 0;
    std::uint64_t next_file_offset_ = 0;
    BufferedStorageLimits limits_{};
    mutable std::mutex mutex_{};
    // Intrusive AVL trees ordered by file offset. Last release allocates no
    // metadata; successive allocations reuse consecutive available offsets.
    BufferedPage* free_ = nullptr;
    BufferedPage* empty_slots_ = nullptr;
    std::size_t backed_pages_ = 0;
    std::size_t live_pages_ = 0;
    std::size_t free_pages_ = 0;
    std::uint64_t reused_pages_ = 0;
    std::size_t mapped_bytes_ = 0;
    std::size_t mapped_runs_ = 0;
    std::uint64_t mapping_calls_ = 0;
    std::size_t event_bytes_ = 0;
    std::map<std::uintptr_t, std::size_t> resident_pages_{};

    void release(BufferedPage const* page) noexcept;
    // Called with mutex_ held. Only unreferenced cached pages may be punched.
    void reserve_backing(std::size_t bytes);

public:
    explicit BufferedPageAllocator(BufferedStorageLimits limits);
    ~BufferedPageAllocator();

    BufferedPageAllocator(BufferedPageAllocator const&) = delete;
    BufferedPageAllocator& operator=(BufferedPageAllocator const&) = delete;

    [[nodiscard]] int file() const noexcept { return file_; }
    [[nodiscard]] std::size_t page_size() const noexcept { return page_size_; }
    [[nodiscard]] BufferedStorageUsage usage() const;
    [[nodiscard]] BufferedStorageLimits const& limits() const noexcept { return limits_; }
    void reserve_mapping(std::size_t bytes, std::size_t runs);
    void release_mapping(std::size_t bytes, std::size_t runs) noexcept;
    [[nodiscard]] std::shared_ptr<void const> prepare_resident(std::span<BufferedResidentRange const> ranges);
    void release_resident(std::span<std::uintptr_t const> pages) noexcept;
    [[nodiscard]] std::shared_ptr<void const> retain_event_bytes(std::size_t bytes);
    void read(BufferedPage const& page, std::span<std::byte> target) const;
    [[nodiscard]] std::shared_ptr<BufferedPage const> allocate(
        std::span<std::byte const> contents);
};
} // namespace iv::persisted_data_store_detail
