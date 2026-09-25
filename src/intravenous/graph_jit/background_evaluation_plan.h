#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/graph/port_ids.h>
#include <intravenous/ports.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iv::graph_jit {

using SemanticNodeIndex = std::size_t;
using SemanticSccIndex = std::size_t;
using BackgroundNodeIndex = std::size_t;
using BackgroundPortIndex = std::size_t;
using BackgroundConnectionIndex = std::size_t;
using PortSubsetIndex = std::size_t;
using PortStorageIndex = std::size_t;
using SampleMaterializationIndex = std::size_t;
using EventMaterializationIndex = std::size_t;

enum class PlannedSourceProduction : std::uint8_t {
    tick,
    tock,
};

enum class PlannedDestinationAccess : std::uint8_t {
    sequential,
    random_access,
};

enum class PlannedDeliveryMechanism : std::uint8_t {
    // Same-slice ordinary realtime transport. This is the only delivery kind
    // consumed by the sequential storage planner.
    tick_to_sequential,
    // A background-produced page is available before Tick playback.
    tock_to_sequential,
    // Background-produced temporary or retained page materialization.
    tock_to_random_access,
    // Finalized retained Tick data is a stored random-access boundary.
    persisted_tick_to_random_access,
    // A pointwise Tick producer is recomputed in the background after
    // whole-graph contextual replayability has been proven.
    replayed_tick_to_random_access,
};

enum class PortDirection : std::uint8_t {
    input,
    output,
};

enum class BackgroundDependencyKind : std::uint8_t {
    // An authored connection participates directly in background
    // materialization/evaluation.
    materialized_delivery,
    // An ordinary Sequential input is traversed only because its Tick consumer
    // is being replayed in background evaluation.
    replay_sequential,
};

// Correctness requirements are joined per exact source/target port subset
// before any storage is selected. These fields describe storage a subset must
// own or be able to view; they do not imply that every target allocates another
// copy of its source data. They are
// intentionally independent: for example, a Tick/persisted source may require
// both current-Tick visibility and canonical published pages.
struct PortStorageRequirements {
    bool current_tick = false;
    bool capture = false;
    bool persisted_pages = false;
    bool tick_sequential = false;
    bool tick_random_access = false;
    bool background_random_access = false;

    bool operator==(PortStorageRequirements const&) const = default;
};

struct SampleSourcePortSubsetPlan {
    NodeBundlePortId port{};
    ChannelLayout source_layout{};
    PlannedSourceProduction production = PlannedSourceProduction::tick;
    OutputRetention retention = OutputRetention::ephemeral;
    std::vector<SampleOutputChannelId> channels{};
    // Indices in ConnectionAnalysisPlan::sample_connections. They are
    // compile-time incidence facts; configured_connection_indices remain
    // meaningful in the immutable CompiledGraph metadata.
    std::vector<std::size_t> connection_indices{};
    std::vector<std::size_t> configured_connection_indices{};
    PortStorageRequirements storage{};
};

struct SampleTargetPortSubsetPlan {
    NodeBundlePortId port{};
    ChannelLayout target_layout{};
    PlannedDestinationAccess access = PlannedDestinationAccess::sequential;
    std::vector<SampleInputChannelId> channels{};
    std::vector<std::size_t> connection_indices{};
    std::vector<std::size_t> configured_connection_indices{};
    std::vector<PortSubsetIndex> source_subsets{};
    PortStorageRequirements storage{};
};

struct EventSourcePortSubsetPlan {
    EventOutputPortId port{};
    EventTypeId type = EventTypeId::empty;
    PlannedSourceProduction production = PlannedSourceProduction::tick;
    OutputRetention retention = OutputRetention::ephemeral;
    double max_events_per_index = 0.0;
    std::vector<std::size_t> connection_indices{};
    std::vector<std::size_t> configured_connection_indices{};
    PortStorageRequirements storage{};
};

