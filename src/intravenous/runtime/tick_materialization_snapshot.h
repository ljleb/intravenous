#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/coverage.h>
#include <intravenous/graph_jit/background_evaluation_plan.h>
#include <intravenous/node/coverage_port_context.h>
#include <intravenous/ports.h>
#include <intravenous/runtime/persisted_data_store.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace iv {

// Calls fn for each maximal piece of left & right without allocating. On the
// circular timeline every such piece begins at left.begin or right.begin, so
// there are at most two.
template<typename Fn>
constexpr void for_each_range_intersection(
    GlobalIndexRange left, GlobalIndexRange right, Fn&& fn)
{
    if (left.empty() || right.empty()) return;
    if (auto const offset = left.offset_of(right.begin)) {
        fn(GlobalIndexRange{
            .begin = right.begin,
            .extent = std::min(left.extent - *offset, right.extent),
        });
    }
    if (left.begin == right.begin) return;
    if (auto const offset = right.offset_of(left.begin)) {
        fn(GlobalIndexRange{
            .begin = left.begin,
            .extent = std::min(right.extent - *offset, left.extent),
        });
    }
}

// One immutable contiguous logical Region. Successive snapshots share whole
// records. Carry-forward never clips one Region into transaction-sized pieces.
struct TickMaterializedSampleRegion {
    GlobalIndexRange range{};
    // Native-layout values with storage_frames frames. range.begin is frame
    // first_frame of that storage when an owner is shared across snapshots.
    std::shared_ptr<std::vector<Sample> const> values{};
    std::shared_ptr<PersistedContinuousStorage const> continuous_values{};
    std::size_t storage_frames = 0;
    std::size_t first_frame = 0;

    [[nodiscard]] std::byte const* storage_data() const noexcept
    {
        return values
            ? reinterpret_cast<std::byte const*>(values->data())
            : continuous_values ? continuous_values->data() : nullptr;
    }

};

struct TickMaterializedSampleInput {
    graph_jit::BackgroundPortIndex port = 0;
    ChannelLayout layout{};
    ContinuousValueStorageGeometry geometry =
        *continuous_value_storage_geometry(
            continuous_value_type_id_v<Sample>, std::nullopt);
    std::vector<std::size_t> channels{};
    // Logical producer order. Records may overlap or be exact duplicates;
    // coverage is their positional union.
    std::vector<TickMaterializedSampleRegion> regions{};
    // Node-facing descriptors in the same logical order as regions. They
    // point into the shared immutable owners above and preserve overlapping
    // or duplicate logical Regions.
    std::vector<ContinuousRegionStorageView> region_views{};
    IndexCoverage coverage{};

    [[nodiscard]] bool has_channel(std::size_t channel) const noexcept;
    [[nodiscard]] Sample at(
        GlobalIndex index, std::size_t channel) const noexcept;
    // Position of channel in channels, or channels.size().
    [[nodiscard]] std::size_t channel_slot(std::size_t channel) const noexcept;

    // Visits every record that intersects range with that intersection.
    // Allocation-free, so realtime playback may use it.
    template<typename Fn>
    void for_each_region(GlobalIndexRange range, Fn&& fn) const
    {
        for (auto const& region : regions) {
            for_each_range_intersection(
                region.range, range,
                [&](GlobalIndexRange part) { fn(region, part); });
        }
    }
};

