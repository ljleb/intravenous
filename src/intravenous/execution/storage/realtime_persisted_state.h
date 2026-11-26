#pragma once

#include <intravenous/execution/storage/persisted_data_store.h>
#include <intravenous/execution/tick_materialization_snapshot.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace iv {

// One immutable background-produced view of a data version and its prepared
// payload leases. Unrelated canonical cache contents are not playback owners.
// Construction and destruction are non-realtime operations.
class RealtimePersistedState {
    friend class RealtimePersistedStateMailbox;

    std::uint64_t generation_ = 0;
    PersistedDataStore::ReaderSlot data_slot_{};
    PersistedDataStore::ReaderPin data_{};
    TickMaterializationStore::ReaderSlot materialization_slot_{};
    TickMaterializationStore::ReaderPin materialization_{};
    std::shared_ptr<TickMaterializationSnapshot const> owned_materialization_{};
    std::unique_ptr<TickMaterializationSnapshot> initial_materialization_{};
    std::unique_ptr<PersistedDataStore::Snapshot> initial_data_{};
    std::shared_ptr<PersistedDataStore::Snapshot const> owned_data_{};
    RealtimePersistedState* returned_next_ = nullptr;
    bool captured_ = false;
    IndexCoverage prepared_playback_{};

    RealtimePersistedState(
        std::uint64_t generation,
        PersistedDataStore::ReaderSlot data_slot,
        TickMaterializationStore::ReaderSlot materialization_slot) noexcept;
    RealtimePersistedState(
        std::uint64_t generation,
        PersistedDataStore::ReaderSlot data_slot) noexcept;

public:
    ~RealtimePersistedState() = default;
    RealtimePersistedState(RealtimePersistedState const&) = delete;
    RealtimePersistedState& operator=(RealtimePersistedState const&) = delete;
    RealtimePersistedState(RealtimePersistedState&&) = delete;
    RealtimePersistedState& operator=(RealtimePersistedState&&) = delete;

    // Allocates/registers every owner needed by the root before a domain
    // transaction commits. capture_current() later performs only bounded
    // atomic pins and validation.
    [[nodiscard]] static std::unique_ptr<RealtimePersistedState> prepare(
        std::uint64_t generation,
        PersistedDataStore& data,
        TickMaterializationStore& materializations);

    // Allocates/registers the owner of a generation's empty initial
    // materialization during staging. capture_initial() later captures the
    // newest data version without retaining unrequested payload or allocating.
    [[nodiscard]] static std::unique_ptr<RealtimePersistedState>
    prepare_initial(
        std::uint64_t generation,
        PersistedDataStore& data);

    [[nodiscard]] static std::unique_ptr<RealtimePersistedState> prepare_owned(
        std::uint64_t generation,
        std::shared_ptr<PersistedDataStore::Snapshot const> data,
        std::shared_ptr<TickMaterializationSnapshot const> materialization);

    [[nodiscard]] bool capture_initial() noexcept;

    [[nodiscard]] std::expected<void, std::string>
    capture_current();

    // Background-only finalization. Tick materializations already lease their
    // payload; retain canonical pages only for bindings that read them directly.
    [[nodiscard]] std::expected<void, std::string> select_payload(
        graph_jit::BackgroundEvaluationPlan const& plan, PersistedDataStore& store);

    // Cold preparation sets the exact requested playback positions, including
    // known gaps. Realtime checks this range without allocating.
    void set_prepared_playback(IndexCoverage demand) { prepared_playback_ = std::move(demand); }
    [[nodiscard]] bool prepares(GlobalIndexRange range) const noexcept {
        return prepared_playback_.contains(range);
    }

    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return generation_;
    }

    // A private preview may precede writes drained from its predecessor. A
    // canonical playback view pins the published data and materialization.
    [[nodiscard]] bool is_canonical_playback() const noexcept
    {
        return captured_ && !owned_data_ && !initial_data_;
    }

    [[nodiscard]] PersistedDataStore::Snapshot const& data() const noexcept
    {
        assert(captured_);
        return owned_data_ ? *owned_data_ : initial_data_ ? *initial_data_ : data_.snapshot();
    }

    [[nodiscard]] TickMaterializationSnapshot const& materialization()
        const noexcept
    {
        assert(captured_);
        return owned_materialization_ ? *owned_materialization_
            : initial_materialization_ ? *initial_materialization_ : materialization_.snapshot();
    }
};

// Single-background-producer/single-realtime-consumer latest-value mailbox.
// Realtime adopts at a pass boundary and returns the previous active owner
// through an allocation-free intrusive stack. Only background reclamation
// destroys roots and releases their underlying data/materialization pins.
class RealtimePersistedStateMailbox {
    std::atomic<RealtimePersistedState*> pending_{nullptr};
    RealtimePersistedState* active_ = nullptr;
    std::atomic<RealtimePersistedState*> returned_{nullptr};

    void return_for_reclamation(RealtimePersistedState& state) noexcept;

public:
    RealtimePersistedStateMailbox();
    ~RealtimePersistedStateMailbox();

    RealtimePersistedStateMailbox(RealtimePersistedStateMailbox const&) = delete;
    RealtimePersistedStateMailbox& operator=(
        RealtimePersistedStateMailbox const&) = delete;

    // Publication replaces a still-pending root and queues the superseded owner
    // for explicit off-realtime reclamation. A root already adopted by realtime
    // is never touched here.
    void publish(std::unique_ptr<RealtimePersistedState> state);

    // Realtime-generation-cutover operation. Installs the successor's already
    // captured initial root directly as active state and returns both the old
    // active root and any pending old-generation publication for reclamation.
    void activate_generation(
        std::unique_ptr<RealtimePersistedState> initial_state);

    // Realtime-pass-boundary operation. An incompatible pending root is
    // returned for background destruction and can never replace the active
    // view of a different execution generation.
    // Transfer an already adopted initial selection to the executor's active
    // mailbox. Only the realtime consumer calls this; no allocation occurs.
    [[nodiscard]] std::unique_ptr<RealtimePersistedState> take_active() noexcept;
    // Producer-side transfer after the consumer has acknowledged initial
    // adoption. The initial mailbox then has no further realtime readers.
    [[nodiscard]] std::unique_ptr<RealtimePersistedState> take_pending() noexcept;

    [[nodiscard]] RealtimePersistedState const* adopt(
        std::uint64_t generation) noexcept;

    // Background/control operation after realtime has left the previous pass.
    [[nodiscard]] std::size_t reclaim_returned() noexcept;
};

} // namespace iv
