#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph/realtime_port_planning.h>
#include <intravenous/graph_jit/background_evaluation_plan.h>

#include <cstddef>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace iv::graph_jit::detail {

enum class PlannedConnectionData {
    sample,
    event,
};

using ::iv::graph_jit::PlannedDestinationAccess;
using ::iv::graph_jit::PlannedDeliveryMechanism;
using ::iv::graph_jit::PlannedSourceProduction;

struct PlannedGraphNode {
    NodeBundleHandle bundle = 0;
    std::size_t internal_latency_samples = 0;
    std::size_t maximum_block_size = 0;
    std::size_t sample_input_count = 0;
    std::size_t sample_output_count = 0;
    std::size_t event_input_count = 0;
    std::size_t event_output_count = 0;
    bool intrinsically_replayable = false;
    bool contextually_replayable = false;
};

struct DependencyEdgePlan {
    NodeBundleHandle source_bundle = 0;
    NodeBundleHandle target_bundle = 0;
    PlannedConnectionData data = PlannedConnectionData::sample;
    PlannedSourceProduction source_production = PlannedSourceProduction::tick;
    OutputRetention source_retention = OutputRetention::ephemeral;
    PlannedDestinationAccess destination_access = PlannedDestinationAccess::sequential;
    PlannedDeliveryMechanism delivery = PlannedDeliveryMechanism::tick_to_sequential;
    std::size_t configured_connection_index = 0;

    // Only Tick -> Sequential transport imposes ordinary same-slice ordering.
    // Background materialization and replay dependencies are retained
    // separately by BackgroundEvaluationPlan.
    bool sequential_tick_dependency = true;
};

struct SccRegionPlan {
    std::vector<NodeBundleHandle> nodes{};
    // Explicit (non-detached) dependencies remain acyclic. Cyclic regions are
    // created only by restoring validated detached source->target dependencies,
    // so this order is a deterministic topological order with those temporal
    // feedback dependencies omitted.
    std::vector<NodeBundleHandle> execution_order{};
    bool cyclic = false;
    std::size_t maximum_block_size = 0;
    // Scheduling quantum exposed to callbacks in this region. This is derived
    // from node block limits and internal detach latencies; it is not the
    // transport delay of any particular detach.
    std::size_t scc_feedback_latency = 0;
};

struct SchedulePlan {
    std::vector<SccRegionPlan> regions{};
    std::vector<std::size_t> region_order{};
    // Background by configured NodeBundleHandle. Boundary/non-concrete bundles
    // have no region.
    std::vector<std::optional<std::size_t>> bundle_to_region{};
    // Background by configured NodeBundleHandle. Values are positions in the
    // flattened provisional execution schedule.
    std::vector<std::optional<std::size_t>> bundle_execution_position{};
};

struct SampleSourceChannelTimingPlan {
    SampleOutputChannelId source{};
    ChannelLayout source_layout{};
    PlannedSourceProduction production = PlannedSourceProduction::tick;
    OutputRetention retention = OutputRetention::ephemeral;
    PlannedDestinationAccess destination_access = PlannedDestinationAccess::sequential;
    PlannedDeliveryMechanism delivery = PlannedDeliveryMechanism::tick_to_sequential;
    bool contextually_replayable = false;
    std::size_t source_history = 0;
    std::size_t source_latency = 0;
    // Effective latency for this particular source channel after feed-forward
    // path equalization. Composed sample inputs may require a different delay
    // per source channel even though a canonical whole-port source always has
    // one common value.
    std::size_t read_latency = 0;
};

// One semantic contribution to a normalized target-port composition. Source
// indices name entries in SampleConnectionPlan::source_channel_timings in the
// semantic source-channel order expected by source_type. target_channels are
// canonical target-port channel indices in the semantic target-channel order
// produced by target_type. Keeping only indices here leaves the central timing
// vector authoritative when latency compensation later adjusts read_latency.
struct SampleProjectionContributionPlan {
    ChannelTypeId source_type = ChannelTypeId::mono;
    std::vector<std::size_t> source_channel_indices{};
    ChannelTypeId target_type = ChannelTypeId::mono;
    std::vector<std::size_t> target_channels{};
};

