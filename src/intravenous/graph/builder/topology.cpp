#include "topology.h"

namespace iv {
size_t GraphBuilderTopology::node_count() const
{
    return _nodes.size();
}

BuilderNode& GraphBuilderTopology::node(size_t index)
{
    return _nodes[index];
}

BuilderNode const& GraphBuilderTopology::node(size_t index) const
{
    return _nodes[index];
}

size_t GraphBuilderTopology::append_node(BuilderNode node)
{
    _nodes.push_back(std::move(node));
    return _nodes.size() - 1;
}

void GraphBuilderTopology::apply_ttl(size_t node_index, size_t ttl_samples)
{
    auto& builder_node = _nodes[node_index];
    builder_node.lifetime.ttl_samples = ttl_samples;
    if (!builder_node.materialization.is_placeholder()) {
        return;
    }

    size_t const begin = builder_node.lowered_subgraph.begin;
    size_t const end = begin + builder_node.lowered_subgraph.count;
    for (size_t child_node_index = begin; child_node_index < end; ++child_node_index) {
        _nodes[child_node_index].lifetime.ttl_samples = ttl_samples;
    }
}

void GraphBuilderTopology::add_sample_edge(GraphEdge edge)
{
    _edges.emplace(std::move(edge));
}

void GraphBuilderTopology::add_event_edge(GraphEventEdge edge)
{
    _event_edges.emplace(std::move(edge));
}

size_t GraphBuilderTopology::append_placeholder_node(
    std::span<OutputConfig const> sample_outputs,
    std::span<EventOutputConfig const> event_outputs
)
{
    return append_node(BuilderNode{
        .ports = NodePorts{
            .sample_outputs = std::vector<OutputConfig>(sample_outputs.begin(), sample_outputs.end()),
            .event_outputs = std::vector<EventOutputConfig>(event_outputs.begin(), event_outputs.end()),
        },
        .materialization = {},
    });
}

size_t GraphBuilderTopology::append_lowered_subgraph_placeholder(
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
)
{
    std::string type_identity = "lowered-subgraph:" + subgraph_kind;
    return append_node(BuilderNode{
        .ports = NodePorts{
            .sample_inputs = std::move(input_configs),
            .sample_outputs = std::move(output_configs),
            .event_inputs = std::move(event_input_configs),
            .event_outputs = std::move(event_output_configs),
        },
        .materialization = {},
        .lifetime = NodeLifetime{ .ttl_samples = std::nullopt },
        .lowered_subgraph = LoweredSubgraphBinding{
            .begin = lowered_subgraph_begin,
            .count = lowered_subgraph_count,
            .sample_input_targets = std::move(subgraph_input_targets),
            .sample_output_sources = std::move(subgraph_output_sources),
            .event_input_targets = std::move(subgraph_event_input_targets),
            .event_output_sources = std::move(subgraph_event_output_sources),
            .kind = std::move(subgraph_kind),
        },
        .type_identity = NodeTypeIdentity{ .value = std::move(type_identity) },
    });
}

size_t GraphBuilderTopology::append_embedded_child(
    GraphBuilderTopology const& child,
    std::span<InputConfig const> child_sample_inputs,
    std::span<OutputConfig const> child_sample_outputs,
    std::span<EventInputConfig const> child_event_inputs,
    std::span<EventOutputConfig const> child_event_outputs,
    size_t child_detach_offset
)
{
    size_t const placeholder_node = node_count();
    size_t const child_node_offset = placeholder_node + 1;

    auto remap_child_port = [&](PortId port) {
        if (port.node == GRAPH_ID) {
            return PortId{ placeholder_node, port.port };
        }
        return PortId{ child_node_offset + port.node, port.port };
    };

    append_lowered_subgraph_placeholder(
        {},
        std::vector<InputConfig>(child_sample_inputs.begin(), child_sample_inputs.end()),
        std::vector<OutputConfig>(child_sample_outputs.begin(), child_sample_outputs.end()),
        std::vector<EventInputConfig>(child_event_inputs.begin(), child_event_inputs.end()),
        std::vector<EventOutputConfig>(child_event_outputs.begin(), child_event_outputs.end()),
        child_node_offset,
        child.node_count(),
        std::vector<std::vector<PortId>>(child_sample_inputs.size()),
        std::vector<PortId>(child_sample_outputs.size()),
        std::vector<std::vector<PortId>>(child_event_inputs.size()),
        std::vector<PortId>(child_event_outputs.size())
    );

    for (size_t i = 0; i < child.node_count(); ++i) {
        BuilderNode copied_node = child.node(i);
        if (!copied_node.materialization.is_placeholder()) {
            auto materialize = std::move(copied_node.materialization.factory);
            copied_node.materialization.factory =
                [materialize = std::move(materialize), child_detach_offset](size_t parent_detach_offset) {
                    return materialize(parent_detach_offset + child_detach_offset);
                };
        } else {
            for (auto& targets : copied_node.lowered_subgraph.sample_input_targets) {
                for (auto& target : targets) {
                    target = remap_child_port(target);
                }
            }
            for (auto& source : copied_node.lowered_subgraph.sample_output_sources) {
                source = remap_child_port(source);
            }
            for (auto& targets : copied_node.lowered_subgraph.event_input_targets) {
                for (auto& target : targets) {
                    target = remap_child_port(target);
                }
            }
            for (auto& source : copied_node.lowered_subgraph.event_output_sources) {
                source = remap_child_port(source);
            }
        }
        append_node(std::move(copied_node));
    }

    child.for_each_sample_edge([&](GraphEdge const& edge) {
        PortId const source = remap_child_port(edge.source);
        PortId const target = remap_child_port(edge.target);
        if (source.node == placeholder_node && target.node == placeholder_node) {
            node(placeholder_node).lowered_subgraph.sample_output_sources[target.port] = source;
        } else if (source.node == placeholder_node) {
            node(placeholder_node).lowered_subgraph.sample_input_targets[source.port].push_back(target);
        } else if (target.node == placeholder_node) {
            node(placeholder_node).lowered_subgraph.sample_output_sources[target.port] = source;
        } else {
            add_sample_edge(GraphEdge{ source, target });
        }
    });

    child.for_each_event_edge([&](GraphEventEdge const& edge) {
        PortId const source = remap_child_port(edge.source);
        PortId const target = remap_child_port(edge.target);
        if (source.node == placeholder_node && target.node == placeholder_node) {
            node(placeholder_node).lowered_subgraph.event_output_sources[target.port] = source;
        } else if (source.node == placeholder_node) {
            node(placeholder_node).lowered_subgraph.event_input_targets[source.port].push_back(target);
        } else if (target.node == placeholder_node) {
            node(placeholder_node).lowered_subgraph.event_output_sources[target.port] = source;
        } else {
            add_event_edge(GraphEventEdge{ source, target, edge.conversion });
        }
    });

    return placeholder_node;
}
}
