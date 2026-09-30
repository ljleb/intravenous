#pragma once

#include <intravenous/graph_jit/tick_invocation_call.h>
#include <intravenous/runtime/async_capacity_manager.h>
#include <intravenous/runtime/persisted_page_store.h>
#include <intravenous/runtime/persisted_tick_capture_registry.h>
#include <intravenous/runtime/realtime_persisted_state.h>
#include <intravenous/runtime/tick_materialization_snapshot.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace iv {

struct RealtimeProducerRequirement {
    PersistedOutputId output{};
    graph_jit::BackgroundPortIndex port = 0;
    PortKind kind = PortKind::sample;
    std::size_t maximum_blocks_per_callback = 0;
};

// Control-path allocation and identity resolution for the fixed Tick binding
// slots retained by one CompiledGraph generation. Its arrays and bounded
// Sequential playback rings remain address-stable for the realization's
// lifetime.
class TickInvocationWorkspace {
    class Impl;
    class CaptureScope {
        Impl* impl_ = nullptr;
        TickCaptureStore::CallbackScope legacy_{};

        CaptureScope(
            Impl& impl,
            TickCaptureStore::CallbackScope legacy) noexcept;
        friend class TickInvocationWorkspace;

    public:
        ~CaptureScope();
        CaptureScope(CaptureScope const&) = delete;
        CaptureScope& operator=(CaptureScope const&) = delete;
        CaptureScope(CaptureScope&&) = delete;
        CaptureScope& operator=(CaptureScope&&) = delete;
    };

    std::unique_ptr<Impl> impl_{};

    friend class TickInvocationFrame;
    [[nodiscard]] CaptureScope begin_capture() noexcept;
    [[nodiscard]] graph_jit::TickInvocationCall bind(
        PersistedPageStore::Snapshot const& published,
        TickMaterializationSnapshot const& materialized,
        SampleIndex sample_index,
        std::size_t block_size) noexcept;

public:
    TickInvocationWorkspace(
        graph_jit::BackgroundEvaluationPlan const& plan,
        std::uint64_t generation,
        std::size_t maximum_block_size = 1,
        PersistedTickCaptureRegistry* captures = nullptr);
    ~TickInvocationWorkspace();

    TickInvocationWorkspace(TickInvocationWorkspace const&) = delete;
    TickInvocationWorkspace& operator=(TickInvocationWorkspace const&) = delete;
    TickInvocationWorkspace(TickInvocationWorkspace&&) = delete;
    TickInvocationWorkspace& operator=(TickInvocationWorkspace&&) = delete;

    [[nodiscard]] std::size_t sequential_sample_count() const noexcept;
    [[nodiscard]] std::size_t sequential_event_count() const noexcept;
    [[nodiscard]] std::size_t random_access_sample_count() const noexcept;
    [[nodiscard]] std::size_t random_access_event_count() const noexcept;
    [[nodiscard]] std::size_t sample_capture_count() const noexcept;
    [[nodiscard]] std::size_t event_capture_count() const noexcept;
    // Temporary compatibility count for the legacy TickCaptureStore adapter.
    // Executor-created workspaces use producer_requirements() instead.
    [[nodiscard]] std::size_t maximum_capture_blocks_per_callback()
        const noexcept;
    // One entry per Tick/persisted producer queue which the prepared execution
    // generation must provision. Entries are aligned sample-first/event-second
    // with the workspace's capture-operation slots and include record-header
    // bytes plus every planned SCC-slice invocation.
    [[nodiscard]] std::span<RealtimeProducerRequirement const>
    producer_requirements() const noexcept;

    // Control-path endpoint binding performed while constructing a complete
    // execution generation. No generated capture operation becomes callable
    // until its matching reserve and pending queue have both been installed.
    void bind_producer_endpoint(
        std::size_t producer,
        ProducerReserve& reserve,
        PendingQueue& pending,
        std::atomic<bool>& reservation_failed);
};

// Callback-scoped owner for the narrow generated Tick invocation record.
// GraphExecutor supplies one mailbox-adopted coherent root; direct component
// callers may instead supply the two pre-registered reader slots. Construction
// refreshes preallocated Sequential playback storage for the requested window.
class TickInvocationFrame {
    PersistedPageStore::ReaderPin published_pages_{};
    TickMaterializationStore::ReaderPin materialized_storage_{};
    PersistedPageStore::Snapshot const* published_pages_view_ = nullptr;
    TickMaterializationSnapshot const* materialized_storage_view_ = nullptr;
    TickInvocationWorkspace::CaptureScope capture_scope_;
    graph_jit::TickInvocationCall call_{};

public:
    explicit TickInvocationFrame(
        PersistedPageStore::ReaderSlot& page_reader,
        TickMaterializationStore::ReaderSlot& materialization_reader,
        TickInvocationWorkspace& workspace,
        SampleIndex sample_index = 0,
        std::size_t block_size = 0) noexcept;

    // GraphExecutor uses one already-adopted coherent root for the entire
    // realtime pass. Its mailbox retains the root until a later pass boundary.
    explicit TickInvocationFrame(
        RealtimePersistedState const& persisted,
        TickInvocationWorkspace& workspace,
        SampleIndex sample_index = 0,
        std::size_t block_size = 0) noexcept;

    TickInvocationFrame(TickInvocationFrame const&) = delete;
    TickInvocationFrame& operator=(TickInvocationFrame const&) = delete;
    TickInvocationFrame(TickInvocationFrame&&) = delete;
    TickInvocationFrame& operator=(TickInvocationFrame&&) = delete;

    [[nodiscard]] graph_jit::TickInvocationCall const& call() const noexcept
    {
        return call_;
    }

    // Runtime binding construction may inspect the pinned root, but this owner
    // is never passed through the generated ABI.
    [[nodiscard]] PersistedPageStore::Snapshot const& published_pages()
        const noexcept
    {
        return *published_pages_view_;
    }

    [[nodiscard]] TickMaterializationSnapshot const& materialized_storage()
        const noexcept
    {
        return *materialized_storage_view_;
    }
};

} // namespace iv