struct SampleConnectionPlan {
    std::size_t configured_connection_index = 0;
    ChannelTypeId source_type = ChannelTypeId::mono;
    std::vector<SampleOutputChannelId> source_channels{};
    std::vector<SampleSourceChannelTimingPlan> source_channel_timings{};
    // Non-empty only after several/partial configured connections have been
    // normalized into one whole target-port composition. Each contribution
    // retains its own gather -> semantic channel-conversion -> projection
    // boundary rather than flattening channel-count conversion away.
    std::vector<SampleProjectionContributionPlan> projection_contributions{};
    std::optional<NodeBundlePortId> canonical_source_port{};
    std::optional<ChannelLayout> canonical_source_layout{};
    ChannelTypeId target_type = ChannelTypeId::mono;
    ChannelLayout target_layout{};
    // After partial/projected connections are normalized, this covers the
    // whole target port in canonical declaration order. The actual semantic
    // projection/permutation remains in projection_contributions.target_channels.
    std::vector<SampleInputChannelId> target_channels{};
    NodeBundlePortId target_port{};

    // Conservative aggregate values retained for whole-port planning. The
    // per-channel timing vector above is authoritative when source_channels
    // compose channels from different producer ports.
    std::size_t source_history = 0;
    std::size_t source_latency = 0;
    // For a canonical whole-port source this is the common effective InputPort
    // latency. For a composed source it is the maximum per-channel read
    // latency until storage channel composition is lowered explicitly.
    std::size_t read_latency = 0;
    std::size_t target_history = 0;
    PlannedDestinationAccess destination_access = PlannedDestinationAccess::sequential;
    bool requires_conversion = false;
    bool requires_block_materialization = false;
    bool external_boundary = false;
    std::optional<ConfiguredSampleConnectionDetach> detach{};
    std::optional<Sample> detach_initial_value{};
    std::optional<std::size_t> detach_region{};
};


struct EventSourcePlan {
    EventOutputPortId source{};
    PlannedSourceProduction production = PlannedSourceProduction::tick;
    OutputRetention retention = OutputRetention::ephemeral;
    std::size_t history = 0;
    std::size_t latency = 0;
    double max_events_per_index = 0.0;
};

struct EventTargetPlan {
    EventInputPortId target{};
    PlannedDestinationAccess access = PlannedDestinationAccess::sequential;
    std::size_t history = 0;
};

struct EventDeliveryPlan {
    std::size_t source_index = 0;
    std::size_t target_index = 0;
    PlannedDeliveryMechanism mechanism = PlannedDeliveryMechanism::tick_to_sequential;
    bool contextually_replayable = false;
};

struct EventConnectionPlan {
    std::size_t configured_connection_index = 0;
    EventTypeId source_type = EventTypeId::empty;
    std::vector<EventOutputPortId> sources{};
    std::vector<EventSourcePlan> source_plans{};
    EventTypeId target_type = EventTypeId::empty;
    std::vector<EventInputPortId> targets{};
    std::vector<EventTargetPlan> target_plans{};
    std::vector<EventDeliveryPlan> deliveries{};
    EventConversionPlan conversion{};

    std::size_t source_history = 0;
    std::size_t source_latency = 0;
    std::size_t target_history = 0;
    double max_events_per_index = 0.0;
    bool requires_conversion = false;
    bool requires_block_materialization = false;
    bool external_boundary = false;
    std::optional<ConfiguredEventConnectionDetach> detach{};
    std::optional<std::size_t> detach_region{};
};


[[nodiscard]] constexpr bool uses_realtime_storage(
    PlannedDeliveryMechanism delivery) noexcept
{
    return delivery == PlannedDeliveryMechanism::tick_to_sequential;
}

[[nodiscard]] inline bool has_realtime_delivery(
    SampleConnectionPlan const& connection) noexcept
{
    return std::ranges::any_of(
        connection.source_channel_timings,
        [](SampleSourceChannelTimingPlan const& source) {
            return uses_realtime_storage(source.delivery);
        });
}

[[nodiscard]] inline bool is_entirely_realtime_delivery(
    SampleConnectionPlan const& connection) noexcept
{
    return !connection.source_channel_timings.empty()
        && std::ranges::all_of(
            connection.source_channel_timings,
            [](SampleSourceChannelTimingPlan const& source) {
                return uses_realtime_storage(source.delivery);
            });
}

[[nodiscard]] inline bool has_realtime_delivery(
    EventConnectionPlan const& connection) noexcept
{
    return std::ranges::any_of(
        connection.deliveries,
        [](EventDeliveryPlan const& delivery) {
            return uses_realtime_storage(delivery.mechanism);
        });
}

[[nodiscard]] inline bool is_entirely_realtime_delivery(
    EventConnectionPlan const& connection) noexcept
{
    return !connection.deliveries.empty()
        && std::ranges::all_of(
            connection.deliveries,
            [](EventDeliveryPlan const& delivery) {
                return uses_realtime_storage(delivery.mechanism);
            });
}

