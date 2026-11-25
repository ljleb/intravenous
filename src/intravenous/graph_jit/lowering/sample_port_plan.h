#pragma once

#include <intravenous/graph_jit/sample_storage_plan.h>
#include <intravenous/node/compiler_record.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iv::graph_jit {
struct NodeImplementation;
}

namespace iv::graph_jit::detail {
struct GraphAnalysis;

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

// Coverage and manual value staging write the current block. With no
// callback history, in-flight frames still belong to the connection.
template<class Binding>
auto& sample_input_state_channels(Binding& input)
{
    if (!input.invocation_supplied && input.history == 0) {
        if (!input.manual_operand_channels.empty()) return input.manual_operand_channels;
        if (!input.coverage_operand_channels.empty()) return input.coverage_operand_channels;
    }
    return input.channels;
}

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

std::expected<std::vector<std::byte>, std::string> initialized_sequential_input_value(
    NodeImplementation const* implementation,
    std::size_t port,
    ContinuousValueStorageGeometry const& geometry,
    iv::details::SequentialInputInitialization initialization);

// Resolve the Tick operands and their detach initial values once for this
// connection analysis. Keep the connection indices used by its producer groups.
std::expected<std::vector<SampleConnectionPlan>, std::string> prepare_sample_storage_connections(
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections);

std::expected<SamplePortBindingPlan, std::string> plan_sample_ports(
    ConfiguredGraph const& graph,
    std::size_t block_size,
    GraphAnalysis const& analysis,
    SampleStorageConnections storage_connections,
    ResolvedPortRealization const& realization,
    RealtimeStorageCostModel const& cost_model,
    std::span<std::optional<Sample> const> static_sample_values);

} // namespace iv::graph_jit::detail
