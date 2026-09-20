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
};

struct PrimitiveEventInputBindingPlan {
    std::optional<std::size_t> representation{};
};

struct PrimitiveEventOutputBindingPlan {
    std::optional<std::size_t> representation{};
    EventTypeId source_type = EventTypeId::empty;
    std::size_t history = 0;
    std::size_t latency = 0;
    // Logical capacity available to this producer. This normally matches the
    // representation capacity; a transient fan-in home producer writes into a
    // larger aggregate representation while retaining its own declared bound.
    std::size_t write_capacity = 0;
    bool append_existing = false;
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
    // capacity remains source-capacity-sized because max_events_per_sample is
    // only a storage-sizing rate, not a runtime density constraint.
    std::size_t history_samples = 0;
    bool select_invocation_window = false;
    // Flattened schedule position of the producer. Execution planning keeps this
    // operation step-local for consumers inside the same SCC, or lifts it to the
    // cyclic region exit when every consumer is downstream of that SCC.
    std::size_t after_execution_position = 0;
};

struct EventCarryPlan {
    std::size_t working_representation = 0;
    std::size_t persistent_representation = 0;
    std::size_t producer_execution_position = 0;
    std::size_t retained_history_samples = 0;
    std::size_t retained_latency_samples = 0;
};

struct EventPersistentRingPlan {
    std::size_t representation = 0;
    std::size_t producer_execution_position = 0;
    std::size_t retained_history_samples = 0;
};

struct EventMergePlan {
    // Sorted producer streams are merged in semantic source order only after
    // every producer has completed its root invocation. For transient fan-in,
    // semantic source 0 writes directly into target_representation and the
    // remaining source_representations are merged into it in one k-way pass.
    // Retained fan-in keeps its current separate-target realization.
    std::vector<std::size_t> source_representations{};
    std::size_t target_representation = 0;
    std::size_t after_execution_position = 0;
    bool target_is_semantic_source = false;
    // Retained targets already contain restored/pruned events from previous
    // invocations; transient producer-home targets contain source 0 instead.
    bool preserve_existing_target = false;
};

struct EventFeedbackPlan {
    std::size_t source_representation = 0;
    std::size_t target_representation = 0;
    std::size_t producer_execution_position = 0;
    std::size_t consumer_execution_position = 0;
    RealtimeBufferStorageKind storage =
        RealtimeBufferStorageKind::transient_stack;
    std::size_t retained_window_samples = 0;
    std::size_t loop_extra_latency = 1;
};

struct PrimitiveEventPortPlan {
    std::vector<PrimitiveEventInputBindingPlan> inputs{};
    std::vector<PrimitiveEventOutputBindingPlan> outputs{};
};

struct EventPortBindingPlan {
    // Indexed by ConnectionAnalysisPlan::event_producer_groups.
    std::vector<std::optional<std::size_t>> producer_group_representations{};
    std::vector<EventRepresentationPlan> representations{};
    std::vector<EventMaterializationPlan> materializations{};
    std::vector<EventCarryPlan> carry_operations{};
    std::vector<EventPersistentRingPlan> persistent_rings{};
    std::vector<EventMergePlan> merges{};
    std::vector<EventFeedbackPlan> feedback_operations{};
    std::vector<EventTransientAllocationPlan> transient_allocations{};
    std::size_t transient_arena_size = 0;
    std::size_t transient_arena_alignment = 1;
    // Indexed by analyzed concrete primitive.
    std::vector<PrimitiveEventPortPlan> primitives{};
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

    // Event transient-sequence flow clears the producer sequence once before
    // all of its slices, then materializes a consumer-facing sequence after
    // the complete producer step.
    std::vector<std::size_t> event_sequence_resets_before{};
    std::vector<std::size_t> event_persistent_ring_prunes_before{};
    std::vector<std::size_t> event_carry_restores_before{};
    std::vector<std::size_t> event_merges_after{};
    std::vector<std::size_t> event_materializations_after{};
    std::vector<std::size_t> event_carry_commits_after{};
    std::vector<std::size_t> event_feedback_appends_after{};
};

struct ExecutionRegionPlan {
    std::vector<std::size_t> primitive_steps{};
    // Retained event state owned by a cyclic producer is root-invocation state,
    // not slice state. These operations therefore surround the complete
    // slice-major SCC traversal. Indices refer to the corresponding
    // EventPortBindingPlan vectors.
    std::vector<std::size_t> event_persistent_ring_prunes_before{};
    std::vector<std::size_t> event_carry_restores_before{};
    std::vector<std::size_t> event_materializations_after{};
    std::vector<std::size_t> event_carry_commits_after{};
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
    ExecutionPlan execution{};
};

std::expected<LoweringPlan, std::string> build_lowering_plan(
    LoweringInput const& input);
} // namespace iv::graph_jit::detail
