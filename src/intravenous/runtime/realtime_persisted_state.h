#pragma once

#include <intravenous/runtime/persisted_page_store.h>
#include <intravenous/runtime/tick_materialization_snapshot.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace iv {

// One immutable background-produced view that pins the exact canonical page
// root and Tick-materialization root intended to become visible together.
// Construction and destruction are non-realtime operations.
class RealtimePersistedState {
    friend class RealtimePersistedStateMailbox;

    std::uint64_t generation_ = 0;
    PersistedPageStore::ReaderSlot page_slot_{};
    PersistedPageStore::ReaderPin pages_{};
    TickMaterializationStore::ReaderSlot materialization_slot_{};
    TickMaterializationStore::ReaderPin materialization_{};
    std::unique_ptr<TickMaterializationSnapshot> owned_materialization_{};
    RealtimePersistedState* returned_next_ = nullptr;
    bool captured_ = false;

    RealtimePersistedState(
        std::uint64_t generation,
        PersistedPageStore::ReaderSlot page_slot,
        TickMaterializationStore::ReaderSlot materialization_slot) noexcept;
    RealtimePersistedState(
        std::uint64_t generation,
        PersistedPageStore::ReaderSlot page_slot) noexcept;

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
        PersistedPageStore& pages,
        TickMaterializationStore& materializations);

    // Allocates/registers the owner of a generation's empty initial
    // materialization during staging. capture_initial() later pins the newest
    // canonical page root using no allocation.
    [[nodiscard]] static std::unique_ptr<RealtimePersistedState>
    prepare_initial(
        std::uint64_t generation,
        PersistedPageStore& pages);

    [[nodiscard]] bool capture_initial() noexcept;

    [[nodiscard]] std::expected<void, std::string>
    capture_current();

    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return generation_;
    }

    [[nodiscard]] PersistedPageStore::Snapshot const& pages() const noexcept
    {
        assert(captured_);
        return pages_.snapshot();
    }

    [[nodiscard]] TickMaterializationSnapshot const& materialization()
        const noexcept
    {
        assert(captured_);
        return owned_materialization_
            ? *owned_materialization_
            : materialization_.snapshot();
    }
};

// Single-background-producer/single-realtime-consumer latest-value mailbox.
// Realtime adopts at a pass boundary and returns the previous active owner
// through an allocation-free intrusive stack. Only background reclamation
// destroys roots and releases their underlying page/materialization pins.
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
    [[nodiscard]] RealtimePersistedState const* adopt(
        std::uint64_t generation) noexcept;

    // Background/control operation after realtime has left the previous pass.
    [[nodiscard]] std::size_t reclaim_returned() noexcept;
};

} // namespace iv
