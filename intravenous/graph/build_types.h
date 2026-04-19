#pragma once

#include "scc_wrapper.h"
#include "types.h"
#include "wiring.h"

#include <cstddef>
#include <cstdint>
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

    struct LogicalPortInfo {
        std::string name {};
        std::string type {};
        LogicalPortConnectivity connectivity = LogicalPortConnectivity::disconnected;

        bool operator==(LogicalPortInfo const&) const = default;
    };

    struct IntrospectionLogicalNode {
        std::string id {};
        std::string kind {};
        std::string source_identity {};
        std::vector<SourceSpan> source_spans {};
        std::vector<LogicalPortInfo> sample_inputs {};
        std::vector<LogicalPortInfo> sample_outputs {};
        std::vector<LogicalPortInfo> event_inputs {};
        std::vector<LogicalPortInfo> event_outputs {};
        std::vector<std::string> backing_node_ids {};
    };

    struct GraphIntrospectionMetadata {
        std::vector<LoweredSubgraph> lowered_subgraphs;
        std::vector<std::vector<SourceInfo>> node_source_infos;
        std::vector<IntrospectionLogicalNode> logical_nodes;
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
