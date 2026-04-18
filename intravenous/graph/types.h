#pragma once

#include "ports.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
#include <optional>
#include <string>
#include <vector>

namespace iv {
    struct PortId {
        size_t node;
        size_t port;

        PortId(size_t node = 0, size_t port = 0) :
            node(node), port(port)
        {}

        bool operator==(PortId const&) const = default;
    };

    struct GraphEdge {
        PortId source, target;

        GraphEdge(PortId source = {}, PortId target = {}) :
            source(source), target(target)
        {}

        bool operator==(GraphEdge const&) const = default;
    };

    struct GraphEventEdge {
        PortId source, target;
        EventConversionPlan conversion;

        GraphEventEdge(
            PortId source = {},
            PortId target = {},
            EventConversionPlan conversion = {}
        ) :
            source(source), target(target), conversion(std::move(conversion))
        {}

        bool operator==(GraphEventEdge const&) const = default;
    };

    struct EventOutputBinding {
        std::string target;
        EventConversionPlan conversion;

        bool operator==(EventOutputBinding const&) const = default;
    };

    struct DetachedInfo {
        size_t detach_id = 0;
        PortId original_source;
        size_t writer_node = std::numeric_limits<size_t>::max();
        PortId reader_output;
        size_t loop_extra_latency = 1;

        bool operator==(DetachedInfo const&) const = default;
    };

    struct SourceSpan {
        std::string file_path {};
        uint32_t begin = 0;
        uint32_t end = 0;

        bool operator==(SourceSpan const&) const = default;
    };

    inline constexpr size_t GRAPH_ID = std::numeric_limits<size_t>::max();

    struct GraphRegion {
        std::vector<size_t> nodes;
        std::vector<size_t> execution_order;
        size_t max_block_size;
    };

    struct DormancySamplePort {
        std::string export_id;
        size_t history = 0;
    };

    struct DormancyEventPort {
        std::string export_id;
    };

    struct DormancyGroup {
        size_t parent_group = GRAPH_ID;
        size_t subtree_end_exclusive = 0;
        std::vector<size_t> member_nodes;
        std::vector<size_t> wake_check_regions;
        std::vector<DormancySamplePort> sample_input_frontier;
        std::vector<DormancyEventPort> event_input_frontier;
        std::vector<DormancySamplePort> sample_output_frontier;
        std::optional<size_t> ttl_samples;
        bool can_skip = false;
    };

    struct LoweredSubgraphSpec {
        struct PortRef {
            std::string node_id {};
            size_t port = 0;
            bool is_graph_port = false;
        };

        size_t parent_scope = GRAPH_ID;
        std::string kind;
        std::vector<std::string> member_node_ids;
        std::vector<SourceSpan> source_spans;
        std::vector<InputConfig> sample_inputs;
        std::vector<OutputConfig> sample_outputs;
        std::vector<EventInputConfig> event_inputs;
        std::vector<EventOutputConfig> event_outputs;
        std::vector<std::vector<PortRef>> sample_input_targets;
        std::vector<PortRef> sample_output_sources;
        std::vector<std::vector<PortRef>> event_input_targets;
        std::vector<PortRef> event_output_sources;
        std::optional<size_t> ttl_samples;
    };

    struct LoweredSubgraph {
        size_t parent_scope = GRAPH_ID;
        std::string kind;
        std::vector<size_t> member_nodes;
        std::vector<SourceSpan> source_spans;
        std::vector<InputConfig> sample_inputs;
        std::vector<OutputConfig> sample_outputs;
        std::vector<EventInputConfig> event_inputs;
        std::vector<EventOutputConfig> event_outputs;
        std::vector<std::vector<PortId>> sample_input_targets;
        std::vector<PortId> sample_output_sources;
        std::vector<std::vector<PortId>> event_input_targets;
        std::vector<PortId> event_output_sources;
        std::optional<size_t> ttl_samples;
    };

    struct GraphExecutionPlan {
        std::vector<GraphRegion> regions;
        std::vector<size_t> node_to_region;
        std::vector<size_t> region_order;
    };
}

template<>
struct std::hash<iv::PortId>
{
    std::hash<size_t> size_t_hash;

    std::size_t operator()(const iv::PortId& p) const
    {
        return size_t_hash(p.node) ^ (~size_t_hash(p.port) - 1);
    }
};

template<>
struct std::hash<iv::GraphEdge>
{
    std::hash<iv::PortId> port_id_hash;

    std::size_t operator()(const iv::GraphEdge& e) const
    {
        return port_id_hash(e.source) ^ (~port_id_hash(e.target) - 1);
    }
};

template<>
struct std::hash<iv::GraphEventEdge>
{
    std::hash<iv::PortId> port_id_hash;

    std::size_t operator()(const iv::GraphEventEdge& e) const
    {
        return port_id_hash(e.source) ^ (~port_id_hash(e.target) - 1);
    }
};
