#pragma once

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/graph_jit/connection_plan.h>
#include <intravenous/graph_jit/sample_storage_plan.h>
#include <intravenous/graph_jit/lowering.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace iv::graph_jit::detail {
// Host-side lowering plan. These records intentionally contain no llvm::Value*
// or other project-module IR handles: the complete plan is built before package
// modules are consumed or output LLVM is emitted.
struct PrimitiveStoragePlan {
    bool has_state = false;
    std::size_t state_offset = 0;
    std::size_t state_size = 0;
};

struct ContinuousActivityChannelStatePlan {
    std::size_t frontier = 0;
};

struct ContinuousInputActivityPlan {
    // A compiler-owned immutable representation is unchanged by definition and
    // therefore owns no remembered-value state or runtime scan.
    bool static_constant = false;
    std::vector<ContinuousActivityChannelStatePlan> channels{};
};

struct PrimitiveActivityPlan {
    std::size_t ttl_samples = 0;
    std::vector<ContinuousInputActivityPlan> continuous_inputs{};
    // Compact realtime-output order, matching the reflected callback ABI.
    std::vector<std::size_t> continuous_output_channel_counts{};
    NodeLayout::RegionHandle region{};
    std::size_t storage_offset = 0;
    std::size_t storage_size = 0;
    std::size_t storage_alignment = 1;
};

// Equal ordinary realtime fanout channels share one persistent comparison
// value. Generated lowering also shares the compact fact while the source
// representation and invocation window remain unchanged.
struct ContinuousActivityFrontierPlan {
    std::size_t representation = no_sample_representation;
    std::size_t representation_channel = 0;
    std::size_t frame_delay = 0;
    std::int32_t absolute_pace_exponent = 0;
    std::size_t read_latency = 0;
    ContinuousValueStorageGeometry geometry{};
    std::size_t owner_primitive = 0;
    std::size_t remembered_valid_relative_offset = 0;
    std::size_t remembered_value_relative_offset = 0;
    std::size_t remembered_valid_storage_offset = 0;
    std::size_t remembered_value_storage_offset = 0;
};

struct DeclarationPlan {
    NodeLayout node_layout{};
    TockStateLayout tock_state_layout{};
    // Top-level graph primitive index -> canonical layout node index. Nested
    // declarations occupy their own layout records and deliberately do not
    // enter graph execution/configuration namespaces.
    std::vector<std::size_t> primitive_layout_nodes{};
    // Realtime primitive storage remains indexed independently from the
    // sidecar so later lowering can add synthetic/layout-only nodes without
    // coupling execution-step identity to canonical layout record order.
    std::vector<PrimitiveStoragePlan> primitive_storage{};
    // Entry positions match the primitive namespace. A missing entry means
    // TTL is disabled and generated execution pays no activity cost.
    std::vector<std::optional<PrimitiveActivityPlan>> primitive_activity{};
    std::vector<ContinuousActivityFrontierPlan> activity_frontiers{};
};

enum class CallbackImportAbi : std::uint8_t {
    block,
    background,
    batch,
};

struct CallbackImportPlan {
    std::string source_symbol{};
    std::string import_symbol{};
    std::string role{};
    CallbackImportAbi abi = CallbackImportAbi::block;
};

struct RetainedGlobalImportPlan {
    std::string source_symbol{};
    std::string import_symbol{};
    std::size_t size = 0;
};

struct PackageImportGroup {
    std::size_t package_index = 0;
    std::vector<CallbackImportPlan> callbacks{};
    std::vector<RetainedGlobalImportPlan> retained_globals{};
};

struct PrimitiveCallbackPlan {
    NodeBundleHandle bundle = 0;
    std::string tick_block{};
    std::string tick_block_batch{};
    std::string skip_block{};
    std::string skip_block_batch{};
    std::string tock_coverage{};
    std::string tock_coverage_batch{};
    std::string propagate_forward_coverage{};
    std::string propagate_forward_coverage_batch{};
    std::string propagate_reverse_coverage{};
    std::string propagate_reverse_coverage_batch{};
};

struct PackageImportPlan {
    // One group per compile-local package module. All selected callback and
    // retained-global roots must be known before that module is consumed once.
    std::vector<PackageImportGroup> packages{};
    // One entry per analyzed concrete primitive. Several primitives may share one
    // imported callback when they select the same package-local implementation.
    std::vector<PrimitiveCallbackPlan> primitive_callbacks{};
};