// One immutable materialized event range over shared event storage. The
// events of range are [begin, end) of the shared sequence.
struct TickMaterializedEventChunk {
    GlobalIndexRange range{};
    // Absolute event times in nondecreasing semantic order.
    std::shared_ptr<std::vector<TimedEvent> const> events{};
    // Parallel semantic source positions for deterministic equal-time merging.
    // max(size_t) means that the source has no multi-source identity.
    std::shared_ptr<std::vector<std::size_t> const> semantic_source_indices{};
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct TickMaterializedEventInput {
    using VisitIndexedEvent = void (*)(
        void*, TimedEvent const&, std::size_t);
    graph_jit::BackgroundPortIndex port = 0;
    EventTypeId type = EventTypeId::empty;
    // Disjoint chunks in signed-canonical order. coverage is their union.
    std::vector<TickMaterializedEventChunk> chunks{};
    IndexCoverage coverage{};

    void for_each(
        GlobalIndexRange range,
        void* visitor_data,
        RandomAccessEventInputPort::VisitEvent visitor) const;
    void for_each_indexed(
        GlobalIndexRange range,
        void* visitor_data,
        VisitIndexedEvent visitor) const;
};

// Builds one input from Region-major native-layout values. Each Region owns
// one independent contiguous payload even when logical ranges overlap.
[[nodiscard]] TickMaterializedSampleInput make_tick_materialized_sample_input(
    graph_jit::BackgroundPortIndex port,
    ChannelLayout layout,
    std::vector<std::size_t> channels,
    std::vector<GlobalIndexRange> regions,
    std::vector<Sample> values);

[[nodiscard]] TickMaterializedSampleInput
make_tick_materialized_continuous_input(
    graph_jit::BackgroundPortIndex port,
    ChannelLayout layout,
    ContinuousValueStorageGeometry geometry,
    std::vector<std::size_t> channels,
    std::vector<GlobalIndexRange> regions,
    std::vector<std::byte> values);

// Compatibility helper for callers whose source is positional coverage and
// therefore has no overlapping or duplicate Region decomposition.
[[nodiscard]] TickMaterializedSampleInput make_tick_materialized_sample_input(
    graph_jit::BackgroundPortIndex port,
    ChannelLayout layout,
    std::vector<std::size_t> channels,
    IndexCoverage coverage,
    std::vector<Sample> values);

// Builds one input from events and their semantic source indices in coverage's
// signed-canonical iteration order.
[[nodiscard]] TickMaterializedEventInput make_tick_materialized_event_input(
    graph_jit::BackgroundPortIndex port,
    EventTypeId type,
    IndexCoverage coverage,
    std::vector<TimedEvent> events,
    std::vector<std::size_t> semantic_source_indices);

// The part of a previous snapshot's input that is still valid and still
// demanded. Ports are BackgroundEvaluationPlan ports of one generation.
struct TickMaterializationRetention {
    graph_jit::BackgroundPortIndex port = 0;
    IndexCoverage retained{};
};

// One immutable set of transaction-produced Tick-visible storage. It is not a
// canonical persisted-data version: generation identifies the compiled plan,
// while data_version identifies the exact published root against which it was
// built.
class TickMaterializationSnapshot {
    friend class TickMaterializationStore;
    friend class RealtimePersistedState;

    std::uint64_t generation_ = 0;
    std::uint64_t semantic_version_ = 0;
    std::uint64_t promotion_sequence_ = 0;
    PersistedDataVersion data_version_{};
    std::vector<TickMaterializedSampleInput> samples_{};
    std::vector<TickMaterializedEventInput> events_{};
    std::unique_ptr<TickMaterializationSnapshot> retired_next_{};

    void validate() const;

public:
    TickMaterializationSnapshot() = default;
    TickMaterializationSnapshot(
        std::uint64_t generation,
        std::uint64_t semantic_version,
        PersistedDataVersion data_version,
        std::vector<TickMaterializedSampleInput> samples,
        std::vector<TickMaterializedEventInput> events);

    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return generation_;
    }
    [[nodiscard]] std::uint64_t semantic_version() const noexcept
    {
        return semantic_version_;
    }
    [[nodiscard]] std::uint64_t promotion_sequence() const noexcept
    {
        return promotion_sequence_;
    }
    [[nodiscard]] PersistedDataVersion data_version() const noexcept
    {
        return data_version_;
    }
    [[nodiscard]] std::size_t sample_count() const noexcept
    {
        return samples_.size();
    }
    [[nodiscard]] std::size_t event_count() const noexcept
    {
        return events_.size();
    }

    [[nodiscard]] TickMaterializedSampleInput const* find_sample(
        graph_jit::BackgroundPortIndex port) const noexcept;
    [[nodiscard]] TickMaterializedEventInput const* find_event(
        graph_jit::BackgroundPortIndex port) const noexcept;

