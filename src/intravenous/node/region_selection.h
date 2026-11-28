#pragma once

#include <intravenous/ports.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace iv {

struct IndexRun {
    std::uint64_t first = 0;
    std::uint64_t count = 0;
    std::uint64_t step = 1;

    [[nodiscard]] constexpr bool fits(std::uint64_t size) const noexcept
    {
        return count == 0 || (step != 0 && first < size && count - 1 <= (size - 1 - first) / step);
    }

    constexpr bool operator==(IndexRun const&) const noexcept = default;
};

namespace details { struct RegionRefAccess; }

class RegionRef {
    std::uint64_t snapshot_ = 0;
    std::size_t port_ = 0;
    std::size_t index_ = 0;

    friend struct details::RegionRefAccess;

public:
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return snapshot_ != 0; }
    constexpr bool operator==(RegionRef const&) const noexcept = default;
};

namespace details {
struct RegionRefAccess {
    static constexpr RegionRef make(std::uint64_t snapshot, std::size_t port, std::size_t index) noexcept
    {
        RegionRef ref;
        ref.snapshot_ = snapshot;
        ref.port_ = port;
        ref.index_ = index;
        return ref;
    }
    static constexpr std::uint64_t snapshot(RegionRef ref) noexcept { return ref.snapshot_; }
    static constexpr std::size_t port(RegionRef ref) noexcept { return ref.port_; }
    static constexpr std::size_t index(RegionRef ref) noexcept { return ref.index_; }
};
} // namespace details

struct RegionSelection {
    RegionRef region;
    std::span<IndexRun const> runs;
};

struct IndexedEventRegionInfo {
    std::uint64_t event_count = 0;
    std::optional<Event> constant_value{};
    bool operator==(IndexedEventRegionInfo const&) const noexcept = default;
};

struct EventRegionInfo {
    GlobalIndexRange range;
    std::optional<IndexedEventRegionInfo> index{};
    bool operator==(EventRegionInfo const&) const noexcept = default;
};

struct ResolvedRegionInfo {
    RegionRef region;
    GlobalIndexRange range;
    std::optional<IndexedEventRegionInfo> event_index{};
};

struct EventRunStorageView {
    RegionRef region;
    GlobalIndexRange range;
    IndexRun run;
    std::span<TimedEvent> slots;
    std::span<std::uint8_t> written;
    Event const* constant_value = nullptr;
    EventTypeId type = EventTypeId::empty;
};

namespace details {
inline TimedEvent const* read_prepared_event(std::span<EventRunStorageView const> runs,
    std::size_t region, std::uint64_t index) noexcept
{
    for (auto const& prepared : runs) {
        if (RegionRefAccess::index(prepared.region) != region || index < prepared.run.first) continue;
        auto const offset = index - prepared.run.first;
        auto const slot = offset / prepared.run.step;
        if (offset % prepared.run.step == 0 && slot < prepared.slots.size() && prepared.written[slot])
            return &prepared.slots[slot];
    }
    return nullptr;
}

template<bool Writable>
class PreparedEventRun {
    EventRunStorageView const* storage_;

public:
    explicit PreparedEventRun(EventRunStorageView const& storage) noexcept : storage_(&storage) {}
    [[nodiscard]] RegionRef region() const noexcept { return storage_->region; }
    [[nodiscard]] GlobalIndexRange region_range() const noexcept { return storage_->range; }
    [[nodiscard]] std::size_t size() const noexcept { return storage_->slots.size(); }
    [[nodiscard]] Event const* constant_value() const noexcept { return storage_->constant_value; }
    [[nodiscard]] std::uint64_t index(std::size_t slot) const noexcept
    {
        IV_ASSERT(slot < size(), "event run slot is out of bounds");
        return storage_->run.first + slot * storage_->run.step;
    }

    [[nodiscard]] std::span<TimedEvent const> events() const noexcept
    requires (!Writable)
    {
        return storage_->slots;
    }

    void write(std::size_t slot, TimedEvent event) const
    requires Writable
    {
        if (slot >= size() || storage_->written[slot] || !storage_->range.contains(event.time)
            || !event_matches_type(event.value, storage_->type))
            throw std::logic_error("invalid indexed event write");
        if (storage_->constant_value && event.value != *storage_->constant_value)
            throw std::logic_error("indexed event write differs from its Region payload");
        storage_->slots[slot] = std::move(event);
        storage_->written[slot] = 1;
    }

    void write_time(std::size_t slot, GlobalIndex time) const
    requires Writable
    {
        if (!storage_->constant_value) throw std::logic_error("indexed event Region has no constant payload");
        write(slot, {time, *storage_->constant_value});
    }
};
} // namespace details

} // namespace iv
