#pragma once

#include <intravenous/node/resources.h>
#include <intravenous/runtime/async_capacity_manager.h>
#include <intravenous/runtime/background_evaluation_transaction.h>
#include <intravenous/runtime/background_coverage_propagation.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/persisted_page_store.h>
#include <intravenous/runtime/realtime_produced_record.h>
#include <intravenous/runtime/realtime_persisted_state.h>
#include <intravenous/runtime/tick_invocation_frame.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace iv {

enum class GraphExecutorStageResult : std::uint8_t {
    staged,
    ignored_stale,
};

struct GraphExecutorReclaimedSnapshots {
    std::size_t realtime_persisted_states = 0;
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

// Sticky failure of actor-driven background progress. A pre-commit failure
// leaves the exact selected producer-queue prefixes pending. A subsequent work
// notification lets the actor retry that same finite selection.
struct BackgroundExecutionFailures {
    bool progress_failed = false;

    [[nodiscard]] bool any() const noexcept
    {
        return progress_failed;
    }
};

// Mutable runtime owner for immutable CompiledGraph generations. Staging and
// activation are control-path operations: callers must activate only at a legal
// whole-root boundary with no concurrent tick_block() invocation. The realtime
// call itself performs no generation selection, allocation, or lifecycle work.
// The executor-owned background worker stops and destroys its generation chain
// before the non-audio capacity worker stops, so prepared generations unregister
// their producer reserves while the capacity manager is still alive.
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
        NodeStorage storage{};
        BackgroundCoverageState coverage{};
        BackgroundPropagationWorkspace propagation;
        std::vector<std::unique_ptr<PendingQueue>> pending_inputs{};
        std::vector<BackgroundProducedInputRoute> input_routes{};
        std::vector<PinnedBlockPrefix> input_selections{};
        // A failed or stale transaction keeps this exact cross-queue set for
        // retry. Later producer publications remain outside every fixed prefix.
        bool input_selection_active = false;

        BackgroundGeneration(
            CompiledGraph const& graph,
            ResourceContext const& resources,
            RealtimeGeneration& realtime,
            AsyncWorkSignal& work_signal);

        void close_inputs() noexcept;
        void select_inputs();
        void discard_empty_selection() noexcept;
        [[nodiscard]] bool has_selected_inputs() const noexcept;
        [[nodiscard]] bool inputs_closed_and_drained() const noexcept;
        [[nodiscard]] bool release_closed_input_sentinels(
            ReleasedBlockQueue& released) noexcept;
    };

    // Complete prepared hot-reload unit. Realtime and the background worker
    // never share mutable NodeStorage or generation-local workspaces.
    struct ExecutionGeneration {
        std::shared_ptr<CompiledGraph const> graph{};
        RealtimeGeneration realtime;
        BackgroundGeneration background;
        std::unique_ptr<RealtimePersistedState> initial_persisted_state{};
        std::optional<NodeStorage::Migration> realtime_migration{};
        std::optional<NodeStorage::Migration> background_migration{};
        bool realtime_initialized = false;
        bool background_initialized = false;
        std::unique_ptr<ExecutionGeneration> successor_owner{};
        std::atomic<ExecutionGeneration*> published_successor{nullptr};

        ExecutionGeneration(
            std::shared_ptr<CompiledGraph const> graph,
            ResourceContext const& resources,
            AsyncCapacityManager& capacity_manager,
            std::atomic<bool>& production_reservation_failed,
            RealtimeProducerCapacityConfig const& capacity_policy,
            AsyncWorkSignal& background_work_signal);

        void initialize_first_generation();
        void prepare_realtime_migration_from(ExecutionGeneration& previous);
        void prepare_background_migration_from(ExecutionGeneration& previous);
        void commit_realtime_migration();
        void commit_background_migration();
        void publish_successor(
            std::unique_ptr<ExecutionGeneration> successor);
        [[nodiscard]] ExecutionGeneration* successor() const noexcept;
        void prepare_initial_persisted_state(
            PersistedPageStore& pages);
    };

    class BackgroundExecutor {
        using Result =
            std::expected<BackgroundEvaluationResult, std::string>;

        enum class CommandKind : std::uint8_t {
            reclaim_snapshots,
            prepare_background_migration,
        };

        struct Command {
            CommandKind kind = CommandKind::reclaim_snapshots;
            ExecutionGeneration* migration_target = nullptr;
            ExecutionGeneration* migration_source = nullptr;
            Command* next = nullptr;
            std::mutex mutex{};
            std::condition_variable completed{};
            std::optional<GraphExecutorReclaimedSnapshots> reclaimed{};
            std::exception_ptr exception{};
            bool done = false;
        };

        GraphExecutor& owner_;
        // Declared before the generation chain so every PendingQueue referring
        // to this signal is destroyed first.
        AsyncWorkSignal work_signal_{};
        std::unique_ptr<ExecutionGeneration> generation_chain_{};
        std::atomic<std::uint64_t> current_generation_{0};
        std::atomic<bool> has_generation_{false};
        std::mutex commands_mutex_{};
        Command* first_command_ = nullptr;
        Command* last_command_ = nullptr;
        bool stopping_ = false;
        std::atomic<bool> progress_failed_{false};
        std::jthread worker_{};

        [[nodiscard]] ExecutionGeneration& generation();
        [[nodiscard]] bool advance_generation();
        [[nodiscard]] Result process_available_work();
        [[nodiscard]] Result process_available_work_safely() noexcept;
        void enqueue(Command& command);
        void run(std::stop_token stop) noexcept;

    public:
        explicit BackgroundExecutor(GraphExecutor& owner);
        ~BackgroundExecutor();

        BackgroundExecutor(BackgroundExecutor const&) = delete;
        BackgroundExecutor& operator=(BackgroundExecutor const&) = delete;

        void install_first_generation(
            std::unique_ptr<ExecutionGeneration> generation);
        void prepare_background_migration(
            ExecutionGeneration& target,
            ExecutionGeneration& source);
        void notify_generation_cutover() noexcept;
        [[nodiscard]] AsyncWorkSignal& work_signal() noexcept;
        [[nodiscard]] std::optional<std::uint64_t>
        current_generation() const noexcept;
        [[nodiscard]] BackgroundExecutionFailures failures() const noexcept;
        [[nodiscard]] GraphExecutorReclaimedSnapshots reclaim_snapshots();
    };

    ResourceContext resources_{};
    RealtimeProducerCapacityConfig producer_capacity_{};
    // Executor-level and deliberately outside either execution generation.
    // Compatible generations will rebind their persisted ports into this one
    // canonical sample/event authority rather than migrate page ownership.
    PersistedPageStore persisted_pages_{};
    // Background publication authority for Tick-visible derived/ephemeral
    // storage. Realtime never pins this store independently.
    TickMaterializationStore tick_materializations_{};
    // Owns the one coherent page/materialization pair used by realtime plus
    // at most the newest pending pair produced by background.
    RealtimePersistedStateMailbox realtime_persisted_state_{};
    // Generic generation-local producer capacity for Tick/persisted queues.
    AsyncCapacityManager async_capacity_manager_{64};
    // Executor-lifetime sticky fault: a finalized Tick/persisted record could
    // not acquire its complete generation-local block chain.
    std::atomic<bool> production_reservation_failed_{false};
    // Realtime may run ahead while background drains closed predecessor queues.
    ExecutionGeneration* realtime_active_ = nullptr;
    // This generation has not crossed a realtime boundary and may be replaced.
    std::unique_ptr<ExecutionGeneration> pending_generation_{};
    // Declared last so its worker stops and its generation chain is destroyed
    // before the stores and capacity manager used by those generations.
    BackgroundExecutor background_executor_;

    [[nodiscard]] ExecutionGeneration& realtime_execution_generation();
    [[nodiscard]] ExecutionGeneration const& realtime_execution_generation()
        const;
    [[nodiscard]] std::expected<BackgroundEvaluationResult, std::string>
    evaluate_generation(
        ExecutionGeneration& generation,
        bool publish_realtime_state);
    [[nodiscard]] GraphExecutorReclaimedSnapshots
    reclaim_retired_snapshots_on_background();

