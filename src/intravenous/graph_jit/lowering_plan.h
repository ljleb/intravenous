#pragma once

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/graph_jit/connection_plan.h>
#include <intravenous/graph_jit/lowering/event_port_plan.h>
#include <intravenous/graph_jit/sample_storage_plan.h>
#include <intravenous/graph_jit/lowering.h>
#include <intravenous/node/config_relocations.h>

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
    // A transition-only suffix can expose an old value followed by the new
    // immutable value. Keep the primitive awake for that transition without
    // adding remembered-value storage to the steady plan.
    bool changed_during_transition = false;
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
// value. The realtime root also shares the compact fact while the source
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

struct LiveTickCoverageHistoryPlan {
    std::size_t output_change = 0;
    std::size_t primitive = 0;
    std::int32_t pace = 0;
    std::size_t capacity = 0;
    NodeLayout::RegionHandle region{};
    std::size_t storage_offset = 0;
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
    // TTL is disabled and the realtime root pays no activity cost.
    std::vector<std::optional<PrimitiveActivityPlan>> primitive_activity{};
    std::vector<ContinuousActivityFrontierPlan> activity_frontiers{};
    std::vector<LiveTickCoverageHistoryPlan> live_coverage_histories{};
};

// The project module imports a package callback by declaring the symbol GraphJit
// gave its node-code address; the generation JITDylib defines that symbol.
struct CallbackImportPlan {
    std::string symbol{};
    std::string role{};
    CallbackImportAbi abi = CallbackImportAbi::block;
};

struct RetainedGlobalImportPlan {
    std::string symbol{};
    std::size_t size = 0;
    std::size_t alignment = 1;
};

struct PackageImportGroup {
    std::size_t package_index = 0;
    std::vector<CallbackImportPlan> callbacks{};
    std::vector<RetainedGlobalImportPlan> retained_globals{};
};

struct PrimitiveCallbackPlan {
    NodeBundleHandle bundle = 0;
    bool resolves_module_input = false;
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
    std::string initialize_sequential_input{};
    std::string combine_sequential_input_values{};
};

struct PackageImportPlan {
    // Compiler-owned operations have no PackageRevision or package code.
    std::vector<CallbackImportPlan> compiler_callbacks{};
    // One group per package code input that the project module imports from.
    std::vector<PackageImportGroup> packages{};
    // One entry per analyzed concrete primitive, naming the imported callback
    // symbols. Several primitives share one symbol when they select the same
    // package-local implementation.
    std::vector<PrimitiveCallbackPlan> primitive_callbacks{};
};

struct NodeConfigurationRelocationPlan {
    std::size_t byte_offset = 0;
    std::size_t addend = 0;
    // Empty means an explicit null pointer index. Non-empty names the imported
    // retained LLVM global whose byte-address plus addend reconstructs the
    // configured pointer value.
    std::string retained_global_symbol{};
    std::shared_ptr<ImmutableConfigData const> immutable_data{};
};

struct NodeConfigurationPlan {
    std::optional<ContinuousInputConfig> module_input_config{};
    // Own the configured bytes so LLVM emission does not depend on native
    // configured-object addresses after host-side planning completes. Pointer
    // indices are zeroed here and reconstructed symbolically during LLVM
    // emission; native process addresses must never enter project IR.
    std::vector<std::byte> bytes{};
    std::vector<NodeConfigurationRelocationPlan> relocations{};
    std::size_t alignment = 1;
    ReflectedNodeTickContext tick_context_template{};
    std::string node_global_symbol{};
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
    std::size_t contiguous_storage_capacity = 0;
    bool has_contiguous_scratch = false;
    // Background evaluation supplies this Sequential input's current block
    // through its Sequential Tick input slot on every invocation. Without
    // history it has no channels and the realtime root binds the slot
    // directly. With history, channels name the input's node-owned timeline:
    // the root writes each delivered block into it before the callback, so
    // its history is the input's own record of earlier deliveries.
    bool invocation_supplied = false;
    // The slot holds the background contribution and realtime_operand_channels
    // the Tick contribution. The root combines both, one current block at a
    // time, into the timeline that channels name.
    bool combines_invocation_supplied = false;
    std::vector<PrimitiveSampleInputChannelBindingPlan>
        realtime_operand_channels{};
    // A participating manual control writes the current block into this
    // input's retained timeline. For an ordinary realtime-only connection,
    // manual_operand_channels names the already-composed input that is copied
    // before the node-specific combine operation adds the manual contribution.
    // Slot-fed inputs are staged first by the existing invocation path.
    std::optional<std::size_t> manual_control{};
    // Canonical retained timeline shared by constant and dynamic control code.
    // Constant disconnected code can keep history current here while the
    // node-facing binding remains an immutable compiler constant.
    std::vector<PrimitiveSampleInputChannelBindingPlan>
        manual_timeline_channels{};
    std::vector<PrimitiveSampleInputChannelBindingPlan>
        manual_operand_channels{};
    // A Random Access input reads only its TickInvocationCall Random Access
    // view. Its declared entry in the reflected Sequential binding array has
    // no storage and is never read by node code.
    bool random_access = false;
    std::vector<SequentialCoverageProjection> coverage_projections{};
    std::vector<SequentialCoverageProjection> realtime_coverage_projections{};
    std::vector<SequentialCoverageProjection> background_coverage_projections{};
    bool resolves_coverage = false;
    std::vector<std::byte> neutral_value{};
    std::vector<std::byte> fallback_value{};
    std::vector<PrimitiveSampleInputChannelBindingPlan> coverage_operand_channels{};
};

