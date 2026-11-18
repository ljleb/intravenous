#pragma once

#include <intravenous/graph_jit/realtime_port_state_transition.h>
#include <intravenous/node/resources.h>
#include <intravenous/runtime/async_capacity_manager.h>
#include <intravenous/runtime/background_evaluation_transaction.h>
#include <intravenous/runtime/background_coverage_propagation.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/persisted_data_store.h>
#include <intravenous/runtime/realtime_produced_record.h>
#include <intravenous/runtime/realtime_persisted_state.h>
#include <intravenous/runtime/project_playback.h>
#include <intravenous/runtime/tick_invocation_frame.h>

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace iv {

enum class GraphExecutorStageResult : std::uint8_t {
    accepted,
    ignored_stale,
};

// Cold preparation owns all generation resources until the caller either
// publishes it once or discards it. Publication only appends the prepared link;
// it does not construct storage or migration data. A real executor's handle
// must be published/discarded on its preparing control thread, before that
// executor is destroyed.
class GraphExecutorPreparedGeneration {
    std::function<GraphExecutorStageResult()> publish_{};
public:
    GraphExecutorPreparedGeneration() = default;
    explicit GraphExecutorPreparedGeneration(std::function<GraphExecutorStageResult()> publish)
        : publish_(std::move(publish)) {}
    GraphExecutorPreparedGeneration(GraphExecutorPreparedGeneration&&) = default;
    GraphExecutorPreparedGeneration& operator=(GraphExecutorPreparedGeneration&&) = default;
    GraphExecutorPreparedGeneration(GraphExecutorPreparedGeneration const&) = delete;
    GraphExecutorPreparedGeneration& operator=(GraphExecutorPreparedGeneration const&) = delete;
    explicit operator bool() const noexcept { return static_cast<bool>(publish_); }
    GraphExecutorStageResult publish() && {
        auto publish = std::exchange(publish_, {});
        return publish ? publish() : GraphExecutorStageResult::ignored_stale;
    }
};

struct GraphExecutorExecutionReadRequest {
    std::optional<std::uint64_t> active_generation{};
};

struct ProjectGraphGraphExecutorResult {
    bool attempted = false;
    GraphExecutorPreparedGeneration prepared{};
};

struct ProjectGraphManualControlTarget {
    std::uint64_t project_generation = 0;
    ManualControlId id{};
    Sample value{};
    ManualControlGesturePhase gesture = ManualControlGesturePhase::none;
    // Returned for ManualControlGesturePhase::begin.
    std::shared_ptr<ManualControlLiveTarget> live_target{};
};

// One ProjectGraph -> GraphExecutor manual-control update per cause: the
// target changes are applied first, in order, then the optional same-layout
// realtime code is staged. A client disconnect can close several gestures in
// one cause. The results return on the same record.
struct ProjectGraphGraphExecutorControlsRequest {
    std::vector<ProjectGraphManualControlTarget> targets{};
    std::shared_ptr<CompiledGraph const> realtime_code{};
    bool target_applied = false;
    GraphExecutorStageResult realtime_code_result =
        GraphExecutorStageResult::ignored_stale;
};

struct GraphExecutorReclaimedSnapshots {
    std::size_t realtime_persisted_states = 0;
    std::size_t persisted_data = 0;
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
            PersistedDataStore& data);
        void provision_producers(
            AsyncCapacityManager& capacity_manager,
            RealtimeProducerCapacityConfig const& capacity_policy);
    };

    struct BackgroundGeneration {
        // Background-only state is a separate sidecar. Pending generations may
        // allocate it on the control path, but only the background worker
        // constructs, initializes, releases, and destroys Tock Node::State.
        TockStateStorage tock_states{};
        BackgroundCoverageState coverage{};
        BackgroundPropagationWorkspace propagation;
        std::shared_ptr<PersistedDataStore::Snapshot const> prepared_data{};
        std::shared_ptr<TickMaterializationSnapshot const> prepared_materialization{};
        std::vector<std::unique_ptr<PendingQueue>> pending_inputs{};
        std::vector<BackgroundProducedInputRoute> input_routes{};
        std::vector<PinnedBlockPrefix> input_selections{};
        std::vector<PersistedOutputHandle> retired_outputs{};
        // Prepared from the exact active predecessor. These roots are retried
        // unchanged until the one required successor reconciliation commits.
        CoveragePropagationRequest reconciliation_roots{};
        // Exact future Tick positions currently requested by the control path.
        // Each transaction demands exactly these positions from every
        // BackgroundEvaluationPlan::tick_sequential_inputs port: a consumer's
        // Sequential history is the node-owned record of earlier deliveries,
        // not a re-read of published data.
        IndexCoverage tick_materialization_demand{};
        bool explicit_playback_demand = false;
        // Playback positions represented by the currently published
        // generation-compatible materialization snapshot. This can remain a
        // superset after the requested window shrinks; the next transaction
        // publishes the exact current demand again.
        IndexCoverage materialized_tick_coverage{};
        // A failed or stale transaction keeps this exact cross-queue set for
        // retry. Later producer publications remain outside every fixed prefix.
        bool input_selection_active = false;
        // Set after a background cutover until one successor transaction has
        // rebound migrated coverage/materialization state to this generation
        // and published a coherent realtime-visible pair.
        bool reconciliation_required = false;
        bool tick_materialization_required = false;

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
        // The structural graph owns layout, background state, and transition
        // metadata. A compatible steady-code replacement publishes operations
        // and their dynamic target bindings for use after the transition phase.
        std::vector<std::pair<ManualControlId, Sample>> manual_control_targets{};
        struct LiveTargetBinding {
            std::shared_ptr<ManualControlLiveTarget> target{};
            std::size_t target_storage_offset = 0;
            // Owned by the realtime thread after publication.
            std::uint64_t observed_completion = 0;
        };
        // Code and its dynamic target list are prepared and published together.
        // The audio thread never scans the structural graph's idle controls.
        struct RealtimeCode {
            std::uint64_t publication_id = 0;
            std::shared_ptr<CompiledGraph const> graph{};
            std::vector<LiveTargetBinding> live_targets{};
        };
        std::atomic<RealtimeCode*> realtime_code{nullptr};
        std::unique_ptr<RealtimeCode> active_realtime_code_owner{};
        std::unique_ptr<RealtimeCode> pending_realtime_code_owner{};
        // Activation only moves owners. The next cold staging operation (or
        // generation reclamation) destroys the previous code and binding list.
        std::unique_ptr<RealtimeCode> retired_realtime_code_owner{};
        RealtimeGeneration realtime;
        BackgroundGeneration background;
        std::unique_ptr<RealtimePersistedState> initial_persisted_state{};
        RealtimePersistedStateMailbox prepared_initial_states{};
        std::atomic<bool> initial_selection_adopted{false};
        ExecutionGeneration* preparation_predecessor = nullptr;
        std::optional<NodeStorage::Migration> realtime_migration{};
        std::optional<BackgroundCoverageState::Migration>
            background_coverage_migration{};
        // Fully checked ownership/range reconciliation and physical transfer
        // program for the realtime cutover. Preparation performs every
        // recoverable validation before publication to the audio thread.
        std::optional<graph_jit::PreparedRealtimePortStateTransition>
            realtime_port_state_transition{};
        struct TransitionCursorProgress {
            graph_jit::SampleRealtimePortStateExpiryCursor expiry{};
            std::uint64_t activation_index = 0;
        };
        // Allocated during cold generation construction. Activation only
        // snapshots the already-migrated cursor values.
        std::vector<TransitionCursorProgress> transition_cursor_progress{};
        std::uint64_t transition_event_progress = 0;
        std::atomic<CompiledGraphRealtimePhase> realtime_phase{
            CompiledGraphRealtimePhase::steady};
        bool realtime_initialized = false;
        bool background_initialized = false;
        std::unique_ptr<ExecutionGeneration> successor_owner{};
        std::atomic<ExecutionGeneration*> published_successor{nullptr};
        // Intrusive cold-to-background request, published once after acceptance.
        ExecutionGeneration* initial_playback_next = nullptr;

        ExecutionGeneration(
            std::shared_ptr<CompiledGraph const> graph,
            ResourceContext const& resources,
            PersistedDataStore& data,
            std::atomic<bool>& production_reservation_failed,
            AsyncWorkSignal& background_work_signal);

        void initialize_first_generation();
        void initialize_first_background_generation();
        void prepare_realtime_migration_from(ExecutionGeneration& previous);
        void prepare_background_migration_from(
            ExecutionGeneration& previous,
            PersistedDataStore& data);
        void commit_realtime_migration(GlobalIndex sample_index) noexcept;
        void commit_background_migration(ExecutionGeneration const* drained_predecessor = nullptr);
        void update_realtime_phase(
            std::size_t event_samples_advanced = 0) noexcept;
        void attach_successor(
            std::unique_ptr<ExecutionGeneration> successor);
        void publish_attached_successor();
        [[nodiscard]] ExecutionGeneration* successor() const noexcept;
        void prepare_initial_persisted_state(
            PersistedDataStore& data);
    };

    class BackgroundExecutor {
        using Result =
            std::expected<BackgroundEvaluationResult, std::string>;

        enum class CommandKind : std::uint8_t {
            reclaim_snapshots,
            prepare_background_migration,
            set_tick_materialization_demand,
        };

        struct Command {
            CommandKind kind = CommandKind::reclaim_snapshots;
            ExecutionGeneration* migration_target = nullptr;
            ExecutionGeneration* migration_source = nullptr;
            ExecutionGeneration* demand_target = nullptr;
            IndexCoverage materialization_demand{};
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
        std::deque<ExecutionGeneration*> preparation_queue_{};
        std::atomic<ExecutionGeneration*> initial_playback_requests_{nullptr};
        ExecutionGeneration* prepared_generation_ = nullptr;
        std::atomic<std::uint64_t> current_generation_{0};
        std::atomic<bool> has_generation_{false};
        std::mutex commands_mutex_{};
        Command* first_command_ = nullptr;
        Command* last_command_ = nullptr;
        bool stopping_ = false;
        std::atomic<bool> progress_failed_{false};
        std::deque<std::shared_ptr<ObservationWindowQuery>> observation_windows_{};
        std::atomic<std::uint64_t> observation_data_version_{0};
        std::jthread worker_{};
        void process_window(ObservationWindowQuery& query) noexcept;

        [[nodiscard]] ExecutionGeneration& generation();
        [[nodiscard]] bool advance_generation();
        [[nodiscard]] Result prepare_playback(ExecutionGeneration& target);
        [[nodiscard]] Result process_available_work();
        [[nodiscard]] Result process_available_work_safely() noexcept;
        void enqueue(Command& command);
        void run(std::stop_token stop) noexcept;

    public:
        explicit BackgroundExecutor(GraphExecutor& owner);
        ~BackgroundExecutor();
        void stop() noexcept;

        BackgroundExecutor(BackgroundExecutor const&) = delete;
        BackgroundExecutor& operator=(BackgroundExecutor const&) = delete;

        void install_initial_generation(
            std::unique_ptr<ExecutionGeneration> generation);
        void activate_initial_generation(
            std::uint64_t generation) noexcept;
        void prepare_background_migration(
            ExecutionGeneration& target,
            ExecutionGeneration& source);
        void queue_initial_playback(ExecutionGeneration& target);
        void set_tick_materialization_demand(
            ExecutionGeneration& target,
            IndexCoverage demand);
        void queue_windows(std::span<std::shared_ptr<ObservationWindowQuery> const> queries);
        std::uint64_t observation_data_version() const noexcept {
            return observation_data_version_.load(std::memory_order_acquire);
        }
        void notify_generation_cutover() noexcept;
        [[nodiscard]] AsyncWorkSignal& work_signal() noexcept;
        [[nodiscard]] std::optional<std::uint64_t>
        current_generation() const noexcept;
        [[nodiscard]] BackgroundExecutionFailures failures() const noexcept;
        [[nodiscard]] GraphExecutorReclaimedSnapshots reclaim_snapshots();
    };

    LiveObservationHandoff observations_{};
    bool measure_observation_capture_ = false;
    std::uint64_t next_observation_publication_ = 1; // Control owner only.
    std::uint64_t last_observation_publication_ = 0; // Realtime owner only.
    std::uint64_t observation_activation_ = 0; // Actual activation order.
    ResourceContext resources_{};
    RealtimeProducerCapacityConfig producer_capacity_{};
    // Executor-level and deliberately outside either execution generation.
    // Compatible generations will rebind their persisted ports into this one
    // canonical sample/event authority rather than migrate record ownership.
    PersistedDataStore persisted_data_{};
    // Background publication authority for Tick-visible derived/ephemeral
    // storage. Realtime never pins this store independently.
    TickMaterializationStore tick_materializations_{};
    // Owns the one coherent persisted-data/materialization pair used by realtime plus
    // at most the newest pending pair produced by background.
    RealtimePersistedStateMailbox realtime_persisted_state_{};
    // Generic generation-local producer capacity for Tick/persisted queues.
    AsyncCapacityManager async_capacity_manager_{64};
    // Executor-lifetime sticky fault: a finalized Tick/persisted record could
    // not acquire its complete generation-local block chain.
    std::atomic<bool> production_reservation_failed_{false};
    // One increment per skipped realtime block; no logging or waiting in Tick.
    std::atomic<std::uint64_t> playback_underrun_blocks_{0};
    // Realtime may run ahead while background drains closed predecessor queues.
    ExecutionGeneration* realtime_active_ = nullptr;
    // Owns a successor accepted after the initial chain was installed and
    // before it is attached to the active/background-owned chain. Successors
    // already attached through ExecutionGeneration::successor_owner remain
    // unpublished until their realtime cutover.
    std::unique_ptr<ExecutionGeneration> pending_owner_{};
    ExecutionGeneration* pending_head_ = nullptr;
    ExecutionGeneration* pending_tail_ = nullptr;
    // Control-path authority for ProjectGraph's next predecessor. Activation
    // never changes this owner, so reading it does not inspect audio-thread
    // state. ProjectGraph serializes accepted_graph() with stage().
    std::shared_ptr<CompiledGraph const> accepted_tail_graph_{};
    // Cold submission may overlap an audio callback. The control path waits;
    // realtime only tries once and defers activation when submission owns the
    // guard. No audio callback blocks on cold generation construction.
    std::atomic_flag accepted_chain_guard_ = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> published_active_generation_{0};
    std::atomic<bool> published_has_active_generation_{false};
    // Held by cold preparation through publication/discard. Realtime and live
    // target updates never take it. It pins the accepted tail while preparing
    // migration data without holding the audio activation guard.
    std::mutex generation_submission_mutex_{};
    // Control-path playback demand copied into every staged generation before
    // it becomes visible to either executor actor.
    IndexCoverage tick_materialization_demand_{};
    // Audio is the sole writer. The actor derives its bounded automatic
    // lookahead from this coordinate; explicit control-path demand overrides it.
    std::atomic<std::uint64_t> requested_playback_index_{0};
    struct PlaybackDemand {
        GlobalIndex project{};
        GlobalIndexRange execution{};
        bool paused = false;
        bool operator==(PlaybackDemand const&) const = default;
    };
    struct PublishedPlaybackDemand {
        std::atomic<std::uint64_t> project{}, execution{}, extent{};
        std::atomic<bool> paused{};
    };
    std::array<PlaybackDemand, 2> last_playback_demand_{};
    std::array<PublishedPlaybackDemand, 2> published_playback_demand_{};
    std::atomic<std::uint64_t> playback_demand_revision_{0};
    ProjectPlayback playback_{};
    // Generations and gestures own their live targets. The registry shares
    // those cells without retaining controls after their last owner is gone.
    // Locked only for lookup, never by realtime.
    std::mutex live_targets_mutex_{};
    std::vector<std::pair<ManualControlId, std::weak_ptr<ManualControlLiveTarget>>>
        live_targets_{};
    // Declared last so its worker stops and its generation chain is destroyed
    // before the stores and capacity manager used by those generations.
    BackgroundExecutor background_executor_;

    [[nodiscard]] ExecutionGeneration& realtime_execution_generation();
    void publish_playback_demand(std::array<PlaybackDemand, 2> demand) noexcept;
    [[nodiscard]] std::optional<std::array<PlaybackDemand, 2>> read_playback_demand() const noexcept;
    [[nodiscard]] static bool playback_ready(CompiledGraph const& graph,
        RealtimePersistedState const& persisted, PlaybackDemand const& demand) noexcept;
    bool activate_pending_at(PlaybackDemand const& applied, PlaybackDemand const* candidate = nullptr,
        RealtimePersistedState const** activated_root = nullptr, bool* applied_candidate = nullptr);
    void execute_tick_block(ExecutionGeneration& generation, RealtimePersistedState const& persisted,
        GlobalIndex sample_index, std::size_t block_size, TickPlaybackSelection selection, bool ready);
    [[nodiscard]] ExecutionGeneration const& realtime_execution_generation()
        const;
    [[nodiscard]] std::expected<BackgroundEvaluationResult, std::string>
    evaluate_generation(
        ExecutionGeneration& generation,
        bool publish_realtime_state,
        bool prepare_only = false);
    [[nodiscard]] GraphExecutorReclaimedSnapshots
    reclaim_retired_snapshots_on_background();
    void lock_accepted_chain() noexcept;
    void unlock_accepted_chain() noexcept;
    [[nodiscard]] ExecutionGeneration* find_accepted_generation(
        std::uint64_t project_generation) noexcept;
    [[nodiscard]] std::shared_ptr<ManualControlLiveTarget> live_target(
        ManualControlId const& id);
    [[nodiscard]] std::unique_ptr<ExecutionGeneration::RealtimeCode>
    prepare_realtime_code(std::shared_ptr<CompiledGraph const> graph);
    // Writes value into every accepted generation that binds id. The caller
    // holds the accepted-chain guard.
    bool set_manual_control_target_in_accepted_chain(
        ManualControlId const& id, Sample value);

public:
    [[nodiscard]] ProjectPlaybackSnapshot playback_snapshot() const noexcept { return playback_.snapshot(); }
    [[nodiscard]] std::uint64_t set_playback_running(bool running) { return playback_.set_running(running); }
    [[nodiscard]] std::uint64_t seek_playback(GlobalIndex position) { return playback_.seek(position); }
    void handle_playback(ProjectPlaybackRequest& request) {
        if (request.operation == ProjectPlaybackOperation::set_running)
            request.accepted_command = set_playback_running(request.running);
        else if (request.operation == ProjectPlaybackOperation::toggle)
            request.accepted_command = playback_.toggle();
        else if (request.operation == ProjectPlaybackOperation::seek)
            request.accepted_command = seek_playback(request.position);
        request.snapshot = playback_snapshot();
    }
    // Device-clock entry. Applies session commands and activation at this
    // boundary, then continues Tick execution even when the playhead is held.
    void render_block(std::size_t frames, double sample_rate);
    explicit GraphExecutor(
        ResourceContext resources = {},
        RealtimeProducerCapacityConfig producer_capacity = {},
        BufferedStorageLimits buffered_storage = {});
    ~GraphExecutor();

    GraphExecutor(GraphExecutor const&) = delete;
    GraphExecutor& operator=(GraphExecutor const&) = delete;
    GraphExecutor(GraphExecutor&&) = delete;
    GraphExecutor& operator=(GraphExecutor&&) = delete;

    // Builds one complete execution generation and appends it after the
    // accepted chain tail. A compiled cutover must name that exact tail. Once
    // accepted, the generation cannot be replaced by a later submission.
    GraphExecutorPreparedGeneration prepare(
        std::shared_ptr<CompiledGraph const> compiled_graph);
    GraphExecutorStageResult stage(
        std::shared_ptr<CompiledGraph const> compiled_graph);

    // Keeps the accepted structural generation and its NodeStorage. The
    // replacement must have the same project generation and canonical layout;
    // a newer compatible request replaces an older unactivated request.
    GraphExecutorStageResult stage_realtime_code(
        std::shared_ptr<CompiledGraph const> compiled_graph);

    // Cold preflight used by ProjectGraph to select code-only activation or
    // an ordinary successor with NodeStorage and port-state migration.
    [[nodiscard]] static bool can_replace_realtime_code(
        CompiledGraph const& structural,
        CompiledGraph const& replacement) noexcept;

    // Atomically changes target_bits in the accepted generation selected by
    // project_generation. One semantic ManualControlId can update several
    // concrete selected inputs.
    [[nodiscard]] bool set_manual_control_target(
        std::uint64_t project_generation,
        ManualControlId const& id,
        Sample value);

    ProjectGraphGraphExecutorResult handle_project_graph_transaction(
        std::shared_ptr<CompiledGraph const> const& compiled_graph);

    void handle_execution_read(GraphExecutorExecutionReadRequest& request) const {
        if (published_has_active_generation_.load(std::memory_order_acquire))
            request.active_generation = published_active_generation_.load(std::memory_order_relaxed);
    }

    void handle_project_graph_controls(
        ProjectGraphGraphExecutorControlsRequest& request);

    // At the caller-provided quiescent boundary, commits the prepared realtime
    // migration at the first successor sample, closes the predecessor's
    // producer queues, installs the successor's prepared initial persisted-state
    // root, publishes the next accepted generation in the ordered cutover chain
    // and switches realtime. Background migration commits only after those
    // closed queues drain. Returns false when no generation is pending or when
    // the next accepted cutover names a predecessor phase that is not active
    // yet. It never discards an accepted generation.
    bool activate_pending(GlobalIndex sample_index);

    // Replaces the anticipated future Tick playback positions for every
    // background-supplied Sequential input of the active generation. The
    // request is serialized with coverage reconciliation and producer-queue
    // work on the background actor. No transaction is scheduled when the
    // currently published materialization already covers the request.
    void set_tick_materialization_demand(IndexCoverage demand);

    [[nodiscard]] std::optional<std::uint64_t> active_generation() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> pending_generation() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    background_generation() const noexcept;
    [[nodiscard]] std::shared_ptr<CompiledGraph const> active_graph() const noexcept;
    [[nodiscard]] std::shared_ptr<CompiledGraph const> accepted_graph() const noexcept;

    // Submits explicit reclamation of immutable roots to the background worker
    // and waits for the reclaimed-owner counts. A live callback pin always
    // defers its owner.
    [[nodiscard]] GraphExecutorReclaimedSnapshots reclaim_retired_snapshots();
    [[nodiscard]] BufferedStorageUsage buffered_storage_usage() const { return persisted_data_.buffered_storage_usage(); }

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

    [[nodiscard]] std::uint64_t playback_underrun_blocks() const noexcept {
        return playback_underrun_blocks_.load(std::memory_order_relaxed);
    }

    // Executes only the realtime half of the already-active generation.
    // If its selected background data does not prepare this block, leave the
    // host's cleared output silent and count an underrun. No node is invoked.
    // Generation activation is deliberately never hidden in this audio-thread
    // entry point.
    void tick_block(GlobalIndex sample_index, std::size_t block_size);
    // One control-path collector; the borrowed frame lasts until next collect.
    void handle_observation_collect(ObservationCollectRequest& request);
    [[nodiscard]] LiveObservationFrame const* collect_observations() noexcept
    { return observations_.collect(); }

};

} // namespace iv
