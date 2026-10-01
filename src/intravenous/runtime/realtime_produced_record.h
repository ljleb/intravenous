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
    samples,
    events,
    void_value,
};

struct RealtimeProductionFailures {
    bool insufficient_reserve_capacity = false;

    [[nodiscard]] bool any() const noexcept
    {
        return insufficient_reserve_capacity;
    }
};

// Stored at byte zero of the first block published for one logical record.
// InputRoute supplies destination identity, so it is deliberately absent here.
// Every record begins at a physical block boundary. record_block_count lets a
// background selection distinguish adjacent records after several record chains
// have been joined into one pass-level pending-queue publication.
struct RealtimeProducedRecordHeader {
    RealtimeProducedPayloadKind payload_kind =
        RealtimeProducedPayloadKind::samples;
    std::size_t record_block_count = 0;
    std::size_t payload_size = 0;
    SampleIndex begin = 0;
    std::size_t sample_count = 0;
    ChannelLayout sample_layout{};
    EventTypeId event_type = EventTypeId::empty;
    std::size_t event_count = 0;
};

static_assert(std::is_trivially_copyable_v<RealtimeProducedRecordHeader>);
static_assert(
    sizeof(RealtimeProducedRecordHeader)
    <= realtime_produced_block_storage_size);

} // namespace iv
