#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/graph/scc_wrapper.h>
#include <intravenous/graph/types.h>
#include <intravenous/graph/wiring.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <flat_map>
#include <flat_set>
#include <memory>
#include <vector>

namespace iv {
    enum class VirtualPortConnectivity : std::uint8_t {
        disconnected,
        connected,
        mixed,
    };

    struct IntrospectionPortInfo {
        std::string name {};
        std::string type {};
        VirtualPortConnectivity connectivity = VirtualPortConnectivity::disconnected;
        size_t ordinal = 0;
        Sample default_value = 0.0f;
        std::optional<Sample> min {};
        std::optional<Sample> max {};
        size_t history = 0;
        size_t latency = 0;
        std::optional<ChannelTypeId> sample_channel_type {};

        bool operator==(IntrospectionPortInfo const&) const = default;
    };

    struct VirtualPortInfo {
        std::string name {};
        std::string type {};
        VirtualPortConnectivity connectivity = VirtualPortConnectivity::disconnected;
        size_t ordinal = 0;
        Sample default_value = 0.0f;
        std::optional<Sample> min {};
        std::optional<Sample> max {};
        Sample current_value = 0.0f;
        bool has_concrete_override = false;
        std::optional<ChannelTypeId> sample_channel_type {};
        std::string state_value {};

        bool operator==(VirtualPortInfo const&) const = default;
    };

    struct IntrospectionVirtualNode {
        struct Member {
            size_t ordinal = 0;
            std::string backing_node_id {};
            std::string kind {};
            std::string type_identity {};
            std::vector<IntrospectionPortInfo> sample_inputs {};
            std::vector<IntrospectionPortInfo> sample_outputs {};
            std::vector<IntrospectionPortInfo> event_inputs {};
            std::vector<IntrospectionPortInfo> event_outputs {};
        };

        std::string id {};
        std::string kind {};
        std::string source_identity {};
        std::string type_identity {};
        std::vector<SourceSpan> source_spans {};
        std::vector<IntrospectionPortInfo> sample_inputs {};
        std::vector<IntrospectionPortInfo> sample_outputs {};
        std::vector<IntrospectionPortInfo> event_inputs {};
        std::vector<IntrospectionPortInfo> event_outputs {};
        std::vector<std::string> backing_node_ids {};
        std::vector<Member> members {};
    };

    struct GraphBuilderPublicSamplePortChannel {
        std::vector<size_t> port_ordinals {};
        std::vector<SourceInfo> source_infos {};
    };

    struct GraphBuilderPublicSamplePortFamily {
        size_t family_ordinal = 0;
        std::string family_name {};
        InputConfig input_config {};
        OutputConfig output_config {};
        ChannelTypeId channel_type = ChannelTypeId::mono;
        std::vector<GraphBuilderPublicSamplePortChannel> channels {};
        std::vector<SourceInfo> source_infos {};
        bool authored_connected = false;
    };

    struct GraphBuilderPublicSamplePortFamilies {
        std::vector<GraphBuilderPublicSamplePortFamily> families {};
    };

    struct GraphBuilderPublicEventInput {
        size_t port_ordinal = 0;
        EventInputConfig config {};
        std::vector<SourceInfo> source_infos {};
        bool graph_connected = false;
    };

    struct GraphBuilderPublicEventOutput {
        size_t port_ordinal = 0;
        EventOutputConfig config {};
        std::vector<SourceInfo> source_infos {};
    };

    struct GraphIntrospectionMetadata {
        std::vector<IntrospectionVirtualNode> virtual_nodes;
        std::vector<GraphBuilderPublicSamplePortFamily> public_sample_inputs;
        std::vector<GraphBuilderPublicEventInput> public_event_inputs;
        std::vector<GraphBuilderPublicSamplePortFamily> public_sample_outputs;
        std::vector<GraphBuilderPublicEventOutput> public_event_outputs;
    };

    struct GraphBuildMetadata {
        std::vector<LoweredSubgraph> lowered_subgraphs;
        // Type identities are build diagnostics.  They make generated routing
        // nodes observable without exposing concrete execution nodes to UI code.
        std::vector<std::string> concrete_node_type_identities;
        std::vector<std::vector<SourceInfo>> node_source_infos;
        std::flat_map<std::string, std::vector<std::string>> virtual_node_ids_by_backing_node_id;
    };

    struct GraphBuildArtifact {
        std::string graph_id;
        std::vector<GraphSccWrapper> scc_wrappers;
        std::vector<std::shared_ptr<void const>> generated_node_storage;
        std::flat_set<GraphEdge> edges;
        std::flat_set<GraphEventEdge> event_edges;
        std::vector<DetachedInfo> detached;
        GraphExecutionPlan execution_plan;
        std::vector<InputConfig> public_inputs;
        std::vector<OutputConfig> public_outputs;
        std::vector<EventInputConfig> public_event_inputs;
        std::vector<EventOutputConfig> public_event_outputs;
        std::vector<InputPortPlan> public_output_buffer_plans;
        std::vector<SampleInputBinding> public_output_bindings;
        std::vector<SampleBufferStorage> public_input_fanout_storage;
        std::vector<std::vector<SampleOutputBinding>> public_input_targets;
        std::vector<DormancyGroup> dormancy_groups;
        size_t internal_latency;
        std::vector<std::string> node_ids;
    };
}