struct EventTargetPortSubsetPlan {
    EventInputPortId port{};
    EventTypeId type = EventTypeId::empty;
    PlannedDestinationAccess access = PlannedDestinationAccess::sequential;
    std::vector<std::size_t> connection_indices{};
    std::vector<std::size_t> configured_connection_indices{};
    std::vector<PortSubsetIndex> source_subsets{};
    PortStorageRequirements storage{};
};

// A persistent identity exists only when the configured concrete node belongs
// to a stable virtual node. Anonymous concrete nodes still receive dense
// generation-local port indices, but are deliberately not assigned a
// misleading persistent identity derived from a bundle handle.
struct StableConcreteNodeId {
    std::string graph{};
    std::string virtual_node{};
    std::size_t direct_member = 0;

    bool operator==(StableConcreteNodeId const&) const = default;
};

struct StableOutputPortId {
    StableConcreteNodeId node{};
    PortKind kind = PortKind::sample;
    std::string port_name{};
    std::size_t port_index = 0;

    bool operator==(StableOutputPortId const&) const = default;
};

struct SemanticNodePlan {
    NodeBundleHandle bundle = 0;
    SemanticSccIndex scc = 0;
    std::optional<BackgroundNodeIndex> background_node{};
};

struct SemanticSccPlan {
    // Semantic node indices, not configured bundle handles.
    std::vector<SemanticNodeIndex> nodes{};
    std::vector<SemanticSccIndex> incoming{};
    std::vector<SemanticSccIndex> outgoing{};
    bool cyclic = false;
};

struct PortAccumulatorIndices {
    std::optional<std::size_t> input_change{};
    // Indexes one output accumulator record containing both exact changed
    // coverage and the output's newly published exact coverage.
    std::optional<std::size_t> output_change{};
    std::optional<std::size_t> output_requirement{};
    std::optional<std::size_t> input_requirement{};
};

struct NodeAccumulatorRanges {
    std::size_t input_change_begin = 0;
    std::size_t input_change_count = 0;
    std::size_t output_change_begin = 0;
    std::size_t output_change_count = 0;
    std::size_t output_requirement_begin = 0;
    std::size_t output_requirement_count = 0;
    std::size_t input_requirement_begin = 0;
    std::size_t input_requirement_count = 0;
};

struct BackgroundPortPlan {
    BackgroundNodeIndex node = 0;
    NodeBundlePortId configured_port{};
    PortKind kind = PortKind::sample;
    PortDirection direction = PortDirection::input;
    std::string name{};

    // Input roles are independent because a Sequential input can require
    // Tock data made available before Tick playback and also participate in a
    // replay of its Tick consumer. Random-access inputs always
    // participate in background coverage propagation.
    bool random_access_input = false;
    bool tick_sequential_input = false;
    bool replay_sequential_input = false;

    // Output roles distinguish authored background production, retained Tick
    // boundaries, and replay. They are planning facts, not authored
    // port modes.
    bool authored_tock_output = false;
    bool persisted_tick_output = false;
    bool replayed_tick_output = false;

    // Sample-input neutral values are retained for later missing-page playback
    // lowering. Event inputs use absence-of-events as their neutral value.
    Sample sample_neutral_value{};

    // Output-only. Input ports have no retention contract.
    std::optional<OutputRetention> retention{};
    std::optional<StableOutputPortId> stable_identity{};

    ChannelLayout sample_layout{};
    EventTypeId event_type = EventTypeId::empty;
    double max_events_per_index = 0.0;

    // Connection indices make fan-in/fan-out convergence explicit. Lists
    // contain each logical background connection once, even when a sample
    // connection contributes several channels from the same output port.
    std::vector<BackgroundConnectionIndex> incoming_connections{};
    std::vector<BackgroundConnectionIndex> outgoing_connections{};
    PortAccumulatorIndices accumulators{};
};

struct BackgroundNodePlan {
    NodeBundleHandle bundle = 0;
    SemanticNodeIndex semantic_node = 0;
    SemanticSccIndex semantic_scc = 0;
    std::optional<StableConcreteNodeId> stable_identity{};

