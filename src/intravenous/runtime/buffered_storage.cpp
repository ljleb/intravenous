#include <intravenous/runtime/buffered_storage.h>
#include <intravenous/runtime/playback_preparation_metrics.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace iv {
namespace {
using persisted_data_store_detail::BufferedPage;
std::uint32_t free_page_height(BufferedPage const* page) noexcept { return page ? page->free_height : 0; }
void update_free_height(BufferedPage* page) noexcept
{
    page->free_height = 1 + std::max(free_page_height(page->free_left), free_page_height(page->free_right));
}
BufferedPage* rotate_free_left(BufferedPage* page) noexcept
{
    auto* next = page->free_right;
    page->free_right = next->free_left;
    next->free_left = page;
    update_free_height(page);
    update_free_height(next);
    return next;
}
BufferedPage* rotate_free_right(BufferedPage* page) noexcept
{
    auto* next = page->free_left;
    page->free_left = next->free_right;
    next->free_right = page;
    update_free_height(page);
    update_free_height(next);
    return next;
}
BufferedPage* balance_free_pages(BufferedPage* page) noexcept
{
    update_free_height(page);
    auto const balance = static_cast<int>(free_page_height(page->free_left))
        - static_cast<int>(free_page_height(page->free_right));
    if (balance > 1) {
        if (free_page_height(page->free_left->free_left) < free_page_height(page->free_left->free_right))
            page->free_left = rotate_free_left(page->free_left);
        return rotate_free_right(page);
    }
    if (balance < -1) {
        if (free_page_height(page->free_right->free_right) < free_page_height(page->free_right->free_left))
            page->free_right = rotate_free_right(page->free_right);
        return rotate_free_left(page);
    }
    return page;
}
BufferedPage* insert_free_page(BufferedPage* root, BufferedPage* page) noexcept
{
    if (!root) return page;
    assert(root->file_offset != page->file_offset);
    if (page->file_offset < root->file_offset) root->free_left = insert_free_page(root->free_left, page);
    else root->free_right = insert_free_page(root->free_right, page);
    return balance_free_pages(root);
}
BufferedPage* take_first_free_page(BufferedPage*& root) noexcept
{
    assert(root);
    if (root->free_left) {
        auto* page = take_first_free_page(root->free_left);
        root = balance_free_pages(root);
        return page;
    }
    auto* page = root;
    root = page->free_right;
    page->free_left = page->free_right = nullptr;
    page->free_height = 1;
    return page;
}
BufferedPage const* first_free_page(BufferedPage const* root) noexcept
{
    if (!root) return nullptr;
    while (root->free_left) root = root->free_left;
    return root;
}
void delete_free_pages(BufferedPage* root) noexcept
{
    if (!root) return;
    delete_free_pages(root->free_left);
    delete_free_pages(root->free_right);
    delete root;
}
} // namespace

using persisted_data_store_detail::ProcessVmaBudget;

thread_local ProcessVmaBudget* ProcessVmaBudget::active_ = nullptr;

ProcessVmaBudget::ProcessVmaBudget() noexcept
{
    if (!active_) active_ = this;
}

ProcessVmaBudget::~ProcessVmaBudget()
{
    if (active_ == this) active_ = nullptr;
}

void ProcessVmaBudget::reserve(std::size_t headroom, std::size_t growth, Operation operation)
{
    // A standalone operation gets a fresh count. Nested work shares the
    // estimate of the outer transaction.
    ProcessVmaBudget standalone;
    auto& available = active_->available_;
    auto fits = [&] {
        return available && headroom <= *available && growth <= *available - headroom;
    };
    if (!fits()) {
        auto const mapping = operation == Operation::mapping;
        PlaybackPreparationMetrics::Interval scan{mapping
            ? PlaybackPreparationPhase::mapping_vma_check
            : PlaybackPreparationPhase::residency_vma_check};
        std::ifstream maps{"/proc/self/maps"}, maximum{"/proc/sys/vm/max_map_count"};
        std::size_t count = 0, limit = 0;
        std::string line;
        while (std::getline(maps, line)) ++count;
        if (!maps.eof() || !(maximum >> limit))
            throw std::runtime_error(mapping
                ? "cannot inspect process VMA budget for buffered mapping"
                : "cannot inspect residency VMA headroom");
        scan.set_work(count);
        available = limit - std::min(limit, count);
    }
    if (!fits())
        throw std::length_error(operation == Operation::mapping
            ? "buffered mapping would exceed process VMA headroom"
            : "buffered residency would exceed process VMA headroom");
    *available -= growth;
}

void ProcessVmaBudget::account_for_growth(std::size_t growth) noexcept
{
    if (active_ && active_->available_) {
        auto& available = *active_->available_;
        available -= std::min(available, growth);
    }
}

