#pragma once

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/graph_jit/connection_plan.h>
#include <intravenous/graph_jit/sample_physical_plan.h>
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
    bool has_compiled_state = false;
    std::size_t compiled_state_offset = 0;
    std::size_t compiled_state_size = 0;
};

struct DeclarationPlan {
    NodeLayout node_layout{};
    // Indexed independently from NodeLayout::nodes so later lowering can add
    // synthetic/layout-only nodes without coupling execution-step identity to
    // canonical layout record order.
    std::vector<PrimitiveStoragePlan> primitive_storage{};
};

struct CallbackImportPlan {
    std::string source_symbol{};
    std::string import_symbol{};
    std::string role{};
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
    std::string tick_block{};
    std::string skip_block{};
};

struct PackageImportPlan {
    // One group per compile-local package module. All selected callback and
    // retained-global roots must be known before that module is consumed once.
    std::vector<PackageImportGroup> packages{};
    // Indexed by analyzed concrete primitive. Several primitives may share one
    // imported callback when they select the same package-local implementation.
    std::vector<PrimitiveCallbackPlan> primitive_callbacks{};
};

struct NodeConfigurationRelocationPlan {
    std::size_t byte_offset = 0;
    std::size_t addend = 0;
    // Empty means an explicit null pointer slot. Non-empty names the imported
    // retained LLVM global whose byte-address plus addend reconstructs the
    // configured pointer value.
    std::string retained_global_symbol{};
};

struct NodeConfigurationPlan {
    // Own the configured bytes so LLVM emission does not depend on native
    // configured-object addresses after host-side planning completes. Pointer
    // slots are zeroed here and reconstructed symbolically during LLVM
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
    std::size_t representation = no_sample_representation;
    std::size_t representation_channel = 0;
    std::size_t frame_delay = 0;
};

struct PrimitiveSampleInputBindingPlan {
    // Canonical target-channel order. Each semantic channel may resolve to a
    // different physical representation/capacity/timing. Materialized inputs
    // simply bind every channel to the corresponding channel of one target
    // representation.
    std::vector<PrimitiveSampleInputChannelBindingPlan> channels{};
    ChannelLayout channel_layout{};
    std::size_t history = 0;
    std::size_t read_latency = 0;
};

struct PrimitiveSampleOutputBindingPlan {
    std::optional<std::size_t> representation{};
    std::size_t history = 0;
    std::size_t latency = 0;
};

struct PrimitiveSamplePortPlan {
    // One immutable physical-representation binding per declared sample port
    // ordinal. Temporal API semantics remain per-port even when fanout shares
    // one physical producer representation.
    std::vector<PrimitiveSampleInputBindingPlan> inputs{};
    std::vector<PrimitiveSampleOutputBindingPlan> outputs{};
};

struct SamplePortBindingPlan {
    SamplePhysicalPlan physical{};
    // Indexed by analyzed concrete primitive.
    std::vector<PrimitiveSamplePortPlan> primitives{};

    [[nodiscard]] bool empty() const noexcept
    {
        return physical.empty();
    }
};

// Points 10-11 realize direct/transient realtime event flow plus bounded
// compact carry and persistent retained rings. Ordinary representations store a
// count plus a bounded TimedEvent sequence. Large retained windows bind producer
// and identity consumers directly to one migration-identified persistent ring
// carrying monotonic read/write indices, so retained events are expired
// incrementally rather than copied through a compact tail each root call.
// Retained canonical representations may also feed windowed transient
// materializations for sliced/converted consumer branches. The canonical
// producer representation reserves one overflow counter per logical event
// output; derived fanout representations never duplicate producer telemetry.
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
    std::size_t events_relative_offset = 0;
    std::size_t size_bytes = 0;
    std::size_t alignment = 1;
    std::size_t transient_allocation = no_event_transient_allocation;
    NodeLayout::RegionHandle region{};
    NodeLayout::RegionHandle overflow_region{};
    // Canonical NodeStorage offsets exist only for persistent representations
    // and producer telemetry. Transient representation addresses come from the
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
    std::optional<std::size_t> representation{};
};

struct PrimitiveEventOutputBindingPlan {
    std::optional<std::size_t> representation{};
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
// this in the physical plan prevents execution planning from rediscovering a
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
    EventOperationScope scope{};
};

struct EventCarryPlan {
    std::size_t working_representation = 0;
    std::size_t persistent_representation = 0;
    // Restore may need to precede an early producer-home writer while commit
    // remains after the group's final merge. Ordinary single-producer carry
    // uses the same position for both.
    EventOperationScope restore_scope{};
    EventOperationScope commit_scope{};
    std::size_t retained_history_samples = 0;
    std::size_t retained_latency_samples = 0;
};