    // Authored Tock nodes execute their imported F/R/T callbacks. Replay nodes
    // instead use compiler-provided pointwise F/R coverage propagation and
    // the already-imported generated tick_block() wrapper. A retained Tick-only
    // boundary may have neither execution mode.
    bool authored_tock_execution = false;
    bool replays_tick = false;
    bool uses_replay_forward_coverage = false;
    bool uses_replay_reverse_coverage = false;
    bool uses_imported_tick_block_for_replay = false;

    std::vector<BackgroundPortIndex> inputs{};
    std::vector<BackgroundPortIndex> outputs{};
    NodeAccumulatorRanges accumulators{};
};

struct SampleProjectionPlan {
    ChannelTypeId source_type = ChannelTypeId::mono;
    std::vector<std::size_t> source_channel_indices{};
    ChannelTypeId target_type = ChannelTypeId::mono;
    std::vector<std::size_t> target_channels{};

    bool operator==(SampleProjectionPlan const&) const = default;
};

struct EventDeliveryPlan {
    EventOutputPortId source{};
    EventInputPortId target{};
    std::optional<BackgroundPortIndex> source_port{};
    std::optional<BackgroundPortIndex> target_port{};
    PlannedDeliveryMechanism mechanism =
        PlannedDeliveryMechanism::tick_to_sequential;
};

struct BackgroundConnectionPlan {
    std::size_t configured_connection_index = 0;
    PortKind kind = PortKind::sample;
    std::vector<BackgroundPortIndex> source_coverage_ports{};
    std::vector<BackgroundPortIndex> target_coverage_ports{};
    std::vector<NodeBundlePortId> source_ports{};
    std::vector<NodeBundlePortId> target_ports{};
    // Indices in the data-specific source/target subset vectors retained by
    // BackgroundEvaluationPlan. Coverage remains port-granular.
    std::vector<PortSubsetIndex> source_subsets{};
    std::vector<PortSubsetIndex> target_subsets{};

    ChannelTypeId sample_source_type = ChannelTypeId::mono;
    ChannelTypeId sample_target_type = ChannelTypeId::mono;
    std::vector<SampleOutputChannelId> sample_source_channels{};
    // Aligned one-for-one with sample_source_channels. Tick -> Sequential
    // entries have no background port; all background/stored entries do.
    std::vector<std::optional<BackgroundPortIndex>>
        sample_source_port_by_channel{};
    std::vector<PlannedDeliveryMechanism> sample_deliveries{};
    std::vector<SampleInputChannelId> sample_target_channels{};
    std::optional<BackgroundPortIndex> sample_target_port{};
    std::vector<SampleProjectionPlan> sample_projections{};
    EventTypeId event_source_type = EventTypeId::empty;
    EventTypeId event_target_type = EventTypeId::empty;
    EventConversionPlan event_conversion{};
    std::vector<EventDeliveryPlan> event_deliveries{};
    bool requires_conversion = false;
};

// These are immutable port-storage decisions. The
// executor supplies their requested range, selected page version and storage;
// those transaction-specific values never enter the compiled plan.
enum class PortStorageKind : std::uint8_t {
    current_tick,
    persisted_pages,
    tick_sequential,
    tick_random_access,
    background,
};

struct PortStoragePlan {
    PortKind kind = PortKind::sample;
    PortStorageKind storage = PortStorageKind::background;

    // Subset indices use the data/direction-specific vectors in
    // BackgroundEvaluationPlan. One allocation may serve several equivalent
    // source or target subsets.
    std::vector<PortSubsetIndex> source_subsets{};
    std::vector<PortSubsetIndex> target_subsets{};
    std::vector<BackgroundConnectionIndex> connections{};
    std::vector<PortStorageIndex> inputs{};

    // Source storage retains its authored port. Canonical persisted
    // pages also bind the logical output port carrying stable identity.
    std::optional<NodeBundlePortId> source_port{};
    std::optional<BackgroundPortIndex> output_port{};

