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

using SemanticNodeOrdinal = std::size_t;
using SemanticSccOrdinal = std::size_t;
using IndexedNodeOrdinal = std::size_t;
using IndexedEndpointOrdinal = std::size_t;
using IndexedConnectionOrdinal = std::size_t;
using EndpointAtomOrdinal = std::size_t;
using IndexedRepresentationOrdinal = std::size_t;
using IndexedSampleMaterializationOrdinal = std::size_t;
using IndexedEventMaterializationOrdinal = std::size_t;

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
    // consumed by the realtime physical-storage planner.
    tick_to_sequential,
    // A background-produced page is prepared before realtime playback.
    tock_to_sequential,
    // Background-produced temporary or retained page materialization.
    tock_to_random_access,
    // Finalized retained Tick data is a stored random-access boundary.
    persisted_tick_to_random_access,
    // A pointwise Tick producer is recomputed in the background after
    // whole-graph contextual replayability has been proven.
    replayed_tick_to_random_access,
};

enum class IndexedEndpointDirection : std::uint8_t {
    input,
    output,
};

enum class IndexedBackgroundDependencyKind : std::uint8_t {
    // A non-realtime authored connection participates directly in background
    // materialization/evaluation.
    materialized_delivery,
    // An ordinary Sequential input is traversed only because its Tick consumer
    // is being synthesized as a replay evaluation.
    replay_sequential,
};

// Correctness requirements are joined per exact source/target incidence atom
// before any physical representation is selected. These capabilities describe
// representations an atom must own or be able to view; they do not imply that
// every target allocates another copy of its source representation. They are
// intentionally independent: for example, a Tick/persisted source may require
// both current-Tick visibility and canonical published pages.
struct EndpointStorageCapabilities {
    bool current_tick_readable = false;
    bool capture_backed_persistence = false;
    bool canonical_persisted_pages = false;
    bool prepared_sequential_window = false;
    bool prepared_addressable_window = false;
    bool transaction_local_addressable = false;

    bool operator==(EndpointStorageCapabilities const&) const = default;
};

struct SampleSourceEndpointAtomPlan {
    NodeBundlePortId port{};
    ChannelLayout source_layout{};
    PlannedSourceProduction production = PlannedSourceProduction::tick;
    OutputRetention retention = OutputRetention::ephemeral;
    std::vector<SampleOutputChannelId> channels{};
    // Ordinals in ConnectionAnalysisPlan::sample_connections. They are
    // compile-time incidence facts; configured_connection_indices remain
    // meaningful in the immutable CompiledGraph metadata.
    std::vector<std::size_t> connection_indices{};
    std::vector<std::size_t> configured_connection_indices{};
    EndpointStorageCapabilities capabilities{};
};

struct SampleTargetEndpointAtomPlan {
    NodeBundlePortId port{};
    ChannelLayout target_layout{};
    PlannedDestinationAccess access = PlannedDestinationAccess::sequential;
    std::vector<SampleInputChannelId> channels{};
    std::vector<std::size_t> connection_indices{};
    std::vector<std::size_t> configured_connection_indices{};
    std::vector<EndpointAtomOrdinal> source_atoms{};
    EndpointStorageCapabilities capabilities{};
};

struct EventSourceEndpointAtomPlan {
    EventOutputPortId port{};
    EventTypeId type = EventTypeId::empty;
    PlannedSourceProduction production = PlannedSourceProduction::tick;
    OutputRetention retention = OutputRetention::ephemeral;
    double max_events_per_index = 0.0;
    std::vector<std::size_t> connection_indices{};
    std::vector<std::size_t> configured_connection_indices{};
    EndpointStorageCapabilities capabilities{};
};

struct EventTargetEndpointAtomPlan {
    EventInputPortId port{};
    EventTypeId type = EventTypeId::empty;
    PlannedDestinationAccess access = PlannedDestinationAccess::sequential;
    std::vector<std::size_t> connection_indices{};
    std::vector<std::size_t> configured_connection_indices{};
    std::vector<EndpointAtomOrdinal> source_atoms{};
    EndpointStorageCapabilities capabilities{};
};

