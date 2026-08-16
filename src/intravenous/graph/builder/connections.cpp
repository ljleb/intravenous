#include <intravenous/graph/builder/connections.h>

#include <intravenous/graph/builder/topology.h>
#include <intravenous/graph/builder/virtual_nodes.h>

#include <algorithm>
#include <unordered_map>

namespace iv {
bool GraphBuilderConnections::sample_input_is_connected(PortId target) const {
  return _placed_sample_inputs.contains(target);
}

bool GraphBuilderConnections::event_input_is_connected(PortId target) const {
  return _placed_event_inputs.contains(target);
}

void GraphBuilderConnections::connect_sample_input(
    GraphBuilderTopology &topology, GraphBuilderIdentity const &identity,
    PortId target, SamplePortRef source) {
  if (source.graph_builder == nullptr) {
    details::error("builder " + identity.value + ": empty SamplePortRef");
  }
  if (target.node >= topology.node_count() ||
      target.port >= topology.ports(target.node).inputs().size()) {
    details::error("sample input target is out of bounds in builder " +
                   identity.value);
  }
  _placed_sample_inputs.insert(target);
  topology.add_sample_edge(GraphEdge{source, target});
}

void GraphBuilderConnections::connect_event_input(
    GraphBuilderTopology &topology,
    std::span<EventInputConfig const> graph_event_inputs,
    GraphBuilderIdentity const &identity, PortId target, EventPortRef source) {
  if (source.graph_builder == nullptr) {
    details::error("builder " + identity.value + ": empty EventPortRef");
  }
  if (target.node >= topology.node_count() ||
      target.port >= topology.ports(target.node).event_inputs().size()) {
    details::error("event input target is out of bounds in builder " +
                   identity.value);
  }
  auto const source_type = source.node_index == GRAPH_ID
                               ? graph_event_inputs[source.output_port].type
                               : topology.is_scope_boundary_port(
                                     PortId{source.node_index, source.output_port})
                                     ? topology.scope_boundary_event_output(
                                           PortId{source.node_index, source.output_port}).type
                                     : topology.ports(source.node_index)
                                           .event_outputs()[source.output_port]
                                           .type;
  auto const target_type =
      topology.ports(target.node).event_inputs()[target.port].type;
  _placed_event_inputs.insert(target);
  topology.add_event_edge(GraphEventEdge{
      PortId{source.node_index, source.output_port}, target,
      EventConversionRegistry::instance().plan(source_type, target_type)});
}

void GraphBuilderConnections::mark_runtime_filled_sample_input(PortId target) {
  _runtime_filled_sample_inputs.insert(target);
}

void GraphBuilderConnections::mark_runtime_filled_event_input(PortId target) {
  _runtime_filled_event_inputs.insert(target);
}

GraphBuilderVacantInputs GraphBuilderConnections::collect_vacant_inputs(
    GraphBuilderTopology const &topology,
    GraphBuilderVirtualNodes const &virtual_nodes) const {
  GraphBuilderVacantInputs result;
  for (auto const &virtual_node : virtual_nodes.records()) {
    for (size_t member_ordinal = 0;
         member_ordinal < virtual_node.concrete_node_indices.size();
         ++member_ordinal) {
      size_t const node_i = virtual_node.concrete_node_indices[member_ordinal];
      auto const &node = topology.concrete_node(node_i);
      for (size_t input_i = 0; input_i < node.inputs().size(); ++input_i) {
        PortId const target_port{node_i, input_i};
        if (_placed_sample_inputs.contains(target_port)) {
          continue;
        }
        result.sample.push_back(GraphBuilderVacantSampleInput{
            .target = target_port,
            .virtual_node_id = virtual_node.id,
            .member_ordinal = member_ordinal,
            .config = node.inputs()[input_i],
        });
      }
      for (size_t input_i = 0; input_i < node.event_inputs().size();
           ++input_i) {
        PortId const target_port{node_i, input_i};
        if (_placed_event_inputs.contains(target_port)) {
          continue;
        }
        result.event.push_back(GraphBuilderVacantEventInput{
            .target = target_port,
            .virtual_node_id = virtual_node.id,
            .member_ordinal = member_ordinal,
            .config = node.event_inputs()[input_i],
        });
      }
    }
  }
  return result;
}

GraphBuilderVirtualInputs GraphBuilderConnections::collect_virtual_inputs(
    GraphBuilderTopology const &topology,
    GraphBuilderVirtualNodes const &virtual_nodes) const {
  GraphBuilderVirtualInputs result;
  for (auto const &virtual_node : virtual_nodes.records()) {
    for (size_t member_ordinal = 0;
         member_ordinal < virtual_node.concrete_node_indices.size();
         ++member_ordinal) {
      size_t const node_i = virtual_node.concrete_node_indices[member_ordinal];
      auto const &node = topology.concrete_node(node_i);
      for (size_t input_i = 0; input_i < node.inputs().size(); ++input_i) {
        PortId const target_port{node_i, input_i};
        result.sample.push_back(GraphBuilderVirtualSampleInput{
            .target = target_port,
            .virtual_node_id = virtual_node.id,
            .member_ordinal = member_ordinal,
            .config = node.inputs()[input_i],
            .has_existing_connection =
                _placed_sample_inputs.contains(target_port),
            .runtime_filled =
                _runtime_filled_sample_inputs.contains(target_port),
        });
      }
      for (size_t input_i = 0; input_i < node.event_inputs().size();
           ++input_i) {
        PortId const target_port{node_i, input_i};
        result.event.push_back(GraphBuilderVirtualEventInput{
            .target = target_port,
            .virtual_node_id = virtual_node.id,
            .member_ordinal = member_ordinal,
            .config = node.event_inputs()[input_i],
            .has_existing_connection =
                _placed_event_inputs.contains(target_port),
            .runtime_filled =
                _runtime_filled_event_inputs.contains(target_port),
        });
      }
    }
  }
  return result;
}

GraphBuilderVirtualSampleInputFamilies
GraphBuilderConnections::collect_virtual_sample_input_families(
    GraphBuilderTopology const &topology,
    GraphBuilderVirtualNodes const &virtual_nodes) const {
  GraphBuilderVirtualSampleInputFamilies result;
  for (auto const &virtual_node : virtual_nodes.records()) {
    for (size_t member_ordinal = 0;
         member_ordinal < virtual_node.concrete_node_indices.size();
         ++member_ordinal) {
      size_t const node_i = virtual_node.concrete_node_indices[member_ordinal];
      auto const &node = topology.concrete_node(node_i);
      for (size_t input_i = 0; input_i < node.inputs().size(); ++input_i) {
        auto const &config = node.inputs()[input_i];
        PortId const target_port{node_i, input_i};
        std::vector<GraphBuilderVirtualSampleInputChannel> channels(
            channel_count(config.channel_layout.channel_type));
        for (auto &channel : channels) {
          channel.target = target_port;
          channel.has_existing_connection =
              _placed_sample_inputs.contains(target_port);
          channel.runtime_filled =
              _runtime_filled_sample_inputs.contains(target_port);
        }
        result.families.push_back(GraphBuilderVirtualSampleInputFamily{
            .virtual_node_id = virtual_node.id,
            .member_ordinal = member_ordinal,
            .family_ordinal = input_i,
            .family_name = config.name,
            .config = config,
            .channel_type = config.channel_layout.channel_type,
            .channels = std::move(channels),
        });
      }
    }
  }
  return result;
}

GraphBuilderVirtualOutputs GraphBuilderConnections::collect_virtual_outputs(
    GraphBuilderTopology const &topology,
    GraphBuilderVirtualNodes const &virtual_nodes) const {
  GraphBuilderVirtualOutputs result;

  std::unordered_set<PortId> connected_sample_sources;
  std::unordered_set<PortId> connected_event_sources;
  topology.for_each_sample_edge([&](GraphEdge const &edge) {
    connected_sample_sources.insert(edge.source);
  });
  topology.for_each_event_edge([&](GraphEventEdge const &edge) {
    connected_event_sources.insert(edge.source);
  });

  for (auto const &virtual_node : virtual_nodes.records()) {
    for (size_t member_ordinal = 0;
         member_ordinal < virtual_node.concrete_node_indices.size();
         ++member_ordinal) {
      size_t const node_i = virtual_node.concrete_node_indices[member_ordinal];
      auto const &node = topology.concrete_node(node_i);
      for (size_t output_i = 0; output_i < node.outputs().size(); ++output_i) {
        PortId const source_port{node_i, output_i};
        result.sample.push_back(GraphBuilderVirtualSampleOutput{
            .source = source_port,
            .virtual_node_id = virtual_node.id,
            .member_ordinal = member_ordinal,
            .config = node.outputs()[output_i],
            .has_existing_downstream_connection =
                connected_sample_sources.contains(source_port),
        });
      }
      for (size_t output_i = 0; output_i < node.event_outputs().size();
           ++output_i) {
        PortId const source_port{node_i, output_i};
        result.event.push_back(GraphBuilderVirtualEventOutput{
            .source = source_port,
            .virtual_node_id = virtual_node.id,
            .member_ordinal = member_ordinal,
            .config = node.event_outputs()[output_i],
            .has_existing_downstream_connection =
                connected_event_sources.contains(source_port),
        });
      }
    }
  }
  return result;
}

GraphBuilderVirtualSampleOutputFamilies
GraphBuilderConnections::collect_virtual_sample_output_families(
    GraphBuilderTopology const &topology,
    GraphBuilderVirtualNodes const &virtual_nodes) const {
  std::unordered_set<PortId> connected_sample_sources;
  topology.for_each_sample_edge([&](GraphEdge const &edge) {
    connected_sample_sources.insert(edge.source);
  });

  GraphBuilderVirtualSampleOutputFamilies result;
  for (auto const &virtual_node : virtual_nodes.records()) {
    for (size_t member_ordinal = 0;
         member_ordinal < virtual_node.concrete_node_indices.size();
         ++member_ordinal) {
      size_t const node_i = virtual_node.concrete_node_indices[member_ordinal];
      auto const &node = topology.concrete_node(node_i);
      for (size_t output_i = 0; output_i < node.outputs().size(); ++output_i) {
        auto const &config = node.outputs()[output_i];
        PortId const source_port{node_i, output_i};
        std::vector<GraphBuilderVirtualSampleOutputChannel> channels(
            channel_count(config.channel_layout.channel_type));
        for (auto &channel : channels) {
          channel.source = source_port;
          channel.has_existing_downstream_connection =
              connected_sample_sources.contains(source_port);
        }
        result.families.push_back(GraphBuilderVirtualSampleOutputFamily{
            .virtual_node_id = virtual_node.id,
            .member_ordinal = member_ordinal,
            .family_ordinal = output_i,
            .family_name = config.name,
            .config = config,
            .channel_type = config.channel_layout.channel_type,
            .channels = std::move(channels),
        });
      }
    }
  }
  return result;
}

void GraphBuilderConnections::import_child(GraphBuilderConnections const &child,
                                           size_t child_node_offset) {
  for (PortId const port : child._placed_sample_inputs) {
    if (port.node != GRAPH_ID) {
      _placed_sample_inputs.insert(
          PortId{child_node_offset + port.node, port.port});
    }
  }
  for (PortId const port : child._placed_event_inputs) {
    if (port.node != GRAPH_ID) {
      _placed_event_inputs.insert(
          PortId{child_node_offset + port.node, port.port});
    }
  }
  for (PortId const port : child._runtime_filled_sample_inputs) {
    if (port.node != GRAPH_ID) {
      _runtime_filled_sample_inputs.insert(
          PortId{child_node_offset + port.node, port.port});
    }
  }
  for (PortId const port : child._runtime_filled_event_inputs) {
    if (port.node != GRAPH_ID) {
      _runtime_filled_event_inputs.insert(
          PortId{child_node_offset + port.node, port.port});
    }
  }
}
} // namespace iv
