#pragma once

#include <intravenous/graph_jit/tick_invocation_call.h>
#include <intravenous/execution/storage/async_capacity_manager.h>
#include <intravenous/execution/storage/persisted_data_store.h>
#include <intravenous/execution/project_playback.h>
#include <intravenous/execution/storage/realtime_persisted_state.h>
#include <intravenous/execution/tick_materialization_snapshot.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace iv::graph_jit {
struct TickInvocationSlotPlan;
}

namespace iv {

class DeferredTickInputRequests;

struct RealtimeProducerRequirement {
    PersistedOutputHandle output{};
    graph_jit::BackgroundPortIndex port = 0;
    PortKind kind = PortKind::sample;
    std::size_t maximum_blocks_per_callback = 0;
};

// One prepared range's completed recording records. Their producer reserve
// must outlive this owner. The worker can move on after handing these records
// to the range; only the device publishes them after consuming it.
class TickRecordingBatch {
    struct Output {
        ProducedBlockChain chain{};
        PendingQueue* pending = nullptr;
        std::atomic<bool>* reservation_failed = nullptr;
    };

    std::vector<Output> outputs_{};
    friend class TickInvocationWorkspace;

public:
    TickRecordingBatch() = default;
    explicit TickRecordingBatch(std::size_t output_count) : outputs_(output_count) {}
    TickRecordingBatch(TickRecordingBatch const&) = delete;
    TickRecordingBatch& operator=(TickRecordingBatch const&) = delete;
    TickRecordingBatch(TickRecordingBatch&&) noexcept = default;
    TickRecordingBatch& operator=(TickRecordingBatch&&) noexcept = default;
    void publish() noexcept;
    void discard() noexcept;
};

// Control-path allocation and identity resolution for the fixed Tick binding
// slots retained by one CompiledGraph generation. Its arrays and bounded
// Sequential playback windows remain address-stable for the realization's
// lifetime. A window holds only the current block; a consumer's Sequential
// history is node-owned realtime state.
class TickInvocationWorkspace {
    class Impl;
    class CaptureScope {
        Impl* impl_ = nullptr;
        graph_jit::TickInvocationSlotPlan const* slots_ = nullptr;
        TickRecordingBatch* batch_ = nullptr;

        explicit CaptureScope(Impl& impl, bool finish, graph_jit::TickInvocationSlotPlan const* slots,
            TickRecordingBatch* batch) noexcept;
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
    [[nodiscard]] CaptureScope begin_capture(bool finish = true, graph_jit::TickInvocationSlotPlan const* slots = nullptr,
        TickRecordingBatch* batch = nullptr) noexcept;
    [[nodiscard]] graph_jit::TickInvocationCall bind(
        PersistedDataStore::Snapshot const& published,
        TickMaterializationSnapshot const& materialized,
        GlobalIndex sample_index,
        std::size_t block_size, TickPlaybackSelection selection,
        TickMaterializationSnapshot const* inherited_deferred = nullptr,
        graph_jit::TickInvocationSlotPlan const* slots = nullptr) noexcept;

public:
    TickInvocationWorkspace(
        graph_jit::BackgroundEvaluationPlan const& plan,
        std::uint64_t generation,
        PersistedDataStore& data,
        std::size_t maximum_block_size = 1,
        graph_jit::SequentialInputCombineFunction combine_sequential_input =
            nullptr,
        graph_jit::SequentialInputInitializeFunction
            initialize_sequential_input = nullptr);
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
    void bind_deferred_input_requests(DeferredTickInputRequests& requests,
        GlobalIndex execution_begin,
        graph_jit::TickInvocationSlotPlan const* slots = nullptr) noexcept;
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

    void prepare_deferred_input_transfer(
        std::span<std::optional<graph_jit::BackgroundPortIndex> const> successor_ports) noexcept;
    void prepare_recording_transfer_from(TickInvocationWorkspace const& previous,
        graph_jit::BackgroundEvaluationPlan const& plan,
        std::span<NodeBundlePortId const> inherited_outputs);
    void transfer_recording_from(TickInvocationWorkspace const& previous) noexcept;
    void discard_unpublished_startup() noexcept;
};

// Callback-scoped owner for the narrow generated Tick invocation record.
// GraphExecutor supplies one mailbox-adopted coherent root; direct component
// callers may instead supply the two pre-registered reader slots. Construction
// refreshes preallocated Sequential playback storage for the requested window.
class TickInvocationFrame {
    PersistedDataStore::ReaderPin published_data_{};
    TickMaterializationStore::ReaderPin materialized_storage_{};
    PersistedDataStore::Snapshot const* published_data_view_ = nullptr;
    TickMaterializationSnapshot const* materialized_storage_view_ = nullptr;
    TickInvocationWorkspace::CaptureScope capture_scope_;
    graph_jit::TickInvocationCall call_{};

public:
    explicit TickInvocationFrame(
        PersistedDataStore::ReaderSlot& data_reader,
        TickMaterializationStore::ReaderSlot& materialization_reader,
        TickInvocationWorkspace& workspace,
        GlobalIndex sample_index = {},
        std::size_t block_size = 0, TickPlaybackSelection selection = {}) noexcept;

    // GraphExecutor uses one already-adopted coherent root for the entire
    // realtime pass. Its mailbox retains the root until a later pass boundary.
    explicit TickInvocationFrame(
        RealtimePersistedState const& persisted,
        TickInvocationWorkspace& workspace,
        GlobalIndex sample_index = {},
        std::size_t block_size = 0, TickPlaybackSelection selection = {},
        bool finish_captures = true,
        graph_jit::TickInvocationSlotPlan const* slots = nullptr,
        TickRecordingBatch* recording_batch = nullptr) noexcept;

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
    [[nodiscard]] PersistedDataStore::Snapshot const& published_data()
        const noexcept
    {
        return *published_data_view_;
    }

    [[nodiscard]] TickMaterializationSnapshot const& materialized_storage()
        const noexcept
    {
        return *materialized_storage_view_;
    }
};

} // namespace iv
