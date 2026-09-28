#pragma once

#include <intravenous/graph_jit/tick_invocation_call.h>
#include <intravenous/runtime/persisted_page_store.h>
#include <intravenous/runtime/tick_materialization_snapshot.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace iv {

// Control-path allocation and identity resolution for the fixed Tick binding
// slots retained by one CompiledGraph generation. Its arrays and bounded
// Sequential playback rings remain address-stable for the realization's
// lifetime.
class TickInvocationWorkspace {
    class Impl;
    std::unique_ptr<Impl> impl_{};

    friend class TickInvocationFrame;
    [[nodiscard]] graph_jit::TickInvocationCall bind(
        PersistedPageStore::Snapshot const& published,
        TickMaterializationSnapshot const& materialized,
        SampleIndex sample_index,
        std::size_t block_size) noexcept;

public:
    TickInvocationWorkspace(
        graph_jit::BackgroundEvaluationPlan const& plan,
        std::uint64_t generation,
        std::size_t maximum_block_size = 1);
    ~TickInvocationWorkspace();

    TickInvocationWorkspace(TickInvocationWorkspace const&) = delete;
    TickInvocationWorkspace& operator=(TickInvocationWorkspace const&) = delete;
    TickInvocationWorkspace(TickInvocationWorkspace&&) = delete;
    TickInvocationWorkspace& operator=(TickInvocationWorkspace&&) = delete;

    [[nodiscard]] std::size_t sequential_sample_count() const noexcept;
    [[nodiscard]] std::size_t sequential_event_count() const noexcept;
    [[nodiscard]] std::size_t random_access_sample_count() const noexcept;
    [[nodiscard]] std::size_t random_access_event_count() const noexcept;
};

// Callback-scoped owner for the narrow generated Tick invocation record.
// Construction performs the bounded atomic pin operations of the two reader
// slots and refreshes preallocated Sequential playback storage for the requested
// callback window; both slots are registered by GraphExecutor on the control path.
class TickInvocationFrame {
    PersistedPageStore::ReaderPin published_pages_{};
    TickMaterializationStore::ReaderPin materialized_storage_{};
    graph_jit::TickInvocationCall call_{};

public:
    explicit TickInvocationFrame(
        PersistedPageStore::ReaderSlot& page_reader,
        TickMaterializationStore::ReaderSlot& materialization_reader,
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
        return published_pages_.snapshot();
    }

    [[nodiscard]] TickMaterializationSnapshot const& materialized_storage()
        const noexcept
    {
        return materialized_storage_.snapshot();
    }
};

} // namespace iv
