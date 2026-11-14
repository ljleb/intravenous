#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph/realtime_port_planning.h>
#include <intravenous/graph_jit/background_evaluation_plan.h>
#include <intravenous/graph_jit/port_realization.h>
#include <intravenous/graph_jit/realtime_port_state_plan.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
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
    // Stable across compiled generations when this concrete node is a direct
    // member of an authored virtual node. Anonymous concrete nodes retain no
    // synthetic identity derived from their generation-local bundle handle.
    std::optional<StableConcreteNodeId> stable_identity{};
    std::size_t internal_latency_samples = 0;
    std::size_t maximum_block_size = 0;
    std::size_t sample_input_count = 0;
    std::size_t sample_output_count = 0;
    std::size_t event_input_count = 0;
    std::size_t event_output_count = 0;
    // Tock-only primitives participate in background dependency planning but
    // never acquire a position in the realtime execution schedule.
    bool tick_execution = true;
    bool intrinsically_replayable = false;
    bool contextually_replayable = false;
    bool authored_forward_coverage = false;
    bool authored_reverse_coverage = false;
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
    // Each entry is one snapshot of mutually ready SCC regions. A multi-entry
    // group contains either compatible acyclic one-node regions or compatible
    // cyclic regions whose corresponding execution-order entries have equal
    // callback batch classes. Later lowering may keep the group scalar, but it
    // need not rediscover whether its members form an antichain.
    std::vector<std::vector<std::size_t>> ready_groups{};
    // Entry positions correspond to configured NodeBundleHandle values.
    // Boundary/non-concrete bundles have no region.
    std::vector<std::optional<std::size_t>> bundle_to_region{};
    // Entry positions correspond to configured NodeBundleHandle values. Values
    // are positions in the flattened provisional execution schedule. For a
    // batched cyclic group, positions are phase-major across its regions.
    std::vector<std::optional<std::size_t>> bundle_execution_position{};
    // Entry positions correspond to configured NodeBundleHandle values. A set
    // entry means planning proved that the node's Tick result is immutable and
    // omitted its callback before constructing the schedule.
    std::vector<std::uint8_t> callback_elided{};
};

struct SampleSourceChannelTimingPlan {
    SampleOutputChannelId source{};
    ChannelLayout source_layout{};
    std::optional<ConfiguredSampleConnectionDetach> detach{};
    std::optional<std::vector<std::byte>> detach_initial_value{};
    std::optional<std::size_t> detach_region{};
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

// Optional target-major linear transform for a sample route composed from
// several ConfiguredSampleConnection records. An empty coefficient vector
// means the ordinary registered conversion from source_type to target_type.
// build_structural_sample_routes() fills this when resolving subgraph,
// boundary, or tiled NodeBundle ports requires more than that conversion.
struct SampleChannelTransformPlan {
    std::size_t source_channel_count = 0;
    std::size_t target_channel_count = 0;
    std::vector<float> coefficients{};

    [[nodiscard]] bool empty() const noexcept
    {
        return coefficients.empty();
    }

