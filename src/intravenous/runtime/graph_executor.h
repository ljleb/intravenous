#pragma once

#include <intravenous/node/resources.h>
#include <intravenous/runtime/async_capacity_manager.h>
#include <intravenous/runtime/background_evaluation_transaction.h>
#include <intravenous/runtime/background_coverage_propagation.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/persisted_page_store.h>
#include <intravenous/runtime/realtime_produced_record.h>
#include <intravenous/runtime/tick_invocation_frame.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace iv {

enum class GraphExecutorStageResult : std::uint8_t {
    staged,
    ignored_stale,
};

struct GraphExecutorReclaimedSnapshots {
    std::size_t persisted_pages = 0;
    std::size_t tick_materializations = 0;
};

// Converts graph-derived maximum callback consumption C into producer reserve
// watermarks. Defaults retain 64 worst-case callbacks below L and refill toward
// 128 callbacks. Generic slab allocation granularity is manager-internal.
struct RealtimeProducerCapacityConfig {
    std::size_t low_watermark_callbacks = 64;
    std::size_t high_watermark_callbacks = 128;
};

// Sticky failures of executor-owned non-audio capacity maintenance. Reservation
// failures are reported separately because they mean a particular realtime
// capture was already lost.
struct RealtimeCapacityMaintenanceFailures {
    bool allocation_failed = false;
    bool unexpected_failure = false;

    [[nodiscard]] bool any() const noexcept
    {
        return allocation_failed || unexpected_failure;
    }
};

// Mutable runtime owner for immutable CompiledGraph generations. Staging and
// activation are control-path operations: callers must activate only at a legal
// whole-root boundary with no concurrent tick_block() invocation. The realtime
// call itself performs no generation selection, allocation, or lifecycle work.
// The executor-owned non-audio capacity worker stops and joins after prepared
// generations unregister their producer reserves.
class GraphExecutor {
    struct RealtimeGeneration {
        NodeStorage storage{};
        TickInvocationWorkspace tick_invocation;
        std::vector<std::unique_ptr<ProducerReserve>> producer_reserves{};
        // Declared after producer_reserves so registrations are removed before
        // their reserves are destroyed.
        std::vector<std::unique_ptr<ProducerCapacityRegistration>>
            capacity_registrations{};

        RealtimeGeneration(
            CompiledGraph const& graph,
            ResourceContext const& resources,
            AsyncCapacityManager& capacity_manager,
            RealtimeProducerCapacityConfig const& capacity_policy);
    };

    struct BackgroundGeneration {
        struct InputRoute {
            PendingQueue* queue = nullptr;
            PersistedOutputId output{};
            PortKind kind = PortKind::sample;
        };

        NodeStorage storage{};
        BackgroundCoverageState coverage{};
        BackgroundPropagationWorkspace propagation;
        std::vector<std::unique_ptr<PendingQueue>> pending_inputs{};
        std::vector<InputRoute> input_routes{};
        std::vector<PinnedBlockPrefix> input_selections{};

        BackgroundGeneration(
            CompiledGraph const& graph,
            ResourceContext const& resources,
            RealtimeGeneration& realtime);
    };

    // Complete prepared hot-reload unit. Realtime and background currently
    // execute synchronously, but they never share mutable NodeStorage or
    // generation-local workspaces.
    struct ExecutionGeneration {
        std::shared_ptr<CompiledGraph const> graph{};
        RealtimeGeneration realtime;
        BackgroundGeneration background;
        bool initialized = false;

        ExecutionGeneration(
            std::shared_ptr<CompiledGraph const> graph,
            ResourceContext const& resources,
            AsyncCapacityManager& capacity_manager,
            std::atomic<bool>& production_reservation_failed,
            RealtimeProducerCapacityConfig const& capacity_policy);

        void initialize();
        void migrate_from(ExecutionGeneration& previous);
    };

    ResourceContext resources_{};
    RealtimeProducerCapacityConfig producer_capacity_{};
    // Executor-level and deliberately outside either execution generation.
    // Compatible generations will rebind their persisted ports into this one
    // canonical sample/event authority rather than migrate page ownership.
    PersistedPageStore persisted_pages_{};
    TickMaterializationStore tick_materializations_{};
    // Generic generation-local producer capacity for Tick/persisted queues.
    AsyncCapacityManager async_capacity_manager_{64};
    // Registered off the audio thread. Each tick_block() acquires one bounded
    // callback-scoped pin from this slot before entering generated code.
    PersistedPageStore::ReaderSlot tick_page_reader_{};
    // The paired materialization root uses the same non-owning pin protocol;
    // generation/page-version validation rejects incoherent root pairs.
    TickMaterializationStore::ReaderSlot tick_materialization_reader_{};
    // Executor-lifetime sticky fault: a finalized Tick/persisted record could
    // not acquire its complete generation-local block chain.
    std::atomic<bool> production_reservation_failed_{false};
    std::array<std::optional<ExecutionGeneration>, 2> generations_{};
    std::optional<std::size_t> active_{};
    std::optional<std::size_t> pending_{};
    [[nodiscard]] ExecutionGeneration& active_execution_generation();
    [[nodiscard]] ExecutionGeneration const& active_execution_generation()
        const;

public:
    explicit GraphExecutor(
        ResourceContext resources = {},
        RealtimeProducerCapacityConfig producer_capacity = {});
    ~GraphExecutor();

    GraphExecutor(GraphExecutor const&) = delete;
    GraphExecutor& operator=(GraphExecutor const&) = delete;
    GraphExecutor(GraphExecutor&&) = delete;
    GraphExecutor& operator=(GraphExecutor&&) = delete;

    // Builds a complete pending execution generation without reading mutable
    // active storage. A newer pending generation supersedes an older pending
    // generation without disturbing the active one.
    GraphExecutorStageResult stage(
        std::shared_ptr<CompiledGraph const> compiled_graph);

    // At the caller-provided quiescent boundary, snapshots/migrates the final
    // active realtime/background state into the corresponding halves of the
    // pending execution generation and publishes it. Returns false when no
    // generation is pending.
    bool activate_pending();

    [[nodiscard]] std::optional<std::uint64_t> active_generation() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> pending_generation() const noexcept;
    [[nodiscard]] std::shared_ptr<CompiledGraph const> active_graph() const noexcept;

    // Runs one complete prepared/evaluate/publish operation against the active
    // generation. No propagation-only commit surface is exposed.
    [[nodiscard]] std::expected<BackgroundEvaluationResult, std::string>
    evaluate_background(BackgroundEvaluationRequest request);

    // Explicit non-audio reclamation for immutable roots retired by successful
    // background publication. A live callback pin always defers its owner.
    [[nodiscard]] GraphExecutorReclaimedSnapshots reclaim_retired_snapshots();

    // Sticky failures to reserve complete realtime-produced records. In
    // particular, insufficient_reserve_capacity means at least one mandatory
    // Tick/persisted or recording write was lost and later replenishment cannot
    // reconstruct it.
    [[nodiscard]] RealtimeProductionFailures
    realtime_production_failures() const noexcept;

    // Sticky failures raised when the executor-owned non-audio worker cannot
    // replenish producer-block storage. These do not imply that produced data
    // was lost unless realtime_production_failures() also reports exhaustion.
    [[nodiscard]] RealtimeCapacityMaintenanceFailures
    realtime_capacity_maintenance_failures() const noexcept;

    // Executes only the realtime half of the already-active generation.
    // Generation activation is deliberately never hidden in this audio-thread
    // entry point.
    void tick_block(std::size_t sample_index, std::size_t block_size);
};

} // namespace iv
