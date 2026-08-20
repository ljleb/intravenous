#pragma once

#include <intravenous/graph/builder/names.h>
#include <intravenous/graph/builder/node_refs.h>
#include <intravenous/basic_nodes/routing.h>

#include <span>
#include <type_traits>
#include <typeinfo>
#include <unordered_set>
#include <vector>

namespace iv {
    struct TopologyEdge {
        TopologyPortId source {}, target {};

        bool operator==(TopologyEdge const& other) const
        {
            return source == other.source && target == other.target;
        }
    };

    struct TopologyEventEdge {
        TopologyPortId source {}, target {};
        EventConversionPlan conversion {};

        bool operator==(TopologyEventEdge const&) const = default;
    };

    struct TopologyEdgeHash {
        size_t operator()(TopologyEdge const& edge) const
        {
            auto hash = std::hash<TopologyPortId>{};
            return hash(edge.source) ^ (~hash(edge.target) - 1);
        }
    };

    struct TopologyEventEdgeHash {
        size_t operator()(TopologyEventEdge const& edge) const
        {
            auto hash = std::hash<TopologyPortId>{};
            return hash(edge.source) ^ (~hash(edge.target) - 1);
        }
    };

    class GraphBuilderTopology {
    public:
        size_t node_count() const;
        ConcreteNode& concrete_node(size_t index);
        ConcreteNode const& concrete_node(size_t index) const;
        SubgraphNode& subgraph_node(size_t index);
        SubgraphNode const& subgraph_node(size_t index) const;
        bool is_subgraph_node(size_t index) const;
        NodePorts& ports(size_t index);
        NodePorts const& ports(size_t index) const;
        NodeLifetime& lifetime(size_t index);
        NodeLifetime const& lifetime(size_t index) const;
        NodeTypeIdentity const& type_identity(size_t index) const;
        size_t append_node(ConcreteNode node);
        size_t append_node(SubgraphNode node);
        void apply_ttl(size_t node_index, size_t ttl_samples);
        void add_sample_edge(TopologyEdge edge);
        void add_event_edge(TopologyEventEdge edge);
        void erase_sample_edges_matching(auto&& predicate);
        void erase_event_edges_matching(auto&& predicate);
        template<class Fn>
        void for_each_sample_edge(Fn&& fn) const
        {
            for (auto const& edge : _edges) {
                fn(edge);
            }
        }
        template<class Fn>
        void for_each_event_edge(Fn&& fn) const
        {
            for (auto const& edge : _event_edges) {
                fn(edge);
            }
        }
        ScopeBoundaryPortId append_scope_sample_input(OutputConfig);
        ScopeBoundaryPortId append_scope_event_input(EventOutputConfig);
        bool is_scope_boundary_port(TopologyPortId) const;
        EventOutputConfig const& scope_boundary_event_output(ScopeBoundaryPortId) const;
        EventOutputConfig const& scope_boundary_event_output(TopologyPortId) const;
        size_t append_lowered_subgraph_node(
            std::string subgraph_kind,
            std::vector<InputConfig> input_configs,
            std::vector<OutputConfig> output_configs,
            std::vector<EventInputConfig> event_input_configs,
            std::vector<EventOutputConfig> event_output_configs,
            size_t lowered_subgraph_begin,
            size_t lowered_subgraph_count,
            std::vector<std::vector<TopologyPortId>> subgraph_input_targets,
            std::vector<TopologyPortId> subgraph_output_sources,
            std::vector<std::vector<TopologyPortId>> subgraph_event_input_targets,
            std::vector<TopologyPortId> subgraph_event_output_sources
        );
        size_t append_embedded_child(
            GraphBuilderTopology const& child,
            std::span<InputConfig const> child_sample_inputs,
            std::span<OutputConfig const> child_sample_outputs,
            std::span<EventInputConfig const> child_event_inputs,
            std::span<EventOutputConfig const> child_event_outputs,
            size_t child_detach_offset
        );

    private:
        std::vector<StoredNode> _nodes {};
        struct ScopeBoundaryPort {
            std::optional<OutputConfig> sample_output {};
            std::optional<EventOutputConfig> event_output {};
        };
        std::vector<ScopeBoundaryPort> _scope_boundary_ports {};
        std::unordered_set<TopologyEdge, TopologyEdgeHash> _edges {};
        std::unordered_set<TopologyEventEdge, TopologyEventEdgeHash> _event_edges {};
    };

    template<class Predicate>
    void GraphBuilderTopology::erase_sample_edges_matching(Predicate&& predicate)
    {
        std::erase_if(_edges, std::forward<Predicate>(predicate));
    }

    template<class Predicate>
    void GraphBuilderTopology::erase_event_edges_matching(Predicate&& predicate)
    {
        std::erase_if(_event_edges, std::forward<Predicate>(predicate));
    }

}