struct EventPersistentRingPlan {
    std::size_t representation = 0;
    EventOperationScope prune_scope{};
    std::size_t retained_history_samples = 0;
};

struct EventSequenceResetPlan {
    std::size_t representation = 0;
    EventOperationScope scope{};
};

struct EventMergePlan {
    // Sorted producer streams are merged in semantic source order only after
    // every producer has completed its root invocation. For transient fan-in,
    // semantic source 0 writes directly into target_representation and the
    // remaining source_representations are merged into it in one k-way pass.
    // Retained producer-home fan-in instead treats the restored/pruned target
    // plus semantic source 0 as the existing target sequence, then merges only
    // the remaining producer-local streams.
    std::vector<std::size_t> source_representations{};
    std::size_t target_representation = 0;
    EventOperationScope scope{};
    bool target_is_semantic_source = false;
    // Retained targets already contain restored/pruned events from previous
    // invocations; transient producer-home targets contain source 0 instead.
    bool preserve_existing_target = false;
};

struct EventFeedbackPlan {
    std::size_t source_representation = 0;
    std::size_t target_representation = 0;
    EventOperationScope append_scope{};
    EventOperationScope reset_scope{};
    EventConnectionStorageRequirements storage_requirements{};
    EventConnectionStoragePlan storage_plan{};
    // Exact unrounded bounds for producer -> delayed-stream writes and the
    // retained tail. Compact carry restore/commit work is derived from the
    // latter by the shared residence cost model.
    std::size_t authored_event_count = 0;
    std::size_t retained_event_count = 0;
    std::size_t retained_window_samples = 0;
    std::size_t loop_extra_latency = 1;
};

struct PrimitiveEventPortPlan {
    std::vector<PrimitiveEventInputBindingPlan> inputs{};
    std::vector<PrimitiveEventOutputBindingPlan> outputs{};
};

struct EventPortBindingPlan {
    // Indexed by ConnectionAnalysisPlan::event_producer_groups. These are the
    // final lowering decisions after concrete producer buffers and operations
    // are known; connection analysis carries only the temporal requirements.
    std::vector<std::optional<EventConnectionStoragePlan>>
        producer_group_storage_plans{};
    std::vector<std::optional<std::size_t>> producer_home_source_indices{};
    std::vector<std::optional<std::size_t>> producer_group_representations{};
    std::vector<EventRepresentationPlan> representations{};
    std::vector<EventMaterializationPlan> materializations{};
    std::vector<EventCarryPlan> carry_operations{};
    std::vector<EventPersistentRingPlan> persistent_rings{};
    std::vector<EventSequenceResetPlan> sequence_resets{};
    std::vector<EventMergePlan> merges{};
    std::vector<EventFeedbackPlan> feedback_operations{};
    std::vector<EventTransientAllocationPlan> transient_allocations{};
    std::size_t transient_arena_size = 0;
    std::size_t transient_arena_alignment = 1;
    // Indexed by analyzed concrete primitive.
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
    std::string skip_callback_symbol{};

    // Explicit sample physical operations surrounding this producer. These are
    // physical-plan indices, not OutputPort behavior or runtime objects.
    std::vector<std::size_t> sample_carry_restores_before{};
    std::vector<std::size_t> sample_materializations_before{};
    std::vector<std::size_t> sample_feedback_writes_after{};
    std::vector<std::size_t> sample_materializations_after{};
    std::vector<std::size_t> sample_compositions_after{};
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

struct ExecutionPlan {
    // Ordered root execution sequence. For disconnected zero-port primitives this
    // is configured-bundle order; connection-aware scheduling will later replace
    // that provisional order without changing the LLVM-emission boundary.
    std::vector<PrimitiveExecutionStep> primitive_steps{};
    std::vector<ExecutionRegionPlan> regions{};
};


struct RootStackBufferPlan {
    std::size_t size_bytes = 0;
    std::size_t alignment = 1;
    std::size_t sample_offset = 0;
    std::size_t event_offset = 0;
};

struct LoweringPlan {
    // Pure graph/topology analysis remains part of the stable lowering plan;
    // sample physical realization consumes it without introducing a parallel
    // topology or policy model.
    ConnectionAnalysisPlan connections{};
    DeclarationPlan declarations{};
    PackageImportPlan imports{};
    ConfigurationPlan configurations{};
    SamplePortBindingPlan sample_ports{};
    EventPortBindingPlan event_ports{};
    RootStackBufferPlan root_stack{};
    ExecutionPlan execution{};
};

std::expected<LoweringPlan, std::string> build_lowering_plan(
    LoweringInput const& input);
} // namespace iv::graph_jit::detail
