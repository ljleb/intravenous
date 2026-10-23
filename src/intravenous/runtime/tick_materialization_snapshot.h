#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/coverage.h>
#include <intravenous/graph_jit/background_evaluation_plan.h>
#include <intravenous/node/coverage_port_context.h>
#include <intravenous/ports.h>
#include <intravenous/runtime/persisted_data_store.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace iv {

struct TickMaterializedSampleInput {
    graph_jit::BackgroundPortIndex port = 0;
    IndexCoverage coverage{};
    ChannelLayout layout{};
    std::vector<std::size_t> channels{};
    // Channel-major values over coverage's signed-canonical iteration order.
    std::vector<Sample> values{};

    [[nodiscard]] bool has_channel(std::size_t channel) const noexcept;
    [[nodiscard]] Sample at(
        GlobalIndex index, std::size_t channel) const noexcept;
};

struct TickMaterializedEventInput {
    using VisitIndexedEvent = void (*)(
        void*, TimedEvent const&, std::size_t);
    graph_jit::BackgroundPortIndex port = 0;
    IndexCoverage coverage{};
    EventTypeId type = EventTypeId::empty;
    // Absolute event times in nondecreasing semantic order.
    std::vector<TimedEvent> events{};
    // Parallel semantic source positions for deterministic equal-time merging.
    // max(size_t) means that the source has no multi-source identity.
    std::vector<std::size_t> semantic_source_indices{};

    void for_each(
        GlobalIndexRange range,
        void* visitor_data,
        RandomAccessEventInputPort::VisitEvent visitor) const;
    void for_each_indexed(
        GlobalIndexRange range,
        void* visitor_data,
        VisitIndexedEvent visitor) const;
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
    [[nodiscard]] ReaderSlot register_reader();
    [[nodiscard]] std::size_t reclaim_retired();
    [[nodiscard]] std::size_t retired_snapshot_count() const noexcept;
};

} // namespace iv