// A persistent identity exists only when the configured concrete node belongs
// to a stable virtual node. Anonymous concrete nodes still receive dense
// generation-local endpoint ordinals, but are deliberately not assigned a
// misleading persistent identity derived from a bundle handle.
struct StableConcreteNodeId {
    std::string graph{};
    std::string virtual_node{};
    std::size_t direct_member = 0;

    bool operator==(StableConcreteNodeId const&) const = default;
};

struct StableIndexedOutputId {
    StableConcreteNodeId node{};
    PortKind kind = PortKind::sample;
    std::string port_name{};
    std::size_t port_ordinal = 0;

    bool operator==(StableIndexedOutputId const&) const = default;
};

struct SemanticNodePlan {
    NodeBundleHandle bundle = 0;
    SemanticSccOrdinal scc = 0;
    std::optional<IndexedNodeOrdinal> indexed_node{};
};

struct SemanticSccPlan {
    // Semantic node ordinals, not configured bundle handles.
    std::vector<SemanticNodeOrdinal> nodes{};
    std::vector<SemanticSccOrdinal> incoming{};
    std::vector<SemanticSccOrdinal> outgoing{};
    bool cyclic = false;
};

struct IndexedAccumulatorSlotPlan {
    std::optional<std::size_t> input_change{};
    // Indexes one output accumulator record containing both exact changed
    // coverage and the output's newly published exact coverage.
    std::optional<std::size_t> output_change{};
    std::optional<std::size_t> output_requirement{};
    std::optional<std::size_t> input_requirement{};
};

struct IndexedNodeAccumulatorPlan {
    std::size_t input_change_begin = 0;
    std::size_t input_change_count = 0;
    std::size_t output_change_begin = 0;
    std::size_t output_change_count = 0;
    std::size_t output_requirement_begin = 0;
    std::size_t output_requirement_count = 0;
    std::size_t input_requirement_begin = 0;
    std::size_t input_requirement_count = 0;
};

struct IndexedEndpointPlan {
    IndexedNodeOrdinal node = 0;
    NodeBundlePortId configured_port{};
    PortKind kind = PortKind::sample;
    IndexedEndpointDirection direction = IndexedEndpointDirection::input;
    std::string name{};

    // Input roles are independent because a Sequential input can require
    // advance-prepared Tock data for live playback and also participate in a
    // synthesized replay of its Tick consumer. Random-access inputs always
    // participate in indexed coverage propagation.
    bool random_access_input = false;
    bool prepared_sequential_input = false;
    bool replay_sequential_input = false;

    // Output roles distinguish authored background production, retained Tick
    // boundaries, and synthesized replay. They are planning facts, not authored
    // port modes.
    bool authored_tock_output = false;
    bool persisted_tick_output = false;
    bool replayed_tick_output = false;

    // Sample-input neutral values are retained for later missing-page playback
    // lowering. Event inputs use absence-of-events as their neutral value.
    Sample sample_neutral_value{};

    // Output-only. Input endpoints have no retention contract.
    std::optional<OutputRetention> retention{};
    std::optional<StableIndexedOutputId> stable_identity{};

    ChannelLayout sample_layout{};
    EventTypeId event_type = EventTypeId::empty;
    double max_events_per_index = 0.0;

    // Connection ordinals make fan-in/fan-out convergence explicit. Lists
    // contain each logical background/indexed connection once, even when a sample
    // connection contributes several channels from the same output port.
    std::vector<IndexedConnectionOrdinal> incoming_connections{};
    std::vector<IndexedConnectionOrdinal> outgoing_connections{};
    IndexedAccumulatorSlotPlan accumulators{};
};

struct IndexedNodePlan {
    NodeBundleHandle bundle = 0;
    SemanticNodeOrdinal semantic_node = 0;
    SemanticSccOrdinal semantic_scc = 0;
    std::optional<StableConcreteNodeId> stable_identity{};

    // Authored Tock nodes execute their imported F/R/T callbacks. Replay nodes
    // instead use compiler-synthesized pointwise F/R coverage propagation and
    // the already-imported generated tick_block() wrapper. A retained Tick-only
    // boundary may have neither execution mode.
    bool authored_tock_execution = false;
    bool synthesized_tick_replay = false;
    bool synthesized_forward_coverage = false;
    bool synthesized_reverse_coverage = false;
    bool uses_imported_tick_block_for_replay = false;