struct NodeConfigurationRelocationPlan {
    std::size_t byte_offset = 0;
    std::size_t addend = 0;
    // Empty means an explicit null pointer index. Non-empty names the imported
    // retained LLVM global whose byte-address plus addend reconstructs the
    // configured pointer value.
    std::string retained_global_symbol{};
};

struct NodeConfigurationPlan {
    // Own the configured bytes so LLVM emission does not depend on native
    // configured-object addresses after host-side planning completes. Pointer
    // indices are zeroed here and reconstructed symbolically during LLVM
    // emission; native process addresses must never enter project IR.
    std::vector<std::byte> bytes{};
    std::vector<NodeConfigurationRelocationPlan> relocations{};
    std::size_t alignment = 1;
    ReflectedNodeTickContext tick_context_template{};
    std::string node_global_symbol{};
    std::string tick_context_global_symbol{};
};

struct ConfigurationPlan {
    std::vector<NodeConfigurationPlan> nodes{};
};


struct PrimitiveSampleInputChannelBindingPlan {
    std::size_t storage = no_sample_representation;
    std::size_t representation_channel = 0;
    std::size_t frame_delay = 0;
};

struct PrimitiveSampleInputBindingPlan {
    // Canonical target-channel order. Each semantic channel may resolve to a
    // different storage/capacity/timing. Materialized inputs
    // simply bind every channel to the corresponding channel of one target
    // storage.
    std::vector<PrimitiveSampleInputChannelBindingPlan> channels{};
    ChannelLayout channel_layout{};
    ContinuousValueStorageGeometry geometry{};
    std::int32_t absolute_pace_exponent = 0;
    std::size_t history = 0;
    std::size_t read_latency = 0;
    // One planar channel scratch range begins here in the shared continuous
    // input scratch tail. A zero capacity means every exposed block is a
    // single direct value and needs no fallback materialization.
    std::size_t contiguous_scratch_offset = 0;
    std::size_t contiguous_value_capacity = 0;
    bool has_contiguous_scratch = false;
};

struct PrimitiveSampleOutputBindingPlan {
    bool realtime = true;
    std::optional<std::size_t> storage{};
    ContinuousValueStorageGeometry geometry{};
    std::int32_t absolute_pace_exponent = 0;
    std::size_t history = 0;
    std::size_t latency = 0;
};

struct PrimitiveSamplePortPlan {
    // Entry positions correspond to declared storage sample-port indices.
    // Lowering compacts realtime outputs into the tick ABI; background outputs
    // instead receive bindings from the background executor.
    std::vector<PrimitiveSampleInputBindingPlan> inputs{};
    std::vector<PrimitiveSampleOutputBindingPlan> outputs{};
};

struct SamplePortBindingPlan {
    SampleStoragePlan storage{};
    // One entry per analyzed concrete primitive.
    std::vector<PrimitiveSamplePortPlan> primitives{};
    std::size_t contiguous_input_scratch_size = 0;
    std::size_t contiguous_input_scratch_alignment = 1;

    [[nodiscard]] bool empty() const noexcept
    {
        return storage.empty();
    }
};

// Points 10-11 realize direct/transient realtime event flow plus bounded
// compact carry and persistent retained rings. Ordinary ports store a
// count plus a bounded TimedEvent sequence. Large retained windows bind producer
// and identity consumers directly to one migration-identified persistent ring
// carrying monotonic read/write indices, so retained events are expired
// incrementally rather than copied through a compact tail each root call.
// Retained canonical ports may also feed windowed transient
// materializations for sliced/converted consumer branches. The canonical
// producer storage reserves one overflow counter per logical event
// output; derived fanout ports never duplicate producer telemetry.
// Primitive callbacks receive only immutable bindings and reconstruct
// invocation-local EventInputPort/EventOutputPort facades.
inline constexpr std::size_t no_event_transient_allocation =
    std::numeric_limits<std::size_t>::max();

