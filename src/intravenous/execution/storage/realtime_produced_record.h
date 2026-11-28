#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/ports.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace iv {

// One transport block size shared by prepared realtime producers. Payloads
// larger than this remain one semantic record spanning several physical blocks.
inline constexpr std::size_t realtime_produced_block_storage_size = 64 * 1024;

enum class RealtimeProducedPayloadKind : std::uint8_t {
    continuous_values,
    events,
    void_value,
    append_events_in_gaps,
};

struct RealtimeProductionFailures {
    bool insufficient_reserve_capacity = false;

    [[nodiscard]] bool any() const noexcept
    {
        return insufficient_reserve_capacity;
    }
};

// The header and payload form a byte stream across the callback's private
// blocks. InputRoute supplies destination identity. payload_size identifies
// the next header even when it begins inside a block.
struct RealtimeProducedRecordHeader {
    RealtimeProducedPayloadKind payload_kind =
        RealtimeProducedPayloadKind::continuous_values;
    std::size_t payload_size = 0;
    GlobalIndex begin{};
    GlobalIndex::extent_type range_extent = 0;
    ChannelLayout continuous_layout{};
    ContinuousValueStorageGeometry continuous_geometry{};
    EventTypeId event_type = EventTypeId::empty;
    std::size_t event_count = 0;
};

static_assert(std::is_trivially_copyable_v<RealtimeProducedRecordHeader>);
static_assert(
    sizeof(RealtimeProducedRecordHeader)
    <= realtime_produced_block_storage_size);

} // namespace iv