    ChannelLayout sample_layout{};
    std::vector<std::size_t> sample_channels{};
    EventTypeId event_type = EventTypeId::empty;
    double max_events_per_index = 0.0;

    std::optional<SampleMaterializationIndex> sample_materialization{};
    std::optional<EventMaterializationIndex> event_materialization{};
};

// One derived sample operation template. Range and selected input-page version
// are runtime keys; everything below is the compile-time portion of the sharing
// key. Several consumers may reference one record only when these facts match.
// Tick-time random-access storage may also serve an otherwise-identical
// sequential use because the former capability subsumes the latter.
struct SampleMaterializationPlan {
    PortStorageKind storage = PortStorageKind::background;
    std::vector<PortSubsetIndex> source_subsets{};
    std::vector<PortStorageIndex> inputs{};
    std::vector<SampleOutputChannelId> source_channels{};
    std::vector<std::size_t> source_read_latencies{};
    ChannelTypeId source_type = ChannelTypeId::mono;
    ChannelLayout target_layout{};
    std::vector<std::size_t> target_channels{};
    // Only projection contributions belonging to this exact target subset are
    // retained. Their source_channel_indices index this materialization's
    // source_channels/inputs, not the parent connection's
    // flattened source-channel table.
    std::vector<SampleProjectionPlan> projections{};
    std::size_t target_history = 0;
    PortStorageIndex output = 0;
    std::vector<PortSubsetIndex> target_subsets{};
    std::vector<BackgroundConnectionIndex> connections{};
};

// Event conversion and fan-in are one ordered materialization. Source subsets
// and inputs retain semantic source order so equal-time ordering is not lost
// when otherwise-equivalent consumers share the result.
struct EventMaterializationPlan {
    PortStorageKind storage = PortStorageKind::background;
    std::vector<PortSubsetIndex> source_subsets{};
    std::vector<PortStorageIndex> inputs{};
    EventTypeId source_type = EventTypeId::empty;
    EventTypeId target_type = EventTypeId::empty;
    EventConversionPlan conversion{};
    std::size_t target_history = 0;
    PortStorageIndex output = 0;
    std::vector<PortSubsetIndex> target_subsets{};
    std::vector<BackgroundConnectionIndex> connections{};
};

struct DirectSampleConnectionPlan {
    BackgroundConnectionIndex connection = 0;
    PortSubsetIndex target_subset = 0;
    std::size_t target_channel = 0;
    PortSubsetIndex source_subset = 0;
    std::size_t source_channel = 0;
    PortStorageIndex storage = 0;
    PlannedDeliveryMechanism delivery =
        PlannedDeliveryMechanism::tick_to_sequential;
    std::size_t read_latency = 0;
    std::size_t target_history = 0;
};

struct DirectEventConnectionPlan {
    BackgroundConnectionIndex connection = 0;
    PortSubsetIndex target_subset = 0;
    PortSubsetIndex source_subset = 0;
    PortStorageIndex storage = 0;
    PlannedDeliveryMechanism delivery =
        PlannedDeliveryMechanism::tick_to_sequential;
    std::size_t target_history = 0;
};

struct ConnectionStoragePlan {
    std::vector<std::size_t> direct_samples{};
    std::vector<std::size_t> direct_events{};
    std::vector<SampleMaterializationIndex> sample_materializations{};
    std::vector<EventMaterializationIndex> event_materializations{};
};

struct BackgroundStoragePlan {
    std::vector<PortStoragePlan> ports{};
    // Aligned with the four port-subset vectors in BackgroundEvaluationPlan.
    std::vector<std::vector<PortStorageIndex>>
        sample_source_storage{};
    std::vector<std::vector<PortStorageIndex>>
        sample_target_storage{};
    std::vector<std::vector<PortStorageIndex>>
        event_source_storage{};
    std::vector<std::vector<PortStorageIndex>>
        event_target_storage{};
    std::vector<SampleMaterializationPlan> sample_materializations{};
    std::vector<EventMaterializationPlan> event_materializations{};
    std::vector<DirectSampleConnectionPlan> direct_samples{};
    std::vector<DirectEventConnectionPlan> direct_events{};
    // Aligned with BackgroundEvaluationPlan::connections.
    std::vector<ConnectionStoragePlan> connections{};
};