persisted_data_store_detail::BufferedPageAllocator::BufferedPageAllocator(BufferedStorageLimits limits)
    : limits_(limits)
{
    if (!limits.max_contiguous_mapping_bytes || !limits.max_total_mapping_bytes
        || !limits.max_backing_bytes || !limits.max_mapping_runs || !limits.max_resident_bytes)
        throw std::invalid_argument("buffered storage limits must be nonzero");
#if defined(__linux__)
    auto const page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot query the Linux page size");
    }
    page_size_ = static_cast<std::size_t>(page_size);
#else
    page_size_ = 4096;
#endif
}

persisted_data_store_detail::BufferedPageAllocator::~BufferedPageAllocator()
{
    assert(live_pages_ == 0);
    delete_free_pages(free_);
    delete_free_pages(empty_slots_);
#if defined(__linux__)
    if (file_ >= 0) ::close(file_);
#endif
}

void persisted_data_store_detail::BufferedPageAllocator::release(BufferedPage const* released) noexcept
{
    std::lock_guard lock{mutex_};
    auto* page = const_cast<BufferedPage*>(released);
    free_ = insert_free_page(free_, page);
    --live_pages_;
    ++free_pages_;
}

BufferedStorageUsage persisted_data_store_detail::BufferedPageAllocator::usage() const
{
    std::lock_guard lock{mutex_};
    return {.page_size = page_size_, .backing_bytes = backed_pages_ * page_size_,
        .file_extent_bytes = static_cast<std::size_t>(next_file_offset_),
        .live_page_bytes = live_pages_ * page_size_, .event_bytes = event_bytes_,
        .reusable_page_bytes = free_pages_ * page_size_,
        .cached_page_bytes = (backed_pages_ - live_pages_) * page_size_,
        .reserved_mapping_bytes = mapped_bytes_, .mapped_runs = mapped_runs_,
        .resident_bytes = resident_pages_.size() * page_size_,
        .reused_pages = reused_pages_, .mapping_calls = mapping_calls_};
}

void persisted_data_store_detail::BufferedPageAllocator::reserve_backing(std::size_t bytes)
{
    auto used = backed_pages_ * page_size_ + event_bytes_;
    if (bytes <= limits_.max_backing_bytes) {
        while (used > limits_.max_backing_bytes - bytes && free_) {
#if defined(__linux__)
            auto const* first = first_free_page(free_);
            if (::fallocate(file_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                    static_cast<off_t>(first->file_offset), static_cast<off_t>(page_size_)) != 0)
                throw std::system_error(errno, std::generic_category(), "cannot reclaim unused buffered backing");
#endif
            auto* page = take_first_free_page(free_);
            empty_slots_ = insert_free_page(empty_slots_, page);
            --backed_pages_;
            used -= page_size_;
        }
    }
    if (bytes > limits_.max_backing_bytes || used > limits_.max_backing_bytes - bytes)
        throw std::length_error("buffered physical backing limit exceeded: " + std::to_string(used)
            + " used, " + std::to_string(bytes) + " requested, limit " + std::to_string(limits_.max_backing_bytes));
}

void persisted_data_store_detail::BufferedPageAllocator::reserve_mapping(std::size_t bytes, std::size_t runs)
{
    std::lock_guard lock{mutex_};
    auto require_room = [](std::size_t used, std::size_t requested, std::size_t maximum, char const* resource) {
        if (requested > maximum || used > maximum - requested)
            throw std::length_error(std::string{"buffered "} + resource + " limit exceeded: "
                + std::to_string(used) + " used, " + std::to_string(requested)
                + " requested, limit " + std::to_string(maximum));
    };
    require_room(0, bytes, limits_.max_contiguous_mapping_bytes, "contiguous mapping bytes");
    require_room(mapped_bytes_, bytes, limits_.max_total_mapping_bytes, "aggregate mapping bytes");
    require_room(mapped_runs_, runs, limits_.max_mapping_runs, "mapping runs");
#if defined(__linux__)
    // Each backing run can add a VMA. Leave room for the reservation and
    // boundary splits while the backing runs replace it.
    if (runs > std::numeric_limits<std::size_t>::max() - 2)
        throw std::length_error("buffered process VMA growth overflows");
    ProcessVmaBudget::reserve(limits_.process_vma_headroom, runs + 2, ProcessVmaBudget::Operation::mapping);
    rlimit address_limit{};
    if (::getrlimit(RLIMIT_AS, &address_limit) != 0)
        throw std::system_error(errno, std::generic_category(), "cannot inspect buffered address-space limit");
    if (address_limit.rlim_cur != RLIM_INFINITY) {
        std::ifstream statm{"/proc/self/statm"};
        std::uint64_t virtual_pages = 0;
        if (!(statm >> virtual_pages) || virtual_pages > std::numeric_limits<std::size_t>::max() / page_size_)
            throw std::runtime_error("cannot inspect process virtual memory for buffered mapping");
        require_room(static_cast<std::size_t>(virtual_pages) * page_size_, bytes,
            static_cast<std::size_t>(address_limit.rlim_cur), "process address space");
    }
#endif
    mapped_bytes_ += bytes;
    mapped_runs_ += runs;
    mapping_calls_ += runs + 1;
}