    std::vector<IndexedEndpointOrdinal> inputs{};
    std::vector<IndexedEndpointOrdinal> outputs{};
    IndexedNodeAccumulatorPlan accumulators{};
};

struct IndexedSampleProjectionPlan {
    ChannelTypeId source_type = ChannelTypeId::mono;
    std::vector<std::size_t> source_channel_indices{};
    ChannelTypeId target_type = ChannelTypeId::mono;
    std::vector<std::size_t> target_channels{};

    bool operator==(IndexedSampleProjectionPlan const&) const = default;
};

struct IndexedEventDeliveryPlan {
    EventOutputPortId source{};
    EventInputPortId target{};
    std::optional<IndexedEndpointOrdinal> source_endpoint{};
    std::optional<IndexedEndpointOrdinal> target_endpoint{};
    PlannedDeliveryMechanism mechanism =
        PlannedDeliveryMechanism::tick_to_sequential;
};

struct IndexedConnectionPlan {
    std::size_t configured_connection_index = 0;
    PortKind kind = PortKind::sample;
    std::vector<IndexedEndpointOrdinal> source_endpoints{};
    std::vector<IndexedEndpointOrdinal> target_endpoints{};
    std::vector<NodeBundlePortId> source_ports{};
    std::vector<NodeBundlePortId> target_ports{};
    // Ordinals in the payload-specific source/target atom vectors retained by
    // IndexedPlan. Port-level endpoints remain the callback/coverage identity.
    std::vector<EndpointAtomOrdinal> source_atoms{};
    std::vector<EndpointAtomOrdinal> target_atoms{};

    ChannelTypeId sample_source_type = ChannelTypeId::mono;
    ChannelTypeId sample_target_type = ChannelTypeId::mono;
    std::vector<SampleOutputChannelId> sample_source_channels{};
    // Aligned one-for-one with sample_source_channels. Tick -> Sequential
    // entries have no indexed endpoint; all background/stored entries do.
    std::vector<std::optional<IndexedEndpointOrdinal>>
        sample_source_endpoint_by_channel{};
    std::vector<PlannedDeliveryMechanism> sample_deliveries{};
    std::vector<SampleInputChannelId> sample_target_channels{};
    std::optional<IndexedEndpointOrdinal> sample_target_endpoint{};
    std::vector<IndexedSampleProjectionPlan> sample_projections{};
    EventTypeId event_source_type = EventTypeId::empty;
    EventTypeId event_target_type = EventTypeId::empty;
    EventConversionPlan event_conversion{};
    std::vector<IndexedEventDeliveryPlan> event_deliveries{};
    bool requires_conversion = false;
};

// Physical indexed representations are immutable binding decisions. The
// executor supplies their requested range, selected page version and storage;
// those transaction-specific values never enter the compiled plan.
enum class IndexedRepresentationResidence : std::uint8_t {
    // Source-origin records are non-owning views of the ordinary Tick physical
    // plan. A derived record names a current-callback materialization.
    current_tick,
    canonical_persisted_pages,
    prepared_sequential_window,
    prepared_addressable_window,
    transaction_local_addressable,
};

enum class IndexedRepresentationOrigin : std::uint8_t {
    source,
    derived,
};

struct IndexedRepresentationPlan {
    PortKind kind = PortKind::sample;
    IndexedRepresentationResidence residence =
        IndexedRepresentationResidence::transaction_local_addressable;
    IndexedRepresentationOrigin origin = IndexedRepresentationOrigin::source;

    // Atom ordinals use the payload/direction-specific vectors in IndexedPlan.
    // Source representations may physically coalesce several source atoms;
    // derived representations additionally name every target atom they serve.
    std::vector<EndpointAtomOrdinal> source_atoms{};
    std::vector<EndpointAtomOrdinal> target_atoms{};
    std::vector<IndexedConnectionOrdinal> connections{};
    std::vector<IndexedRepresentationOrdinal> input_representations{};

    // Source representations retain their authored port. Canonical persisted
    // pages also bind the logical output endpoint carrying stable identity.
    std::optional<NodeBundlePortId> source_port{};
    std::optional<IndexedEndpointOrdinal> output_endpoint{};

    ChannelLayout sample_layout{};
    std::vector<std::size_t> sample_channels{};
    EventTypeId event_type = EventTypeId::empty;
    double max_events_per_index = 0.0;

