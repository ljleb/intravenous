#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph/realtime_port_planning.h>

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace iv::graph_jit::detail {

enum class PlannedConnectionPayload {
    sample,
    event,
};

enum class PlannedConnectionAccess {
    realtime_to_realtime,
    compiled_to_compiled,
    compiled_to_realtime,
    realtime_to_compiled,
};

struct PlannedGraphNode {
    NodeBundleHandle bundle = 0;
    std::size_t internal_latency_samples = 0;
    std::size_t maximum_block_size = 0;
    std::size_t sample_input_count = 0;
    std::size_t sample_output_count = 0;
    std::size_t event_input_count = 0;
    std::size_t event_output_count = 0;
};

struct DependencyEdgePlan {
    NodeBundleHandle source_bundle = 0;
    NodeBundleHandle target_bundle = 0;
    PlannedConnectionPayload payload = PlannedConnectionPayload::sample;
    PlannedConnectionAccess access = PlannedConnectionAccess::realtime_to_realtime;
    std::size_t configured_connection_index = 0;

    // Only sequential producers impose ordinary tick ordering. A compiled
    // output is materialized by the compiled-access executor and must not be
    // ordered by pretending its producer tick creates that output.
    bool sequential_tick_dependency = true;
};

struct SccRegionPlan {
    std::vector<NodeBundleHandle> nodes{};
    // Point 12 will replace this provisional deterministic order for cyclic
    // regions with the feedback-aware execution schedule. Acyclic singleton
    // regions already have their final order.
    std::vector<NodeBundleHandle> execution_order{};
    bool cyclic = false;
    std::size_t maximum_block_size = 0;
};

struct SchedulePlan {
    std::vector<SccRegionPlan> regions{};
    std::vector<std::size_t> region_order{};
    // Indexed by configured NodeBundleHandle. Boundary/non-concrete bundles
    // have no region.
    std::vector<std::optional<std::size_t>> bundle_to_region{};
    // Indexed by configured NodeBundleHandle. Values are positions in the
    // flattened provisional execution schedule.
    std::vector<std::optional<std::size_t>> bundle_execution_position{};
};

struct SampleConnectionPlan {
    std::size_t configured_connection_index = 0;
    ChannelTypeId source_type = ChannelTypeId::mono;
    std::vector<SampleOutputChannelId> source_channels{};
    std::optional<NodeBundlePortId> canonical_source_port{};
    std::optional<ChannelLayout> canonical_source_layout{};
    ChannelTypeId target_type = ChannelTypeId::mono;
    ChannelLayout target_layout{};
    std::vector<SampleInputChannelId> target_channels{};
    NodeBundlePortId target_port{};

    std::size_t source_history = 0;
    std::size_t source_latency = 0;
    // Effective InputPort latency after whole-graph feed-forward path
    // equalization. This is at least source_latency; the difference is the
    // compiler-inserted compensation for a faster path converging with a
    // slower sibling path.
    std::size_t read_latency = 0;
    std::size_t target_history = 0;
    PlannedConnectionAccess access = PlannedConnectionAccess::realtime_to_realtime;
    bool requires_conversion = false;
    bool requires_block_materialization = false;
    bool external_boundary = false;
    bool feedback = false;
};

struct EventConnectionPlan {
    std::size_t configured_connection_index = 0;
    EventTypeId source_type = EventTypeId::empty;
    std::vector<EventOutputPortId> sources{};
    EventTypeId target_type = EventTypeId::empty;
    std::vector<EventInputPortId> targets{};

    std::size_t source_history = 0;
    std::size_t source_latency = 0;
    std::size_t target_history = 0;
    PlannedConnectionAccess access = PlannedConnectionAccess::realtime_to_realtime;
    bool requires_conversion = false;
    bool requires_block_materialization = false;
    bool external_boundary = false;
    bool feedback = false;
};

struct ConnectionLiveIntervalPlan {
    // Positions in SchedulePlan's flattened concrete-node order. Boundary
    // ingress is represented by 0; boundary egress by execution_count.
    std::size_t begin = 0;
    std::size_t end = 0;
    bool crosses_kernel_invocations = false;
};

struct SampleProducerGroupPlan {
    ChannelTypeId source_type = ChannelTypeId::mono;
    std::vector<SampleOutputChannelId> source_channels{};
    std::optional<ChannelLayout> canonical_source_layout{};
    std::vector<std::size_t> connection_indices{};
    bool has_realtime_connections = false;
    bool has_compiled_connections = false;
    SampleConnectionImplementationRequirements requirements{};
    std::optional<SampleConnectionImplementationKind> implementation{};
    ConnectionLiveIntervalPlan live_interval{};
};

struct EventProducerGroupPlan {
    EventTypeId source_type = EventTypeId::empty;
    std::vector<EventOutputPortId> sources{};
    std::vector<std::size_t> connection_indices{};
    bool has_realtime_connections = false;
    bool has_compiled_connections = false;
    EventConnectionImplementationRequirements requirements{};
    std::optional<EventConnectionImplementationKind> implementation{};
};

enum class ConnectionStorageLifetime {
    transient,
    persistent,
    external,
};

// This is a semantic storage request, not an allocated region. Sample physical
// realization consumes these requirements after implementation selection;
// transient byte-range arena allocation is a separate concern from policy choice.
struct ConnectionStorageRegionRequirement {
    PlannedConnectionPayload payload = PlannedConnectionPayload::sample;
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
};

// Pure host-side topology/temporal/storage-requirement analysis. It does not
// require LLVM or resolved package implementations and is intentionally usable
// before the current lowering capability gate. That lets connected graphs be
// analyzed and unit-tested before their port contexts are executable.
std::expected<ConnectionAnalysisPlan, std::string> build_connection_analysis_plan(
    ConfiguredGraph const& graph,
    std::size_t kernel_block_size);

} // namespace iv::graph_jit::detail