void persisted_data_store_detail::BufferedPageAllocator::release_mapping(std::size_t bytes, std::size_t runs) noexcept
{
    std::lock_guard lock{mutex_};
    assert(bytes <= mapped_bytes_ && runs <= mapped_runs_);
    mapped_bytes_ -= bytes;
    mapped_runs_ -= runs;
}

std::shared_ptr<void const> persisted_data_store_detail::BufferedPageAllocator::prepare_resident(
    std::span<BufferedResidentRange const> ranges)
{
    std::set<std::uintptr_t> unique;
    for (auto const range : ranges) {
        if (!range.bytes) continue;
        auto const begin = reinterpret_cast<std::uintptr_t>(range.data);
        if (!begin || range.bytes - 1 > std::numeric_limits<std::uintptr_t>::max() - begin)
            throw std::length_error("buffered resident range overflows its address");
        auto const last = (begin + range.bytes - 1) / page_size_ * page_size_;
        for (auto page = begin / page_size_ * page_size_;; page += page_size_) {
            unique.insert(page);
            if (unique.size() > limits_.max_resident_bytes / page_size_)
                throw std::length_error("selected buffered payload exceeds realtime residency limit "
                    + std::to_string(limits_.max_resident_bytes));
            if (page == last) break;
        }
    }
    auto selected = std::make_unique<std::vector<std::uintptr_t>>(unique.begin(), unique.end());
    if (selected->empty()) return {};
    auto owner = shared_from_this();
    {
        std::lock_guard lock{mutex_};
        auto added = std::ranges::count_if(*selected, [&](auto page) { return !resident_pages_.contains(page); });
        auto const requested = static_cast<std::size_t>(added) * page_size_;
        auto const used = resident_pages_.size() * page_size_;
        if (requested > limits_.max_resident_bytes || used > limits_.max_resident_bytes - requested)
            throw std::length_error("buffered realtime residency limit exceeded: " + std::to_string(used)
                + " used, " + std::to_string(requested) + " requested, limit " + std::to_string(limits_.max_resident_bytes));
#if defined(__linux__)
        std::size_t groups = 0;
        std::optional<std::uintptr_t> previous;
        for (auto page : *selected) if (!resident_pages_.contains(page)) {
            if (!previous || page - *previous != page_size_) ++groups;
            previous = page;
        }
        if (groups) {
            // A newly locked run can split a VMA at its two ends.
            ProcessVmaBudget::reserve(limits_.process_vma_headroom, 2 * groups, ProcessVmaBudget::Operation::residency);
        }
#endif
        std::size_t acquired = 0;
        try {
            for (auto page : *selected) {
                auto [entry, inserted] = resident_pages_.try_emplace(page, 0);
                if (inserted) {
#if defined(__linux__)
                    PlaybackPreparationMetrics::Interval lock_page{PlaybackPreparationPhase::mlock_call, page_size_};
                    if (::mlock(reinterpret_cast<void*>(page), page_size_) != 0) {
                        auto const error = errno;
                        resident_pages_.erase(entry);
                        throw std::system_error(error, std::generic_category(), "cannot pin prepared buffered data; check RLIMIT_MEMLOCK");
                    }
#endif
                }
                ++entry->second;
                ++acquired;
            }
        } catch (...) {
            for (std::size_t index = 0; index < acquired; ++index) {
                auto entry = resident_pages_.find((*selected)[index]);
                if (!--entry->second) {
#if defined(__linux__)
                    PlaybackPreparationMetrics::Interval unlock_page{PlaybackPreparationPhase::munlock_call, page_size_};
                    ProcessVmaBudget::account_for_growth(2);
                    ::munlock(reinterpret_cast<void*>(entry->first), page_size_);
#endif
                    resident_pages_.erase(entry);
                }
            }
            throw;
        }
    }
    return std::shared_ptr<std::vector<std::uintptr_t> const>(selected.release(),
        [owner = std::move(owner)](auto* pages) noexcept { owner->release_resident(*pages); delete pages; });
}

void persisted_data_store_detail::BufferedPageAllocator::release_resident(std::span<std::uintptr_t const> pages) noexcept
{
    std::lock_guard lock{mutex_};
    for (auto page : pages) {
        auto entry = resident_pages_.find(page);
        assert(entry != resident_pages_.end());
        if (!--entry->second) {
#if defined(__linux__)
            PlaybackPreparationMetrics::Interval unlock_page{PlaybackPreparationPhase::munlock_call, page_size_};
            ProcessVmaBudget::account_for_growth(2);
            ::munlock(reinterpret_cast<void*>(page), page_size_);
#endif
            resident_pages_.erase(entry);
        }
    }
}