    auto operator<=>(SampleChannelTransformPlan const&) const = default;
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
    SampleChannelTransformPlan transform{};
};

// One scalar contribution in a route whose configured sample source channels
// do not all have the same detach settings. source_coefficients are aligned
// with SampleConnectionPlan::source_channel_timings. target_coefficients are
// aligned with SampleConnectionPlan::target_channels. A detach cuts every
// dependency used to compute this scalar contribution before it is added to
// the target channels.
struct SampleRoutedContributionPlan {
    std::vector<float> source_coefficients{};
    std::vector<float> target_coefficients{};
    std::optional<ConfiguredSampleConnectionDetach> detach{};
    std::optional<std::vector<std::byte>> detach_initial_value{};
    std::optional<std::size_t> detach_region{};
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
    std::vector<SampleRoutedContributionPlan> routed_contributions{};
    std::optional<NodeBundlePortId> canonical_source_port{};
    std::optional<ChannelLayout> canonical_source_layout{};
    ChannelTypeId target_type = ChannelTypeId::mono;
    ChannelLayout target_layout{};
    SampleChannelTransformPlan transform{};
    // After partial/projected connections are normalized, this covers the
    // whole target port in canonical declaration order. The actual semantic
    // projection/permutation remains in projection_contributions.target_channels.
    std::vector<SampleInputChannelId> target_channels{};
    NodeBundlePortId target_port{};
    ContinuousValueStorageGeometry geometry{};
    std::int32_t absolute_pace_exponent = 0;

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
    std::optional<std::vector<std::byte>> detach_initial_value{};
    std::optional<std::size_t> detach_region{};
};


struct EventSourcePlan {
    EventOutputPortId source{};
    // The concrete output writes source_type. conversion maps that raw event
    // stream to EventConnectionPlan::source_type before fan-in composition.
    EventTypeId source_type = EventTypeId::empty;
    EventConversionPlan conversion{};
    PlannedSourceProduction production = PlannedSourceProduction::tick;
    OutputRetention retention = OutputRetention::ephemeral;
    std::size_t history = 0;
    std::size_t latency = 0;
    double max_events_per_index = 0.0;
    std::optional<ConfiguredEventConnectionDetach> detach{};
    std::optional<std::size_t> detach_region{};
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
    // The target reads its source at position - read_latency: an event its
    // source authored at P reaches it at P + read_latency
    // (sequential_port_storage_planning.md, "Sample and event streams share one
    // lifetime and ownership model"). It is the source port's latency plus the
    // compensation that feed-forward path-latency alignment assigns this path,
    // as SampleSourceChannelTimingPlan::read_latency is for a sample channel. A
    // detach() delay is not part of it; it adds to it.
    std::size_t read_latency = 0;
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
    bool requires_source_composition = false;
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

[[nodiscard]] inline bool has_detached_source(
    SampleConnectionPlan const& connection) noexcept
{
    return connection.detach.has_value() || std::ranges::any_of(
        connection.source_channel_timings,
        [](SampleSourceChannelTimingPlan const& source) {
            return source.detach.has_value();
        }) || std::ranges::any_of(
        connection.routed_contributions,
        [](SampleRoutedContributionPlan const& contribution) {
            return contribution.detach.has_value();
        });
}

[[nodiscard]] inline bool has_detached_source(
    EventConnectionPlan const& connection) noexcept
{
    return connection.detach.has_value() || std::ranges::any_of(
        connection.source_plans,
        [](EventSourcePlan const& source) {
            return source.detach.has_value();
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
    ContinuousValueStorageGeometry geometry{};
    std::int32_t absolute_pace_exponent = 0;
    std::vector<std::size_t> connection_indices{};
    bool has_realtime_connections = false;
    bool has_background_connections = false;
    SampleConnectionStorageRequirements storage_requirements{};
    std::optional<SampleConnectionStoragePlan> storage_plan{};
    ConnectionLiveIntervalPlan live_interval{};
};

struct EventProducerGroupPlan {
    // This is a derived fan-in identity. Concrete callback outputs own their
    // raw storage independently and may participate in several groups.
    EventTypeId source_type = EventTypeId::empty;
    std::vector<EventOutputPortId> sources{};
    std::vector<EventConversionPlan> source_conversions{};
    // One value per entry of sources: the latency compensation that every
    // realtime target of the group's connections needs for that source, its
    // EventDeliveryPlan::read_latency minus the source's latency. The group's
    // stream holds each source's events at the position its readers read
    // them, their own position plus the source latency and this compensation,
    // so connections whose targets need different compensation read
    // different groups.
    std::vector<std::size_t> source_compensations{};
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
    // Parallel to connection_indices: the realtime targets of that connection
    // that read this group, the ones whose compensation for each source is
    // source_compensations. A connection whose targets need different
    // compensation joins one group per distinct compensation.
    std::vector<std::vector<EventInputPortId>> connection_targets{};
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

// A public module input is a source at its inward-facing boundary. Structural
// routing preserves its channel transforms and temporal cuts before lowering
// its contribution alongside ordinary producers at a concrete destination.
struct ManualSampleRoutePlan {
    std::size_t configured_connection_index = 0;
    std::size_t control_index = 0;
    NodeBundlePortId target{};
    std::size_t target_channel = 0;
    float coefficient = 0;
    std::size_t delay = 0;
    std::size_t read_latency = 0;
    std::optional<Sample> initial_value{};
    float initial_coefficient = 0;
    // Ordinary sources in the same detached scalar expression. They retain
    // the cycle proof even when target-projection normalization merges routes.
    std::vector<NodeBundleHandle> detach_sources{};
};

struct ConnectionAnalysisPlan {
    std::vector<ManualSampleRoutePlan> manual_sample_routes{};
    NodeBundleHandle boundary_bundle = 0;
    std::vector<PlannedGraphNode> nodes{};
    std::vector<DependencyEdgePlan> dependencies{};
    SchedulePlan schedule{};
    std::vector<SampleConnectionPlan> sample_connections{};
    std::vector<EventConnectionPlan> event_connections{};
    std::vector<SampleProducerGroupPlan> sample_producer_groups{};
    std::vector<EventProducerGroupPlan> event_producer_groups{};
    ConnectionStoragePlan storage{};
    // Authoritative semantic inventory of every non-empty Sequential-input
    // history and Tick-output history/latency extent. Storage planning may
    // share or elide their physical representations, but must map back to
    // these node-owned state pieces for graph-revision reconciliation.
    RealtimePortStateRequirements realtime_port_states{};
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
    ResolvedPortRealization const& port_realization,
    std::size_t kernel_block_size,
    RealtimeStorageCostModel const& cost_model = {},
    // Stack-pressure retries change only realtime storage policy. Supplying
    // the immutable plan retained from the first analysis avoids repeating
    // semantic SCC/background topology work for the same graph specialization.
    BackgroundEvaluationPlan const* retained_background_plan = nullptr,
    // Optional entry positions are ConfiguredGraph NodeBundleHandle values.
    // Equal identifiers mean equal callback-facing implementation/realization.
    // The empty span preserves the ordinary deterministic bundle order.
    std::span<std::size_t const> callback_batch_classes = {},
    // Concrete bundle handles whose node metadata makes callback elision a
    // candidate. Connection analysis accepts a candidate only when its output
    // has realtime consumers and no background consumer.
    std::span<NodeBundleHandle const> callback_elision_candidates = {});

// Compiler-owned raw regions (producer storage, detach structures) migrate
// across graph revisions by identity, and that identity names what owns the
// region's values. The node part is the node's stable identity, so renumbering
// the graph's bundles keeps it. A node under no virtual node has none: like
// everything else it owns, its storage starts fresh at every edit.
std::optional<std::string> raw_region_node_identity(
    ConnectionAnalysisPlan const& connections,
    NodeBundleHandle bundle);

// raw_region_node_identity(), or a marker standing for a node without one. A
// region is declared with owned_raw_region_identity() of its identity, which is
// empty, so the region never migrates, when the identity contains the marker.
std::string raw_region_node_identity_text(
    ConnectionAnalysisPlan const& connections,
    NodeBundleHandle bundle);
std::string owned_raw_region_identity(std::string identity);

// The parts that can identify one detach() across graph revisions. A detach
// is not a node: it is identified by its producer port when that tells it
// apart from the graph's other detaches, then also by its consumer port, then
// also by its latency and initial value. producer is present only when every
// detached source port has an identity, consumer only when the consumer port
// has one.
struct DetachIdentityParts {
    std::optional<std::string> producer{};
    std::optional<std::string> consumer{};
    std::string settings{};
};

// One identity per detach, in order: the shortest of those keys that no other
// detach of the graph shares, or empty when none tells it apart.
std::vector<std::string> resolve_detach_identities(
    std::span<DetachIdentityParts const> detaches);

// One port in a detach's producer or consumer part, whatever its stream kind:
// its node's identity, its port index, and a sample port's channel. Empty when
// its node has no identity, which leaves the whole part absent.
std::optional<std::string> detach_identity_port(
    ConnectionAnalysisPlan const& connections,
    NodeBundleHandle bundle,
    std::size_t port_index,
    std::optional<std::size_t> channel = std::nullopt);

// A detach's settings part: its latency, then its initial value (empty for a
// detach without one, such as an event detach).
std::string detach_identity_settings(
    std::size_t latency,
    std::optional<std::vector<std::byte>> const& initial_value);

} // namespace iv::graph_jit::detail