struct ConnectionLiveIntervalPlan {
    // Positions in SchedulePlan's flattened concrete-node order. Boundary
    // ingress is represented by 0; boundary egress by execution_count.
    std::size_t begin = 0;
    std::size_t end = 0;
    bool crosses_kernel_invocations = false;
};

struct SampleProducerGroupPlan {
    // Storage producer identity is the declared output port, not the
    // connection's possibly-composed semantic source channel set.
    std::optional<NodeBundlePortId> source_port{};
    ChannelTypeId source_type = ChannelTypeId::mono;
    std::vector<SampleOutputChannelId> source_channels{};
    // Port atoms are the correctness units. This group is a later storage
    // coalescing decision for the node-facing Tick storage.
    std::vector<PortSubsetIndex> source_atom_indices{};
    std::optional<ChannelLayout> canonical_source_layout{};
    std::vector<std::size_t> connection_indices{};
    bool has_realtime_connections = false;
    bool has_background_connections = false;
    SampleConnectionStorageRequirements storage_requirements{};
    std::optional<SampleConnectionStoragePlan> storage_plan{};
    ConnectionLiveIntervalPlan live_interval{};
};

struct EventProducerGroupPlan {
    EventTypeId source_type = EventTypeId::empty;
    std::vector<EventOutputPortId> sources{};
    std::vector<PortSubsetIndex> source_atom_indices{};
    std::vector<PortSubsetIndex> target_atom_indices{};
    // The semantic fan-in source set is retained above. Realtime storage
    // planning uses only Tick -> Sequential contributors; background-only
    // sources are tracked separately so mixed fan-in is representable without
    // allocating realtime storage for Tock/replay materialization.
    std::vector<EventOutputPortId> realtime_sources{};
    std::vector<EventOutputPortId> background_sources{};
    double max_events_per_index = 0.0;
    std::vector<std::size_t> connection_indices{};
    bool has_realtime_connections = false;
    bool has_background_connections = false;
    // Cyclic or block-adapted producers append into one invocation-wide
    // sequence. This is an execution/materialization fact, not a storage kind.
    bool requires_invocation_aggregate = false;
    EventConnectionStorageRequirements storage_requirements{};
    std::optional<EventConnectionStoragePlan> storage_plan{};
    ConnectionLiveIntervalPlan live_interval{};
};

enum class ConnectionStorageLifetime {
    transient,
    persistent,
    external,
};

// This is a semantic storage request, not an allocated region. Sample storage
// realization consumes these requirements after storage-plan selection;
// transient byte-range arena allocation is a separate concern from policy choice.
struct ConnectionStorageRegionRequirement {
    PlannedConnectionData data = PlannedConnectionData::sample;
    std::size_t producer_group_index = 0;
    ConnectionStorageLifetime lifetime = ConnectionStorageLifetime::transient;
    ConnectionLiveIntervalPlan live_interval{};
    std::size_t current_block_frames = 0;
    std::size_t retained_extent = 0;
    std::size_t channel_count = 1;
    std::size_t value_size_bytes = 0;
};

struct ConnectionStoragePlan {
    std::vector<ConnectionStorageRegionRequirement> regions{};
};

struct ConnectionAnalysisPlan {
    NodeBundleHandle boundary_bundle = 0;
    std::vector<PlannedGraphNode> nodes{};
    std::vector<DependencyEdgePlan> dependencies{};
    SchedulePlan schedule{};
    std::vector<SampleConnectionPlan> sample_connections{};
    std::vector<EventConnectionPlan> event_connections{};
    std::vector<SampleProducerGroupPlan> sample_producer_groups{};
    std::vector<EventProducerGroupPlan> event_producer_groups{};
    ConnectionStoragePlan storage{};
    // The background plan owns the complete semantic SCC decomposition as well as
    // the background/background topology. Later lowering/runtime stages retain and
    // reuse it instead of rediscovering either graph view.
    BackgroundEvaluationPlan background{};
};

// Pure host-side topology/temporal/storage-requirement analysis. It does not
// require LLVM or resolved package implementations and is intentionally usable
// before the current lowering capability gate. That lets connected graphs be
// analyzed and unit-tested before their port contexts are executable.
std::expected<ConnectionAnalysisPlan, std::string> build_connection_analysis_plan(
    ConfiguredGraph const& graph,
    std::size_t kernel_block_size,
    RealtimeStorageCostModel const& cost_model = {},
    // Stack-pressure retries change only realtime storage policy. Supplying
    // the immutable plan retained from the first analysis avoids repeating
    // semantic SCC/background topology work for the same graph specialization.
    BackgroundEvaluationPlan const* retained_background_plan = nullptr);

} // namespace iv::graph_jit::detail