struct ManualControlPlan {
    ConfiguredManualControl configured{};
    std::size_t primitive_index = 0;
    std::size_t input_index = 0;
    bool participates = false;
    bool stages_timeline = false;
    // Use this value only on known-absent positions, after ordinary input
    // composition. It is not an additional fan-in contribution.
    bool fallback_only = false;
    // Routed module-input values are added within the connection composition,
    // before a direct leaf manual control uses the leaf's combine operation.
    bool boundary_source = false;
    bool first_for_input = true;
    std::vector<float> channel_coefficients{};
    std::size_t delay = 0;
    std::size_t delay_capacity = 0;
    std::vector<Sample> delay_initial_values{};
    std::size_t delay_storage_offset = 0;
    NodeLayout::RegionHandle state_region{};
    std::size_t state_storage_offset = 0;
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
    // The node-level static_sample_value fact has been validated against this
    // primitive and every realtime output is backed by immutable storage.
    // Connection analysis omits the primitive from the realtime schedule.
    bool tick_callback_elided = false;
};

struct SamplePortBindingPlan {
    SampleStoragePlan storage{};
    // One entry per analyzed concrete primitive.
    std::vector<PrimitiveSamplePortPlan> primitives{};
    std::vector<ManualControlPlan> manual_controls{};
    std::size_t contiguous_input_scratch_size = 0;
    std::size_t contiguous_input_scratch_alignment = 1;

    [[nodiscard]] bool empty() const noexcept
    {
        return storage.empty();
    }
};

struct PrimitiveExecutionStep {
    std::size_t configuration_index = 0;
    std::size_t storage_index = 0;
    // Maximum block size accepted by this primitive. The realtime root uses
    // this as the single invocation-slicing boundary for both tick and skip.
    std::size_t maximum_block_size = 0;
    // Node-code identities of the batch callbacks. Ready-group planning
    // batches only primitives that share both.
    std::string tick_batch_callback_symbol{};
    std::string skip_batch_callback_symbol{};

    // Explicit sample storage operations surrounding this producer. A carry
    // whose endpoints share one cyclic region belongs to ExecutionRegionPlan
    // instead. These are storage-plan indices, not OutputPort behavior or
    // runtime objects.
    std::vector<std::size_t> sample_carry_restores_before{};
    std::vector<std::size_t> sample_materializations_before{};
    std::vector<std::size_t> sample_compositions_before{};
    // Transition-root materializations that must observe this step's
    // sample_compositions_before and run before its callback reads them.
    std::vector<std::size_t> sample_transition_materializations_before{};
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

    [[nodiscard]] bool has_port_operations() const noexcept
    {
        return !sample_carry_restores_before.empty()
            || !sample_materializations_before.empty()
            || !sample_compositions_before.empty()
            || !sample_transition_materializations_before.empty()
            || !sample_feedback_writes_after.empty()
            || !sample_materializations_after.empty()
            || !sample_compositions_after.empty()
            || !sample_transition_materializations_after.empty()
            || !sample_carry_commits_after.empty()
            || !event_operations_before.empty()
            || !event_operations_after.empty();
    }
};

struct ExecutionRegionPlan {
    std::vector<std::size_t> primitive_steps{};
    // A compact sample carry crosses complete root calls. When both of its
    // execution positions belong to this cyclic region, restore and commit
    // surround the complete sliced invocation rather than every slice.
    std::vector<std::size_t> sample_carry_restores_before{};
    std::vector<std::size_t> sample_carry_commits_after{};
    std::vector<EventOperationRef> event_operations_before{};
    std::vector<EventOperationRef> event_operations_after{};
    bool cyclic = false;
    std::size_t maximum_block_size = 0;
    std::size_t scc_feedback_latency = 0;
};

struct ExecutionReadyGroupPlan {
    // A multi-step entry is one compatible antichain discovered from one
    // scheduler ready-frontier snapshot. Planning keeps a multi-step entry
    // only when the realtime root can run it as one native batch; otherwise its
    // members return to the scalar schedule. Cyclic SCC execution is
    // represented by cyclic_regions. Its primitive_steps are phase-major:
    // one contiguous lane set per corresponding cyclic-region step.
    std::vector<std::size_t> primitive_steps{};
    std::vector<std::size_t> cyclic_regions{};
};

struct ExecutionPlan {
    // Ordered root execution sequence. For disconnected zero-port primitives this
    // is configured-bundle order; connection-aware scheduling will later replace
    // that provisional order without changing the realtime-root boundary.
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