struct BackgroundDependencyPlan {
    BackgroundNodeIndex source_node = 0;
    BackgroundNodeIndex target_node = 0;
    NodeBundlePortId source_port{};
    NodeBundlePortId target_port{};
    BackgroundDependencyKind kind =
        BackgroundDependencyKind::materialized_delivery;
    PlannedDeliveryMechanism delivery =
        PlannedDeliveryMechanism::tick_to_sequential;
    // Persisted Tick is a terminal stored boundary: ordering may depend on the
    // selected stored version, but background evaluation never traverses into
    // the live producer.
    bool source_is_stored_boundary = false;
};

struct BackgroundComponentPlan {
    std::vector<BackgroundNodeIndex> nodes{};
    std::vector<SemanticSccIndex> semantic_scc_order{};
    std::vector<BackgroundNodeIndex> forward_order{};
    std::vector<BackgroundNodeIndex> reverse_order{};
    std::vector<BackgroundNodeIndex> tock_order{};
    std::vector<BackgroundNodeIndex> replay_order{};
    std::vector<BackgroundNodeIndex> background_evaluation_order{};
    std::vector<BackgroundConnectionIndex> connections{};
};

struct CoverageAccumulatorCounts {
    std::size_t input_change_count = 0;
    std::size_t output_change_count = 0;
    std::size_t output_requirement_count = 0;
    std::size_t input_requirement_count = 0;
};

// Immutable topology and fixed-layout metadata retained by CompiledGraph. No
// mutable coverage, data, validity, or transaction state belongs here.
struct BackgroundEvaluationPlan {
    std::vector<SemanticNodePlan> semantic_nodes{};
    std::vector<SemanticSccPlan> semantic_sccs{};
    std::vector<SemanticSccIndex> semantic_condensation_order{};
    std::vector<std::optional<SemanticNodeIndex>> bundle_to_semantic_node{};

    std::vector<BackgroundNodePlan> nodes{};
    std::vector<std::optional<BackgroundNodeIndex>> bundle_to_background_node{};
    std::vector<BackgroundPortPlan> ports{};
    std::vector<BackgroundConnectionPlan> connections{};
    // Logical callback/coverage ports above remain whole ports. These
    // partitions are the finer correctness unit used by storage planning.
    // Storage coalescing may combine subsets only after their full requirements
    // have been derived.
    std::vector<SampleSourcePortSubsetPlan> sample_source_subsets{};
    std::vector<SampleTargetPortSubsetPlan> sample_target_subsets{};
    std::vector<EventSourcePortSubsetPlan> event_source_subsets{};
    std::vector<EventTargetPortSubsetPlan> event_target_subsets{};
    BackgroundStoragePlan storage{};
    std::vector<BackgroundComponentPlan> components{};
    std::vector<std::size_t> component_order{};
    // Intrinsic replayability is an authored candidate fact. A candidate enters
    // replay only when whole-graph contextual proof reaches it.
    // Keeping candidate bundles separate does not make an otherwise sequential
    // graph an active background-evaluation plan.
    std::vector<NodeBundleHandle> intrinsic_replay_candidates{};
    std::vector<BackgroundPortIndex> requestable_outputs{};
    std::vector<BackgroundPortIndex> tick_sequential_inputs{};
    std::vector<BackgroundDependencyPlan> background_dependencies{};
    std::vector<BackgroundNodeIndex> background_evaluation_order{};
    CoverageAccumulatorCounts accumulators{};

    [[nodiscard]] bool empty() const noexcept
    {
        // Sequential-only graphs still retain port-subset storage facts;
        // they do not require generated background roots.
        return ports.empty();
    }
};

} // namespace iv::graph_jit
