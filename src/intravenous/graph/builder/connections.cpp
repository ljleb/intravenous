#include <intravenous/graph/builder/connections.h>

#include <intravenous/graph/builder/topology.h>

namespace iv {
bool GraphBuilderConnections::sample_input_is_connected(PortId target) const
{
    return _placed_sample_inputs.contains(target);
}

bool GraphBuilderConnections::event_input_is_connected(PortId target) const
{
    return _placed_event_inputs.contains(target);
}

void GraphBuilderConnections::connect_sample_input(
    GraphBuilderTopology& topology,
    GraphBuilderIdentity const& identity,
    PortId target,
    SamplePortRef source
)
{
    if (source.graph_builder == nullptr) {
        details::error("builder " + identity.value + ": empty SamplePortRef");
    }
    if (target.node >= topology.node_count() || target.port >= topology.node(target.node).inputs().size()) {
        details::error("sample input target is out of bounds in builder " + identity.value);
    }
    if (_placed_sample_inputs.contains(target)) {
        details::error("sample input was already placed in builder " + identity.value);
    }
    _placed_sample_inputs.insert(target);
    topology.add_sample_edge(GraphEdge{ source, target });
}

void GraphBuilderConnections::connect_event_input(
    GraphBuilderTopology& topology,
    std::span<EventInputConfig const> graph_event_inputs,
    GraphBuilderIdentity const& identity,
    PortId target,
    EventPortRef source
)
{
    if (source.graph_builder == nullptr) {
        details::error("builder " + identity.value + ": empty EventPortRef");
    }
    if (target.node >= topology.node_count() || target.port >= topology.node(target.node).event_inputs().size()) {
        details::error("event input target is out of bounds in builder " + identity.value);
    }
    if (_placed_event_inputs.contains(target)) {
        details::error("event input was already placed in builder " + identity.value);
    }
    auto const source_type = (source.node_index == GRAPH_ID)
        ? graph_event_inputs[source.output_port].type
        : topology.node(source.node_index).event_outputs()[source.output_port].type;
    auto const target_type = topology.node(target.node).event_inputs()[target.port].type;
    _placed_event_inputs.insert(target);
    topology.add_event_edge(GraphEventEdge{
        PortId{ source.node_index, source.output_port },
        target,
        EventConversionRegistry::instance().plan(source_type, target_type)
    });
}

void GraphBuilderConnections::mark_runtime_filled_sample_input(PortId target)
{
    _runtime_filled_sample_inputs.insert(target);
}

void GraphBuilderConnections::mark_runtime_filled_event_input(PortId target)
{
    _runtime_filled_event_inputs.insert(target);
}

GraphBuilderVacantInputs GraphBuilderConnections::collect_vacant_inputs(GraphBuilderTopology const& topology) const
{
    GraphBuilderVacantInputs result;
    size_t const original_node_count = topology.node_count();
    std::unordered_map<std::string, std::unordered_set<std::string>> types_by_logical_id;
    for (size_t node_i = 0; node_i < original_node_count; ++node_i) {
        auto const& node = topology.node(node_i);
        if (node.materialization.is_placeholder() || node.vacant_input_ownership.logical_node_id.empty()) {
            continue;
        }
        types_by_logical_id[node.vacant_input_ownership.logical_node_id].insert(node.type_identity.value);
    }
    std::unordered_map<std::string, size_t> next_member_ordinal_by_logical_id;
    for (size_t node_i = 0; node_i < original_node_count; ++node_i) {
        auto const& node = topology.node(node_i);
        if (node.materialization.is_placeholder()) {
            continue;
        }
        std::string const logical_node_id = node.vacant_input_ownership.logical_node_id;
        if (logical_node_id.empty()) {
            continue;
        }
        std::string final_logical_node_id = logical_node_id;
        if (auto it = types_by_logical_id.find(logical_node_id);
            it != types_by_logical_id.end() && it->second.size() > 1) {
            final_logical_node_id += "#type:" + details::stable_identity_suffix(node.type_identity.value);
        }
        size_t const member_ordinal = next_member_ordinal_by_logical_id[final_logical_node_id]++;
        for (size_t input_i = 0; input_i < node.inputs().size(); ++input_i) {
            PortId const target_port{ node_i, input_i };
            if (_placed_sample_inputs.contains(target_port)) {
                continue;
            }
            result.sample.push_back(GraphBuilderVacantSampleInput{
                .target = target_port,
                .logical_node_id = final_logical_node_id,
                .member_ordinal = member_ordinal,
                .config = node.inputs()[input_i],
            });
        }
        for (size_t input_i = 0; input_i < node.event_inputs().size(); ++input_i) {
            PortId const target_port{ node_i, input_i };
            if (_placed_event_inputs.contains(target_port)) {
                continue;
            }
            result.event.push_back(GraphBuilderVacantEventInput{
                .target = target_port,
                .logical_node_id = final_logical_node_id,
                .member_ordinal = member_ordinal,
                .config = node.event_inputs()[input_i],
            });
        }
    }
    return result;
}

void GraphBuilderConnections::import_child(GraphBuilderConnections const& child, size_t child_node_offset)
{
    for (PortId const port : child._placed_sample_inputs) {
        if (port.node != GRAPH_ID) {
            _placed_sample_inputs.insert(PortId{ child_node_offset + port.node, port.port });
        }
    }
    for (PortId const port : child._placed_event_inputs) {
        if (port.node != GRAPH_ID) {
            _placed_event_inputs.insert(PortId{ child_node_offset + port.node, port.port });
        }
    }
    for (PortId const port : child._runtime_filled_sample_inputs) {
        if (port.node != GRAPH_ID) {
            _runtime_filled_sample_inputs.insert(PortId{ child_node_offset + port.node, port.port });
        }
    }
    for (PortId const port : child._runtime_filled_event_inputs) {
        if (port.node != GRAPH_ID) {
            _runtime_filled_event_inputs.insert(PortId{ child_node_offset + port.node, port.port });
        }
    }
}
}
