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

enum class IndexedEndpointDirection : std::uint8_t {
    input,
    output,
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

    // Output-only. Input endpoints have no retention contract.
    std::optional<OutputRetention> retention{};
    std::optional<StableIndexedOutputId> stable_identity{};

    ChannelLayout sample_layout{};
    EventTypeId event_type = EventTypeId::empty;
    double max_events_per_index = 0.0;

    // Connection ordinals make fan-in/fan-out convergence explicit. Lists
    // contain each logical indexed connection once, even when a sample
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
    std::vector<IndexedEndpointOrdinal> inputs{};
    std::vector<IndexedEndpointOrdinal> outputs{};
    IndexedNodeAccumulatorPlan accumulators{};
};

struct IndexedSampleProjectionPlan {
    ChannelTypeId source_type = ChannelTypeId::mono;
    std::vector<std::size_t> source_channel_indices{};
    ChannelTypeId target_type = ChannelTypeId::mono;
    std::vector<std::size_t> target_channels{};
};

struct IndexedConnectionPlan {
    std::size_t configured_connection_index = 0;
    PortKind kind = PortKind::sample;
    std::vector<IndexedEndpointOrdinal> source_endpoints{};
    std::vector<IndexedEndpointOrdinal> target_endpoints{};
    std::vector<NodeBundlePortId> source_ports{};
    std::vector<NodeBundlePortId> target_ports{};

    ChannelTypeId sample_source_type = ChannelTypeId::mono;
    ChannelTypeId sample_target_type = ChannelTypeId::mono;
    std::vector<SampleOutputChannelId> sample_source_channels{};
    std::vector<SampleInputChannelId> sample_target_channels{};
    std::vector<IndexedSampleProjectionPlan> sample_projections{};
    EventTypeId event_source_type = EventTypeId::empty;
    EventTypeId event_target_type = EventTypeId::empty;
    EventConversionPlan event_conversion{};
    bool requires_conversion = false;
};

struct IndexedComponentPlan {
    std::vector<IndexedNodeOrdinal> nodes{};
    std::vector<SemanticSccOrdinal> semantic_scc_order{};
    std::vector<IndexedNodeOrdinal> forward_order{};
    std::vector<IndexedNodeOrdinal> reverse_order{};
    std::vector<IndexedNodeOrdinal> tock_order{};
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
    std::vector<IndexedComponentPlan> components{};
    std::vector<std::size_t> component_order{};
    std::vector<IndexedEndpointOrdinal> requestable_outputs{};
    IndexedAccumulatorPlan accumulators{};

    [[nodiscard]] bool empty() const noexcept
    {
        return endpoints.empty();
    }
};

} // namespace iv::graph_jit