struct EventRepresentationPlan {
    std::size_t producer_group_index = 0;
    EventTypeId type = EventTypeId::empty;
    std::size_t event_capacity = 0;
    bool persistent = false;
    bool persistent_ring = false;
    std::string migration_identity{};
    bool has_producer_overflow_counter = false;
    std::size_t count_relative_offset = 0;
    std::size_t read_index_relative_offset = 0;
    std::size_t write_index_relative_offset = 0;
    // Persistent Sequential event storage owns one transport mapping. The
    // initialized byte distinguishes a genuine expected position of zero from
    // storage which has not completed an invocation yet.
    bool has_sequential_timeline = false;
    std::size_t timeline_initialized_relative_offset = 0;
    std::size_t expected_next_time_relative_offset = 0;
    std::size_t events_relative_offset = 0;
    // Ordered fan-in carries one compiler-only semantic source index beside
    // each TimedEvent. Staged fan-in needs it to preserve equal-time ordering
    // across execution regions; retained fan-in also needs it when individual
    // producers own history/latency state. Authored port facades still see only
    // TimedEvent.
    bool has_source_indices = false;
    std::size_t source_indices_relative_offset = 0;
    std::size_t size_bytes = 0;
    std::size_t alignment = 1;
    std::size_t transient_allocation = no_event_transient_allocation;
    NodeLayout::RegionHandle region{};
    NodeLayout::RegionHandle overflow_region{};
    // Canonical NodeStorage offsets exist only for persistent ports
    // and producer telemetry. Transient storage addresses come from the
    // generated root's event arena plus region_relative_offset.
    std::size_t storage_offset = 0;
    std::size_t overflow_count_storage_offset = 0;
};

struct EventTransientAllocationPlan {
    std::size_t representation_index = 0;
    std::size_t size_bytes = 0;
    std::size_t alignment = 1;
    std::size_t region_relative_offset = 0;
    // Inclusive flattened schedule positions where this stack buffer may be
    // accessed. This is compile-time planning data only.
    ConnectionLiveIntervalPlan live_interval{};
};

struct PrimitiveEventInputBindingPlan {
    std::optional<std::size_t> storage{};
};

struct PrimitiveEventOutputBindingPlan {
    // Entry positions correspond to declared storage event-port indices. Only
    // realtime entries are emitted into ReflectedNodeTickContext.
    bool realtime = true;
    // Callback-facing storage. A retained or sliced output can use a separate
    // canonical_storage so the callback writes only its current invocation.
    std::optional<std::size_t> storage{};
    std::optional<std::size_t> canonical_storage{};
    EventTypeId source_type = EventTypeId::empty;
    std::size_t history = 0;
    std::size_t latency = 0;
    bool append_existing = false;
};

enum class EventOperationScopeKind : std::uint8_t {
    primitive,
    region,
};

enum class EventOperationPhase : std::uint8_t {
    before,
    after,
};

// Event operations are planned against the invocation whose index/block-size
// define their semantic window. A primitive scope executes for every SCC slice;
// a region scope executes once around the complete root-call region. Keeping
// this in the storage plan prevents execution planning from rediscovering a
// materialization's consumers and guessing which window it meant.
struct EventOperationScope {
    EventOperationScopeKind kind = EventOperationScopeKind::primitive;
    EventOperationPhase phase = EventOperationPhase::after;
    std::size_t index = 0;

    friend bool operator==(EventOperationScope const&, EventOperationScope const&)
        = default;
};

struct EventMaterializationPlan {
    std::size_t source_representation = 0;
    std::size_t target_representation = 0;
    EventConversionPlan conversion{};
    // A value selects only events authored by one semantic source from an
    // ordered fan-in representation. The selected stream no longer needs a
    // source-index sidecar.
    std::optional<std::size_t> source_index_filter{};
    // Aggregate or retained canonical sources may contain events outside the
    // consumer invocation currently being materialized. Windowed selection uses
    // the index/block_size supplied at the emission site: a cyclic step-local
    // materialization therefore selects one SCC slice, while a region-exit
    // materialization selects the complete root call. In both cases the window
    // is [index-history_samples, index+block_size) before conversion. Target
    // capacity remains source-capacity-sized because max_events_per_index is
    // only a storage-sizing rate, not a runtime density constraint.
    std::size_t history_samples = 0;
    // Exact unrounded upper bound for events written by this one shared
    // operation. Costing uses the operation once regardless of fanout count.
    std::size_t maximum_output_event_count = 0;
    // Total source events examined across every execution of this operation in
    // one root call. This differs from output count for windowed SCC
    // materialization and is charged only when the source candidate is a ring.
    std::size_t maximum_source_event_reads = 0;
    bool select_invocation_window = false;
    bool before_merge = false;
    EventOperationScope scope{};
};

