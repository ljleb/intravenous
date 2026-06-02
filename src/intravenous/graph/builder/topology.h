#pragma once

#include "names.h"
#include "node_refs.h"
#include "../../basic_nodes/routing.h"

#include <span>
#include <type_traits>
#include <typeinfo>
#include <unordered_set>
#include <vector>

namespace iv {
    class GraphBuilderTopology {
    public:
        size_t node_count() const;
        BuilderNode& node(size_t index);
        BuilderNode const& node(size_t index) const;
        size_t append_node(BuilderNode node);
        void apply_ttl(size_t node_index, size_t ttl_samples);
        void add_sample_edge(GraphEdge edge);
        void add_event_edge(GraphEventEdge edge);
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
        size_t append_placeholder_node(
            std::span<OutputConfig const> sample_outputs,
            std::span<EventOutputConfig const> event_outputs
        );
        template<class Config>
        static void validate_output_port_configs(
            std::span<Config const> configs,
            std::string_view node_label,
            std::string_view kind
        );
        template<class Node, class... Args>
        details::node_ref_for_t<Node> insert_node(GraphBuilder& builder, Args&&... args);
        size_t append_lowered_subgraph_placeholder(
            std::string subgraph_kind,
            std::vector<InputConfig> input_configs,
            std::vector<OutputConfig> output_configs,
            std::vector<EventInputConfig> event_input_configs,
            std::vector<EventOutputConfig> event_output_configs,
            size_t lowered_subgraph_begin,
            size_t lowered_subgraph_count,
            std::vector<std::vector<PortId>> subgraph_input_targets,
            std::vector<PortId> subgraph_output_sources,
            std::vector<std::vector<PortId>> subgraph_event_input_targets,
            std::vector<PortId> subgraph_event_output_sources
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
        std::vector<BuilderNode> _nodes {};
        std::unordered_set<GraphEdge> _edges {};
        std::unordered_set<GraphEventEdge> _event_edges {};
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

    template<class Config>
    void GraphBuilderTopology::validate_output_port_configs(
        std::span<Config const> configs,
        std::string_view node_label,
        std::string_view kind
    )
    {
        if (configs.size() <= 1) {
            return;
        }
        for (auto const& config : configs) {
            if (config.name.empty()) {
                details::error(
                    std::string(node_label)
                    + ": output "
                    + std::string(kind)
                    + " ports require names when more than one output is exposed"
                );
            }
        }
    }

    template<class Node, class... Args>
    details::node_ref_for_t<Node> GraphBuilderTopology::insert_node(GraphBuilder& builder, Args&&... args)
    {
        using StoredNode = std::remove_cvref_t<Node>;
        StoredNode node_value(std::forward<Args>(args)...);
        auto inputs = get_inputs(node_value);
        auto outputs = get_outputs(node_value);
        auto event_inputs = get_event_inputs(node_value);
        auto event_outputs = get_event_outputs(node_value);

        auto const node_type = typeid(StoredNode).name();
        validate_output_port_configs(
            std::span<OutputConfig const>(std::begin(outputs), std::end(outputs)),
            node_type,
            "sample"
        );
        validate_output_port_configs(
            std::span<EventOutputConfig const>(std::begin(event_outputs), std::end(event_outputs)),
            node_type,
            "event"
        );
        auto materialize = [node_value = std::move(node_value)]([[maybe_unused]] size_t detach_id_offset) {
            if constexpr (std::same_as<StoredNode, DetachWriterNode>) {
                return TypeErasedNode(DetachWriterNode{
                    DetachArrayId(node_value.id.id + detach_id_offset),
                    node_value.loop_extra_latency
                });
            } else if constexpr (std::same_as<StoredNode, DetachReaderNode>) {
                return TypeErasedNode(DetachReaderNode{
                    DetachArrayId(node_value.id.id + detach_id_offset),
                    node_value.loop_extra_latency
                });
            } else {
                return TypeErasedNode(node_value);
            }
        };

        size_t const node_index = append_node(BuilderNode{
            .ports = NodePorts{
                .sample_inputs = std::vector<InputConfig>(std::begin(inputs), std::end(inputs)),
                .sample_outputs = std::vector<OutputConfig>(std::begin(outputs), std::end(outputs)),
                .event_inputs = std::vector<EventInputConfig>(std::begin(event_inputs), std::end(event_inputs)),
                .event_outputs = std::vector<EventOutputConfig>(std::begin(event_outputs), std::end(event_outputs)),
            },
            .materialization = NodeMaterialization{ .factory = std::move(materialize) },
            .type_identity = NodeTypeIdentity{ .value = details::demangle_type_name(typeid(StoredNode).name()) },
        });
        if constexpr (details::has_fixed_output_count_v<StoredNode>) {
            return StructuredNodeRef<StoredNode>(builder, node_index);
        } else if constexpr (details::should_preserve_node_type_v<StoredNode>) {
            return TypedNodeRef<StoredNode>(builder, node_index);
        } else {
            return NodeRef(builder, node_index);
        }
    }
}