public:
    explicit GraphExecutor(
        ResourceContext resources = {},
        RealtimeProducerCapacityConfig producer_capacity = {});
    ~GraphExecutor();

    GraphExecutor(GraphExecutor const&) = delete;
    GraphExecutor& operator=(GraphExecutor const&) = delete;
    GraphExecutor(GraphExecutor&&) = delete;
    GraphExecutor& operator=(GraphExecutor&&) = delete;

    // Builds a complete pending execution generation and prepares both storage
    // migrations without copying mutable active state. A newer pending
    // generation supersedes an older one without disturbing the active one.
    GraphExecutorStageResult stage(
        std::shared_ptr<CompiledGraph const> compiled_graph);

    // At the caller-provided quiescent boundary, commits the prepared realtime
    // migration, closes the predecessor's producer queues, installs the
    // successor's prepared initial persisted-state root, appends the pending
    // generation to the ordered cutover chain and switches realtime. Background
    // migration commits only after those closed queues drain. Returns false when
    // no generation is pending.
    bool activate_pending();

    [[nodiscard]] std::optional<std::uint64_t> active_generation() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> pending_generation() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    background_generation() const noexcept;
    [[nodiscard]] std::shared_ptr<CompiledGraph const> active_graph() const noexcept;

    // Submits explicit reclamation of immutable roots to the background worker
    // and waits for the reclaimed-owner counts. A live callback pin always
    // defers its owner.
    [[nodiscard]] GraphExecutorReclaimedSnapshots reclaim_retired_snapshots();

    // Sticky failures to reserve complete realtime-produced records. In
    // particular, insufficient_reserve_capacity means at least one mandatory
    // Tick/persisted recording write was lost and later replenishment cannot
    // reconstruct it.
    [[nodiscard]] RealtimeProductionFailures
    realtime_production_failures() const noexcept;

    // Sticky failures raised when the executor-owned non-audio worker cannot
    // replenish producer-block storage. These do not imply that produced data
    // was lost unless realtime_production_failures() also reports exhaustion.
    [[nodiscard]] RealtimeCapacityMaintenanceFailures
    realtime_capacity_maintenance_failures() const noexcept;

    // Sticky failure of queue/cutover-driven background evaluation.
    [[nodiscard]] BackgroundExecutionFailures
    background_execution_failures() const noexcept;

    // Executes only the realtime half of the already-active generation.
    // Generation activation is deliberately never hidden in this audio-thread
    // entry point.
    void tick_block(std::size_t sample_index, std::size_t block_size);
};

} // namespace iv
