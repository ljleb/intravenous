#pragma once

#include <intravenous/node/resources.h>
#include <intravenous/runtime/background_evaluation_transaction.h>
#include <intravenous/runtime/background_coverage_propagation.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/persisted_page_store.h>
#include <intravenous/runtime/persisted_tick_capture_registry.h>
#include <intravenous/runtime/tick_capture_store.h>
#include <intravenous/runtime/tick_invocation_frame.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace iv {

enum class GraphExecutorStageResult : std::uint8_t {
    staged,
    ignored_stale,
};

struct GraphExecutorReclaimedSnapshots {
    std::size_t persisted_pages = 0;
    std::size_t tick_materializations = 0;
};

// Converts graph-derived maximum callback consumption C into allocator
// watermarks. Defaults retain 64 worst-case callbacks below L and refill toward
// 128 callbacks, allocating slabs in multiples of 64 capture blocks.
struct TickCaptureAllocatorConfig {
    std::size_t low_watermark_callbacks = 64;
    std::size_t high_watermark_callbacks = 128;
    std::size_t slab_allocation_granularity = 64;
};

// Sticky failures of the executor-owned non-audio capture-maintenance worker.
// Reservation failures are reported separately by TickCaptureStore because they
// mean a particular audio-thread capture was already lost.
struct TickCaptureMaintenanceFailures {
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
// Each executor owns one non-audio worker for capture-block replenishment and
// committed-block reclamation; destruction stops and joins it before stores die.
class GraphExecutor {
    class MaintenanceWorker;

    struct RealtimeGeneration {
        NodeStorage storage{};
        TickInvocationWorkspace tick_invocation;

        RealtimeGeneration(
            CompiledGraph const& graph,
            ResourceContext const& resources,
            PersistedTickCaptureRegistry& captures);
    };

    struct BackgroundGeneration {
        NodeStorage storage{};
        BackgroundCoverageState coverage{};
        BackgroundPropagationWorkspace propagation;

        BackgroundGeneration(
            CompiledGraph const& graph,
            ResourceContext const& resources);
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
            PersistedTickCaptureRegistry& captures);

        void initialize();
        void migrate_from(ExecutionGeneration& previous);
    };

    static constexpr std::size_t tick_capture_payload_capacity = 64 * 1024;

    ResourceContext resources_{};
    TickCaptureAllocatorConfig tick_capture_allocator_{};
    // Executor-level and deliberately outside either execution generation.
    // Compatible generations will rebind their persisted ports into this one
    // canonical sample/event authority rather than migrate page ownership.
    PersistedPageStore persisted_pages_{};
    TickMaterializationStore tick_materializations_{};
    // One generation-independent log. Large finalized windows are split across
    // fixed-size blocks, so staging a graph never replaces this owner.
    TickCaptureStore tick_captures_{tick_capture_payload_capacity};
    // Retention-specific identity lives above the generic transport and remains
    // resolvable after the generation which produced a pending record retires.
    PersistedTickCaptureRegistry persisted_tick_captures_{tick_captures_};
    // Registered off the audio thread. Each tick_block() acquires one bounded
    // callback-scoped pin from this slot before entering generated code.
    PersistedPageStore::ReaderSlot tick_page_reader_{};
    // The paired materialization root uses the same non-owning pin protocol;
    // generation/page-version validation rejects incoherent root pairs.
    TickMaterializationStore::ReaderSlot tick_materialization_reader_{};
    std::array<std::optional<ExecutionGeneration>, 2> generations_{};
    std::optional<std::size_t> active_{};
    std::optional<std::size_t> pending_{};
    // Declared last so its thread stops and joins before any capture-store or
    // execution-generation state it accesses is destroyed.
    std::unique_ptr<MaintenanceWorker> maintenance_{};

    [[nodiscard]] ExecutionGeneration& active_execution_generation();
    [[nodiscard]] ExecutionGeneration const& active_execution_generation()
        const;
    [[nodiscard]] std::size_t
    maximum_capture_blocks_per_callback() const noexcept;
    [[nodiscard]] TickCaptureReservePolicy tick_capture_reserve_policy(
        std::size_t maximum_blocks_per_callback) const;
    void publish_tick_capture_maintenance_policy();

public:
    explicit GraphExecutor(
        ResourceContext resources = {},
        TickCaptureAllocatorConfig tick_capture_allocator = {});
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

    // Sticky failures to reserve complete Tick capture records. In particular,
    // insufficient_free_blocks means at least one required Tick/persisted
    // capture may have been lost and later reserve maintenance cannot repair it.
    [[nodiscard]] TickCaptureReservationFailures
    tick_capture_reservation_failures() const noexcept;

    // Sticky failures raised when the executor-owned non-audio worker cannot
    // replenish capture-block storage. These do not imply that a capture was
    // lost unless tick_capture_reservation_failures() also reports exhaustion.
    [[nodiscard]] TickCaptureMaintenanceFailures
    tick_capture_maintenance_failures() const noexcept;

    // Executes only the realtime half of the already-active generation.
    // Generation activation is deliberately never hidden in this audio-thread
    // entry point.
    void tick_block(std::size_t sample_index, std::size_t block_size);
};

} // namespace iv