struct EventCarryPlan {
    std::size_t working_representation = 0;
    std::size_t persistent_representation = 0;
    // Staged SCC fan-in can restore before its first stage and commit after its
    // last stage. Other carry operations normally use one producer scope.
    EventOperationScope restore_scope{};
    EventOperationScope commit_scope{};
    std::size_t retained_history_samples = 0;
    std::size_t retained_latency_samples = 0;
};

struct EventPersistentRingPlan {
    std::size_t storage = 0;
    EventOperationScope prune_scope{};
    std::size_t retained_history_samples = 0;
};

struct EventSequenceResetPlan {
    std::size_t storage = 0;
    EventOperationScope scope{};
};

struct EventMergePlan {
    // Sorted callback-output streams are merged in semantic source order after
    // the producers for this scope have completed. target_representation is a
    // separate fan-in sequence. It never aliases a concrete EventOutputPortId
    // raw representation.
    std::vector<std::size_t> source_representations{};
    // Empty for an unindexed stable merge. Ordered fan-in supplies one semantic
    // source index per source storage; the target owns a parallel sidecar used
    // only by compiler runtime helpers.
    std::vector<std::size_t> source_indices{};
    std::size_t target_representation = 0;
    EventOperationScope scope{};
    // Every input stream is sorted in forward circular order beginning at the
    // root invocation's history-inclusive aggregate origin. That origin stays
    // fixed while an SCC contributes several slices to the same sequence.
    std::size_t ordering_history_samples = 0;
    // Retained targets already contain restored/pruned events from previous
    // invocations. Staged SCC fan-in also retains events from earlier slices
    // of the current root invocation.
    bool preserve_existing_target = false;
    // Canonical EventOutputPortId storage must receive the callback-local
    // sequence before any source conversion or connection fan-in reads it.
    bool before_materialization = false;
};

struct EventFeedbackPlan {
    std::size_t source_representation = 0;
    std::size_t target_representation = 0;
    std::optional<std::size_t> source_index_filter{};
    // A callback-local source sequence is cleared and populated again for
    // each primitive invocation. Feedback therefore consumes the complete
    // sequence at every append scope instead of retaining a root-call cursor.
    bool source_sequence_restarts_each_append = false;
    EventOperationScope append_scope{};
    EventOperationScope reset_scope{};
    EventConnectionStorageRequirements storage_requirements{};
    EventConnectionStoragePlan storage_plan{};
    // Exact unrounded bounds for producer -> delayed-stream writes and the
    // retained tail. Compact carry restore/commit work is derived from the
    // latter by the shared storage cost model.
    std::size_t authored_event_count = 0;
    std::size_t source_event_reads = 0;
    std::size_t retained_event_count = 0;
    std::size_t retained_window_samples = 0;
    // Source and target sequences are ordered from the retained-history
    // boundary, not necessarily from the current invocation index.
    std::size_t ordering_history_samples = 0;
    std::size_t loop_extra_latency = 1;
};

struct PrimitiveEventPortPlan {
    std::vector<PrimitiveEventInputBindingPlan> inputs{};
    std::vector<PrimitiveEventOutputBindingPlan> outputs{};
};

struct EventPortBindingPlan {
    // Entry positions correspond to
    // ConnectionAnalysisPlan::event_producer_groups. These are the final
    // lowering decisions after concrete producer buffers and operations are known;
    // connection analysis carries only the temporal requirements.
    std::vector<std::optional<EventConnectionStoragePlan>>
        producer_group_storage_plans{};
    std::vector<std::optional<std::size_t>> producer_group_representations{};
    // Per-source representations consumed by each producer-group merge.
    // A detached source reads this representation before the group merge,
    // rather than selecting the source from the already-sorted group result.
    std::vector<std::vector<std::optional<std::size_t>>>
        producer_group_source_representations{};
    std::vector<EventRepresentationPlan> ports{};
    std::vector<EventMaterializationPlan> materializations{};
    std::vector<EventCarryPlan> carry_operations{};
    std::vector<EventPersistentRingPlan> persistent_rings{};
    std::vector<EventSequenceResetPlan> sequence_resets{};
    std::vector<EventMergePlan> merges{};
    std::vector<EventFeedbackPlan> feedback_operations{};
    std::vector<EventTransientAllocationPlan> transient_allocations{};
    std::size_t transient_arena_size = 0;
    std::size_t transient_arena_alignment = 1;
    // One entry per analyzed concrete primitive.
    std::vector<PrimitiveEventPortPlan> primitives{};
};

