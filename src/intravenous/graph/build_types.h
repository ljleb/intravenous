#pragma once

#include <intravenous/lane_node/channels.h>
#include <intravenous/graph/scc_wrapper.h>
#include <intravenous/graph/types.h>
#include <intravenous/graph/wiring.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
    enum class LogicalPortConnectivity : std::uint8_t {
        disconnected,
        connected,
        mixed,
    };

    struct IntrospectionPortInfo {
        std::string name {};
        std::string type {};
        LogicalPortConnectivity connectivity = LogicalPortConnectivity::disconnected;
        size_t ordinal = 0;
        Sample default_value = 0.0f;
        size_t history = 0;
        size_t latency = 0;
        std::optional<ChannelTypeId> sample_channel_type {};

        bool operator==(IntrospectionPortInfo const&) const = default;
    };

    struct LogicalPortInfo {
        std::string name {};
        std::string type {};
        LogicalPortConnectivity connectivity = LogicalPortConnectivity::disconnected;
        size_t ordinal = 0;
        Sample default_value = 0.0f;
        Sample current_value = 0.0f;
        bool has_concrete_override = false;
        std::optional<ChannelTypeId> sample_channel_type {};

        bool operator==(LogicalPortInfo const&) const = default;
    };

    struct IntrospectionLogicalNode {
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

    struct GraphIntrospectionMetadata {
        std::vector<IntrospectionLogicalNode> logical_nodes;
    };

    struct GraphBuildMetadata {
        std::vector<LoweredSubgraph> lowered_subgraphs;
        std::vector<std::vector<SourceInfo>> node_source_infos;
        std::unordered_map<std::string, std::vector<std::string>> logical_node_ids_by_backing_node_id;
    };

    struct GraphBuildArtifact {
        std::string graph_id;
        std::vector<GraphSccWrapper> scc_wrappers;
        std::unordered_set<GraphEdge> edges;
        std::unordered_set<GraphEventEdge> event_edges;
        std::vector<DetachedInfo> detached;
        GraphExecutionPlan execution_plan;
        std::vector<InputConfig> public_inputs;
        std::vector<OutputConfig> public_outputs;
        std::vector<EventInputConfig> public_event_inputs;
        std::vector<EventOutputConfig> public_event_outputs;
        std::vector<PortBufferPlan> public_output_buffer_plans;
        std::vector<DormancyGroup> dormancy_groups;
        size_t internal_latency;
        std::vector<std::string> node_ids;
    };
}