    std::optional<IndexedSampleMaterializationOrdinal> sample_materialization{};
    std::optional<IndexedEventMaterializationOrdinal> event_materialization{};
};

// One derived sample operation template. Range and selected input-page version
// are runtime keys; everything below is the compile-time portion of the sharing
// key. Several consumers may reference one record only when these facts match.
// A prepared-addressable record may also serve an otherwise-identical prepared
// sequential use because the former capability subsumes the latter.
struct IndexedSampleMaterializationPlan {
    IndexedRepresentationResidence residence =
        IndexedRepresentationResidence::transaction_local_addressable;
    std::vector<EndpointAtomOrdinal> source_atoms{};
    std::vector<IndexedRepresentationOrdinal> input_representations{};
    std::vector<SampleOutputChannelId> source_channels{};
    std::vector<std::size_t> source_read_latencies{};
    ChannelTypeId source_type = ChannelTypeId::mono;
    ChannelLayout target_layout{};
    std::vector<std::size_t> target_channels{};
    // Only projection contributions belonging to this exact target atom are
    // retained. Their source_channel_indices index this materialization's
    // source_channels/input_representations, not the parent connection's
    // flattened source-channel table.
    std::vector<IndexedSampleProjectionPlan> projections{};
    std::size_t target_history = 0;
    IndexedRepresentationOrdinal output_representation = 0;
    std::vector<EndpointAtomOrdinal> target_atoms{};
    std::vector<IndexedConnectionOrdinal> connections{};
};

// Event conversion and fan-in are one ordered materialization. source_atoms and
// input_representations retain semantic source order so equal-time ordering is
// not lost when otherwise-equivalent consumers share the result. Prepared
// addressable residence likewise subsumes an identical sequential-only result.
struct IndexedEventMaterializationPlan {
    IndexedRepresentationResidence residence =
        IndexedRepresentationResidence::transaction_local_addressable;
    std::vector<EndpointAtomOrdinal> source_atoms{};
    std::vector<IndexedRepresentationOrdinal> input_representations{};
    EventTypeId source_type = EventTypeId::empty;
    EventTypeId target_type = EventTypeId::empty;
    EventConversionPlan conversion{};
    std::size_t target_history = 0;
    IndexedRepresentationOrdinal output_representation = 0;
    std::vector<EndpointAtomOrdinal> target_atoms{};
    std::vector<IndexedConnectionOrdinal> connections{};
};

struct IndexedSampleDirectBindingPlan {
    IndexedConnectionOrdinal connection = 0;
    EndpointAtomOrdinal target_atom = 0;
    std::size_t target_channel = 0;
    EndpointAtomOrdinal source_atom = 0;
    std::size_t source_channel = 0;
    IndexedRepresentationOrdinal representation = 0;
    PlannedDeliveryMechanism delivery =
        PlannedDeliveryMechanism::tick_to_sequential;
    std::size_t read_latency = 0;
    std::size_t target_history = 0;
};

struct IndexedEventDirectBindingPlan {
    IndexedConnectionOrdinal connection = 0;
    EndpointAtomOrdinal target_atom = 0;
    EndpointAtomOrdinal source_atom = 0;
    IndexedRepresentationOrdinal representation = 0;
    PlannedDeliveryMechanism delivery =
        PlannedDeliveryMechanism::tick_to_sequential;
    std::size_t target_history = 0;
};

struct IndexedConnectionPhysicalPlan {
    std::vector<std::size_t> sample_direct_bindings{};
    std::vector<std::size_t> event_direct_bindings{};
    std::vector<IndexedSampleMaterializationOrdinal> sample_materializations{};
    std::vector<IndexedEventMaterializationOrdinal> event_materializations{};
};

struct IndexedPhysicalPlan {
    std::vector<IndexedRepresentationPlan> representations{};
    // Aligned with the four endpoint-atom vectors in IndexedPlan.
    std::vector<std::vector<IndexedRepresentationOrdinal>>
        sample_source_representations{};
    std::vector<std::vector<IndexedRepresentationOrdinal>>
        sample_target_representations{};
    std::vector<std::vector<IndexedRepresentationOrdinal>>
        event_source_representations{};
    std::vector<std::vector<IndexedRepresentationOrdinal>>
        event_target_representations{};
    std::vector<IndexedSampleMaterializationPlan> sample_materializations{};
    std::vector<IndexedEventMaterializationPlan> event_materializations{};
    std::vector<IndexedSampleDirectBindingPlan> sample_direct_bindings{};
    std::vector<IndexedEventDirectBindingPlan> event_direct_bindings{};
    // Aligned with IndexedPlan::connections.
    std::vector<IndexedConnectionPhysicalPlan> connections{};
};