std::shared_ptr<void const> persisted_data_store_detail::BufferedPageAllocator::retain_event_bytes(std::size_t bytes)
{
    if (!bytes) return {};
    auto owner = shared_from_this();
    {
        std::lock_guard lock{mutex_};
        reserve_backing(bytes);
        event_bytes_ += bytes;
    }
    return std::shared_ptr<void const>(nullptr, [owner = std::move(owner), bytes](void const*) noexcept {
        std::lock_guard lock{owner->mutex_};
        owner->event_bytes_ -= bytes;
    });
}

void persisted_data_store_detail::BufferedPageAllocator::read(
    BufferedPage const& page,
    std::span<std::byte> target) const
{
    PlaybackPreparationMetrics::Interval timing{PlaybackPreparationPhase::read_backing_page, target.size()};
    if (target.size() != page_size_) {
        throw std::invalid_argument("buffered page read has the wrong size");
    }
#if defined(__linux__)
    std::size_t read_bytes = 0;
    while (read_bytes != target.size()) {
        auto const result = ::pread(
            file_,
            target.data() + read_bytes,
            target.size() - read_bytes,
            static_cast<off_t>(page.file_offset + read_bytes));
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(
                errno, std::generic_category(), "cannot read a buffered page");
        }
        if (result == 0) {
            throw std::runtime_error("buffered page storage ended unexpectedly");
        }
        read_bytes += static_cast<std::size_t>(result);
    }
#else
    (void)page;
#endif
}

std::shared_ptr<persisted_data_store_detail::BufferedPage const>
persisted_data_store_detail::BufferedPageAllocator::allocate(
    std::span<std::byte const> contents)
{
    PlaybackPreparationMetrics::Interval timing{PlaybackPreparationPhase::allocate_backing_page, contents.size()};
    if (contents.size() != page_size_) {
        throw std::invalid_argument("buffered page write has the wrong size");
    }
#if defined(__linux__)
    auto owner = shared_from_this();
    BufferedPage* page;
    {
        std::lock_guard lock{mutex_};
        auto const* cached = first_free_page(free_);
        auto const* empty = first_free_page(empty_slots_);
        if (cached && (!empty || cached->file_offset < empty->file_offset)) {
            page = take_first_free_page(free_);
            --free_pages_;
            ++reused_pages_;
        } else {
            reserve_backing(page_size_);
            if (empty_slots_) {
                page = take_first_free_page(empty_slots_);
                --free_pages_;
                ++reused_pages_;
            } else {
                if (next_file_offset_ > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - page_size_)
                    throw std::length_error("buffered page file offset limit exceeded");
                auto created = std::make_unique<BufferedPage>(BufferedPage{.file_offset = next_file_offset_});
                auto const new_size = next_file_offset_ + page_size_;
                if (file_ < 0) {
                    file_ = ::memfd_create("iv-buffered-output", MFD_CLOEXEC);
                    if (file_ < 0)
                        throw std::system_error(errno, std::generic_category(), "cannot create buffered page storage");
                }
                if (::ftruncate(file_, static_cast<off_t>(new_size)) != 0)
                    throw std::system_error(errno, std::generic_category(), "cannot extend buffered page storage");
                next_file_offset_ = new_size;
                page = created.release();
            }
            ++backed_pages_;
        }
        ++live_pages_;
        try {
            PlaybackPreparationMetrics::Interval write{PlaybackPreparationPhase::write_backing_page, contents.size()};
            std::size_t written = 0;
            while (written != contents.size()) {
                auto const result = ::pwrite(file_, contents.data() + written, contents.size() - written,
                    static_cast<off_t>(page->file_offset + written));
                if (result < 0) {
                    if (errno == EINTR) continue;
                    throw std::system_error(errno, std::generic_category(), "cannot write a buffered page");
                }
                if (result == 0) throw std::runtime_error("buffered page write made no progress");
                written += static_cast<std::size_t>(result);
            }
        } catch (...) {
            free_ = insert_free_page(free_, page);
            --live_pages_;
            ++free_pages_;
            throw;
        }
    }
    // The intrusive free trees allocate no metadata on last release. Reusing
    // their lowest file offsets preserves consecutive runs for dense writes.
    return std::shared_ptr<BufferedPage const>(page,
        [owner = std::move(owner)](BufferedPage const* released) noexcept { owner->release(released); });
#else
    throw std::runtime_error("buffered page storage requires Linux virtual memory");
#endif
}
} // namespace iv