enum class EventOperationKind : std::uint8_t {
    sequence_reset,
    persistent_ring_prune,
    carry_restore,
    merge,
    materialize,
    feedback_append,
    carry_commit,
};

struct EventOperationRef {
    EventOperationKind kind = EventOperationKind::sequence_reset;
    std::size_t index = 0;
};

struct PrimitiveExecutionStep {
    std::size_t configuration_index = 0;
    std::size_t storage_index = 0;
    // Maximum block size accepted by this primitive. LLVM realization uses
    // this as the single invocation-slicing boundary for both tick and skip.
    std::size_t maximum_block_size = 0;
    std::string tick_callback_symbol{};
    std::string tick_batch_callback_symbol{};
    std::string skip_callback_symbol{};
    std::string skip_batch_callback_symbol{};

    // Explicit sample storage operations surrounding this producer. These are
    // storage-plan indices, not OutputPort behavior or runtime objects.
    std::vector<std::size_t> sample_carry_restores_before{};
    std::vector<std::size_t> sample_materializations_before{};
    std::vector<std::size_t> sample_compositions_before{};
    std::vector<std::size_t> sample_feedback_writes_after{};
    std::vector<std::size_t> sample_materializations_after{};
    std::vector<std::size_t> sample_compositions_after{};
    // Transition-root mirrors that must observe feedback/composition writes
    // completed by this step. Ordinary materializations stay earlier so
    // authored consumers retain their established ordering.
    std::vector<std::size_t> sample_transition_materializations_after{};
    std::vector<std::size_t> sample_carry_commits_after{};

    std::vector<EventOperationRef> event_operations_before{};
    std::vector<EventOperationRef> event_operations_after{};
};

struct ExecutionRegionPlan {
    std::vector<std::size_t> primitive_steps{};
    std::vector<EventOperationRef> event_operations_before{};
    std::vector<EventOperationRef> event_operations_after{};
    bool cyclic = false;
    std::size_t maximum_block_size = 0;
    std::size_t scc_feedback_latency = 0;
};

struct ExecutionReadyGroupPlan {
    // A multi-step entry is one compatible antichain discovered from one
    // scheduler ready-frontier snapshot. Planning keeps a multi-step entry
    // only when lowering can emit it as one native batch; otherwise its
    // members return to the scalar schedule. Cyclic SCC execution is
    // represented by cyclic_region instead.
    std::vector<std::size_t> primitive_steps{};
    std::optional<std::size_t> cyclic_region{};
};

struct ExecutionPlan {
    // Ordered root execution sequence. For disconnected zero-port primitives this
    // is configured-bundle order; connection-aware scheduling will later replace
    // that provisional order without changing the LLVM-emission boundary.
    std::vector<PrimitiveExecutionStep> primitive_steps{};
    std::vector<ExecutionRegionPlan> regions{};
    std::vector<ExecutionReadyGroupPlan> ready_groups{};
};


struct RootStackBufferPlan {
    std::size_t size_bytes = 0;
    std::size_t alignment = 1;
    std::size_t sample_offset = 0;
    std::size_t event_offset = 0;
};

struct LoweringPlan {
    ResolvedPortRealization port_realization{};
    // Pure graph/topology analysis remains part of the stable lowering plan;
    // sample storage realization consumes it without introducing a parallel
    // topology or policy model.
    ConnectionAnalysisPlan connections{};
    DeclarationPlan declarations{};
    PackageImportPlan imports{};
    ConfigurationPlan configurations{};
    SamplePortBindingPlan sample_ports{};
    EventPortBindingPlan event_ports{};
    RealtimePortStateRealizations realtime_port_state_realizations{};
    RootStackBufferPlan root_stack{};
    ExecutionPlan execution{};
};

std::expected<LoweringPlan, std::string> build_lowering_plan(
    LoweringInput const& input);

// Splits each multi-step ready group that can no longer be emitted as one
// native batch into singleton groups in the same order. A caller that adds
// storage operations to a built plan's steps uses this to restore the
// ExecutionReadyGroupPlan invariant without changing execution positions.
std::expected<void, std::string> split_unbatched_ready_groups(LoweringPlan& plan);

} // namespace iv::graph_jit::detail
