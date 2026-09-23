#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph/realtime_port_planning.h>
#include <intravenous/graph_jit/indexed_plan.h>

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
    indexed_to_indexed,
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

    // Only sequential producers impose ordinary tick ordering. An indexed
    // output is materialized by the indexed executor and must not be
    // ordered by pretending its producer tick creates that output.
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
    // Indexed by configured NodeBundleHandle. Boundary/non-concrete bundles
    // have no region.
    std::vector<std::optional<std::size_t>> bundle_to_region{};
    // Indexed by configured NodeBundleHandle. Values are positions in the
    // flattened provisional execution schedule.
    std::vector<std::optional<std::size_t>> bundle_execution_position{};
};

struct SampleSourceChannelTimingPlan {
    SampleOutputChannelId source{};
    ChannelLayout source_layout{};
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
// canonical target-port channel ordinals in the semantic target-channel order
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
    // latency until physical channel composition is lowered explicitly.
    std::size_t read_latency = 0;
    std::size_t target_history = 0;
    PlannedConnectionAccess access = PlannedConnectionAccess::realtime_to_realtime;
    bool requires_conversion = false;
    bool requires_block_materialization = false;
    bool external_boundary = false;
    std::optional<ConfiguredSampleConnectionDetach> detach{};
    std::optional<Sample> detach_initial_value{};
    std::optional<std::size_t> detach_region{};
};

struct EventConnectionPlan {
    std::size_t configured_connection_index = 0;
    EventTypeId source_type = EventTypeId::empty;
    std::vector<EventOutputPortId> sources{};
    EventTypeId target_type = EventTypeId::empty;
    std::vector<EventInputPortId> targets{};
    EventConversionPlan conversion{};

    std::size_t source_history = 0;
    std::size_t source_latency = 0;
    std::size_t target_history = 0;
    double max_events_per_index = 0.0;
    PlannedConnectionAccess access = PlannedConnectionAccess::realtime_to_realtime;
    bool requires_conversion = false;
    bool requires_block_materialization = false;
    bool external_boundary = false;
    std::optional<ConfiguredEventConnectionDetach> detach{};
    std::optional<std::size_t> detach_region{};
};


struct ConnectionLiveIntervalPlan {
    // Positions in SchedulePlan's flattened concrete-node order. Boundary
    // ingress is represented by 0; boundary egress by execution_count.
    std::size_t begin = 0;
    std::size_t end = 0;
    bool crosses_kernel_invocations = false;
};

struct SampleProducerGroupPlan {
    // Physical producer identity is the declared output port, not the
    // connection's possibly-composed semantic source channel set.
    std::optional<NodeBundlePortId> source_port{};
    ChannelTypeId source_type = ChannelTypeId::mono;
    std::vector<SampleOutputChannelId> source_channels{};
    std::optional<ChannelLayout> canonical_source_layout{};
    std::vector<std::size_t> connection_indices{};
    bool has_realtime_connections = false;
    bool has_indexed_connections = false;
    SampleConnectionStorageRequirements storage_requirements{};
    std::optional<SampleConnectionStoragePlan> storage_plan{};
    ConnectionLiveIntervalPlan live_interval{};
};

struct EventProducerGroupPlan {
    EventTypeId source_type = EventTypeId::empty;
    std::vector<EventOutputPortId> sources{};
    double max_events_per_index = 0.0;
    std::vector<std::size_t> connection_indices{};
    bool has_realtime_connections = false;
    bool has_indexed_connections = false;
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

// This is a semantic storage request, not an allocated region. Sample physical
// realization consumes these requirements after storage-plan selection;
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
    // The indexed plan owns the complete semantic SCC decomposition as well as
    // the indexed-only topology. Later lowering/runtime stages retain and reuse
    // it instead of rediscovering either graph view.
    IndexedPlan indexed{};
};

// Pure host-side topology/temporal/storage-requirement analysis. It does not
// require LLVM or resolved package implementations and is intentionally usable
// before the current lowering capability gate. That lets connected graphs be
// analyzed and unit-tested before their port contexts are executable.
std::expected<ConnectionAnalysisPlan, std::string> build_connection_analysis_plan(
    ConfiguredGraph const& graph,
    std::size_t kernel_block_size,
    RealtimeStorageCostModel const& cost_model = {},
    // Stack-pressure retries change only realtime physical policy. Supplying
    // the immutable plan retained from the first analysis avoids repeating
    // semantic SCC/indexed topology work for the same graph specialization.
    IndexedPlan const* retained_indexed_plan = nullptr);

} // namespace iv::graph_jit::detail