    // Adds the retained parts of previous, which must belong to the same
    // generation, to this newly produced snapshot. Retained records share
    // previous's storage. This snapshot's own coverage of a port supersedes
    // any overlapping retained range.
    void carry_forward(
        TickMaterializationSnapshot const& previous,
        std::span<TickMaterializationRetention const> retention);
};

// Single-writer, pre-registered-reader publication for Tick materializations.
// Promotion only relinks already-owned nodes and is noexcept. Retired owners
// are destroyed solely by explicit non-audio reclamation or store destruction.
class TickMaterializationStore {
public:
    class ReaderSlot;

private:
    struct ReaderSlotState {
        std::atomic<bool> acquiring{false};
        std::atomic<TickMaterializationSnapshot const*> pinned{nullptr};
    };

    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(
        std::atomic<TickMaterializationSnapshot const*>::is_always_lock_free);

public:
    class ReaderPin {
        friend class ReaderSlot;

        ReaderSlotState* slot_ = nullptr;
        TickMaterializationSnapshot const* snapshot_ = nullptr;

        explicit ReaderPin(
            ReaderSlotState& slot,
            std::atomic<TickMaterializationSnapshot const*> const& published)
            noexcept;
        void reset() noexcept;

    public:
        ReaderPin() = default;
        ~ReaderPin();
        ReaderPin(ReaderPin const&) = delete;
        ReaderPin& operator=(ReaderPin const&) = delete;
        ReaderPin(ReaderPin&& other) noexcept;
        ReaderPin& operator=(ReaderPin&& other) noexcept;

        [[nodiscard]] TickMaterializationSnapshot const& snapshot()
            const noexcept;
        [[nodiscard]] TickMaterializationSnapshot const* operator->()
            const noexcept
        {
            return snapshot_;
        }
    };

    class ReaderSlot {
        friend class TickMaterializationStore;

        TickMaterializationStore* store_ = nullptr;
        ReaderSlotState* state_ = nullptr;

        ReaderSlot(
            TickMaterializationStore& store,
            ReaderSlotState& state) noexcept;
        void reset() noexcept;

    public:
        ReaderSlot() = default;
        ~ReaderSlot();
        ReaderSlot(ReaderSlot const&) = delete;
        ReaderSlot& operator=(ReaderSlot const&) = delete;
        ReaderSlot(ReaderSlot&& other) noexcept;
        ReaderSlot& operator=(ReaderSlot&& other) noexcept;

        [[nodiscard]] ReaderPin pin() noexcept;
    };

private:
    std::unique_ptr<TickMaterializationSnapshot> published_owner_{};
    std::atomic<TickMaterializationSnapshot const*> published_{nullptr};
    std::unique_ptr<TickMaterializationSnapshot> retired_{};
    mutable std::mutex reader_slots_mutex_{};
    std::vector<std::unique_ptr<ReaderSlotState>> reader_slots_{};
    std::uint64_t promotion_sequence_ = 0;

    void unregister_reader(ReaderSlotState& state) noexcept;

public:
    TickMaterializationStore();
    ~TickMaterializationStore();

    TickMaterializationStore(TickMaterializationStore const&) = delete;
    TickMaterializationStore& operator=(
        TickMaterializationStore const&) = delete;
    TickMaterializationStore(TickMaterializationStore&&) = delete;
    TickMaterializationStore& operator=(TickMaterializationStore&&) = delete;

    [[nodiscard]] std::uint64_t promote(
        std::unique_ptr<TickMaterializationSnapshot> snapshot) noexcept;
    // The currently published snapshot, for the single writer that promotes
    // snapshots. Readers must pin through a ReaderSlot instead.
    [[nodiscard]] TickMaterializationSnapshot const* published_for_writer()
        const noexcept
    {
        return published_owner_.get();
    }
    [[nodiscard]] ReaderSlot register_reader();
    [[nodiscard]] std::size_t reclaim_retired();
    [[nodiscard]] std::size_t retired_snapshot_count() const noexcept;
};

} // namespace iv