struct IndexedBackgroundDependencyPlan {
    IndexedNodeOrdinal source_node = 0;
    IndexedNodeOrdinal target_node = 0;
    NodeBundlePortId source_port{};
    NodeBundlePortId target_port{};
    IndexedBackgroundDependencyKind kind =
        IndexedBackgroundDependencyKind::materialized_delivery;
    PlannedDeliveryMechanism delivery =
        PlannedDeliveryMechanism::tick_to_sequential;
    // Persisted Tick is a terminal stored boundary: ordering may depend on the
    // selected stored version, but background evaluation never traverses into
    // the live producer.
    bool source_is_stored_boundary = false;
};

struct IndexedComponentPlan {
    std::vector<IndexedNodeOrdinal> nodes{};
    std::vector<SemanticSccOrdinal> semantic_scc_order{};
    std::vector<IndexedNodeOrdinal> forward_order{};
    std::vector<IndexedNodeOrdinal> reverse_order{};
    std::vector<IndexedNodeOrdinal> tock_order{};
    std::vector<IndexedNodeOrdinal> replay_order{};
    std::vector<IndexedNodeOrdinal> background_evaluation_order{};
    std::vector<IndexedConnectionOrdinal> connections{};
};

struct IndexedAccumulatorPlan {
    std::size_t input_change_count = 0;
    std::size_t output_change_count = 0;
    std::size_t output_requirement_count = 0;
    std::size_t input_requirement_count = 0;
};

// Immutable topology and fixed-layout metadata retained by CompiledGraph. No
// mutable coverage, payload, validity, or transaction state belongs here.
struct IndexedPlan {
    std::vector<SemanticNodePlan> semantic_nodes{};
    std::vector<SemanticSccPlan> semantic_sccs{};
    std::vector<SemanticSccOrdinal> semantic_condensation_order{};
    std::vector<std::optional<SemanticNodeOrdinal>> bundle_to_semantic_node{};

    std::vector<IndexedNodePlan> nodes{};
    std::vector<std::optional<IndexedNodeOrdinal>> bundle_to_indexed_node{};
    std::vector<IndexedEndpointPlan> endpoints{};
    std::vector<IndexedConnectionPlan> connections{};
    // Logical callback/coverage endpoints above remain whole ports. These
    // partitions are the finer correctness unit used by storage planning.
    // Physical coalescing may combine atoms only after their full requirements
    // have been derived.
    std::vector<SampleSourceEndpointAtomPlan> sample_source_atoms{};
    std::vector<SampleTargetEndpointAtomPlan> sample_target_atoms{};
    std::vector<EventSourceEndpointAtomPlan> event_source_atoms{};
    std::vector<EventTargetEndpointAtomPlan> event_target_atoms{};
    IndexedPhysicalPlan physical{};
    std::vector<IndexedComponentPlan> components{};
    std::vector<std::size_t> component_order{};
    // Intrinsic replayability is an authored candidate fact. A candidate enters
    // synthesized replay only when whole-graph contextual proof reaches it.
    // Keeping candidate bundles separate does not make an otherwise realtime
    // graph an active indexed/background plan.
    std::vector<NodeBundleHandle> intrinsic_replay_candidates{};
    std::vector<IndexedEndpointOrdinal> requestable_outputs{};
    std::vector<IndexedEndpointOrdinal> prepared_sequential_inputs{};
    std::vector<IndexedBackgroundDependencyPlan> background_dependencies{};
    std::vector<IndexedNodeOrdinal> background_evaluation_order{};
    IndexedAccumulatorPlan accumulators{};

    [[nodiscard]] bool empty() const noexcept
    {
        // Sequential-only graphs still retain endpoint-atom storage facts;
        // they do not require generated background roots.
        return endpoints.empty();
    }
};

} // namespace iv::graph_jit
