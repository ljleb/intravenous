#include <intravenous/graph/builder/connections.h>

#include <intravenous/graph/builder/topology.h>
#include <intravenous/graph/builder/node_bundles.h>
#include <intravenous/graph/builder/virtual_nodes.h>

#include <algorithm>
#include <unordered_map>

namespace iv {
namespace {
template <class Channel, class Ports>
void append_resolved_channels(std::vector<Channel> &channels,
                              Ports const &ports,
                              ChannelLayout layout, auto connected) {
  auto const channel_total = channel_count(layout.channel_type);
  if (ports.size() != 1 && ports.size() != channel_total) {
    details::error("NodeBundle port does not match its declared channel layout");
  }
  for (size_t channel = 0; channel < channel_total; ++channel) {
    auto const port = ports.size() == 1 ? ports.front() : ports[channel];
    if constexpr (requires { channels[channel].targets; }) {
      channels[channel].targets.push_back(port);
    } else {
      channels[channel].sources.push_back(port);
    }
    if constexpr (requires { channels[channel].has_existing_connection; }) {
      channels[channel].has_existing_connection =
          channels[channel].has_existing_connection || connected(port);
    } else {
      channels[channel].has_existing_downstream_connection =
          channels[channel].has_existing_downstream_connection || connected(port);
    }
  }
}
} // namespace
bool GraphBuilderConnections::sample_input_is_connected(ConcretePortId target) const {
  return _placed_sample_inputs.contains(target);
}

bool GraphBuilderConnections::event_input_is_connected(ConcretePortId target) const {
  return _placed_event_inputs.contains(target);
}

void GraphBuilderConnections::connect_sample_input(
    GraphBuilderTopology &topology, GraphBuilderIdentity const &identity,
    ConcretePortId target, ConcretePortId source) {
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
    GraphBuilderIdentity const &identity, ConcretePortId target, EventPortRef source) {
  if (source.graph_builder == nullptr) {
    details::error("builder " + identity.value + ": empty EventPortRef");
  }
  if (target.node >= topology.node_count() ||
      target.port >= topology.ports(target.node).event_inputs().size()) {
    details::error("event input target is out of bounds in builder " +
                   identity.value);
  }
  auto const source_type = source.graph_input_port
                               ? graph_event_inputs[
                                     source.graph_input_port->port_ordinal].type
                               : source.scope_boundary_port
                                     ? topology.scope_boundary_event_output(
                                           *source.scope_boundary_port).type
                                     : topology.ports(source.node_index)
                                           .event_outputs()[source.output_port]
                                           .type;
  auto const target_type =
      topology.ports(target.node).event_inputs()[target.port].type;
  _placed_event_inputs.insert(target);
  topology.add_event_edge(GraphEventEdge{
      static_cast<ConcretePortId>(source), target,
      EventConversionRegistry::instance().plan(source_type, target_type)});
}

void GraphBuilderConnections::mark_runtime_filled_sample_input(ConcretePortId target) {
  _runtime_filled_sample_inputs.insert(target);
}

void GraphBuilderConnections::mark_runtime_filled_event_input(ConcretePortId target) {
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
        ConcretePortId const target_port{node_i, input_i};
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
        ConcretePortId const target_port{node_i, input_i};
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
        ConcretePortId const target_port{node_i, input_i};
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
        ConcretePortId const target_port{node_i, input_i};
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
    GraphBuilderNodeBundles const &node_bundles,
    GraphBuilderVirtualNodes const &virtual_nodes) const {
  GraphBuilderVirtualSampleInputFamilies result;
  for (auto const &virtual_node : virtual_nodes.records()) {
    for (auto const &mapping : virtual_node.sample_inputs) {
      if (mapping.node_bundle_ports.empty()) {
        continue;
      }
      auto config =
          node_bundles.resolve_sample_input(mapping.node_bundle_ports.front()).config;
      config.channel_layout = mapping.channel_layout;
      auto const channel_total = channel_count(mapping.channel_layout.channel_type);
      std::vector<GraphBuilderVirtualSampleInputChannel> channels(channel_total);
      for (auto const bundle_port : mapping.node_bundle_ports) {
        auto const descriptor = node_bundles.resolve_sample_input(bundle_port);
        auto const &ports = descriptor.endpoints;
        append_resolved_channels(channels, ports, mapping.channel_layout,
                                 [&](ConcretePortId port) { return _placed_sample_inputs.contains(port); });
        for (size_t channel = 0; channel < channel_total; ++channel) {
          auto const port = ports.size() == 1 ? ports.front() : ports[channel];
          channels[channel].runtime_filled = channels[channel].runtime_filled
              || _runtime_filled_sample_inputs.contains(port);
        }
      }
      result.families.push_back(GraphBuilderVirtualSampleInputFamily{
          .virtual_node_id = virtual_node.id,
          .family_ordinal = mapping.ordinal,
          .family_name = mapping.name,
          .config = std::move(config),
          .channel_type = mapping.channel_layout.channel_type,
          .channels = std::move(channels),
      });
    }
  }
  return result;
}

GraphBuilderVirtualOutputs GraphBuilderConnections::collect_virtual_outputs(
    GraphBuilderTopology const &topology,
    GraphBuilderVirtualNodes const &virtual_nodes) const {
  GraphBuilderVirtualOutputs result;

  std::unordered_set<ConcretePortId> connected_sample_sources;
  std::unordered_set<ConcretePortId> connected_event_sources;
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
        ConcretePortId const source_port{node_i, output_i};
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
        ConcretePortId const source_port{node_i, output_i};
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
    GraphBuilderNodeBundles const &node_bundles,
    GraphBuilderVirtualNodes const &virtual_nodes) const {
  std::unordered_set<ConcretePortId> connected_sample_sources;
  topology.for_each_sample_edge([&](GraphEdge const &edge) {
    connected_sample_sources.insert(edge.source);
  });

  GraphBuilderVirtualSampleOutputFamilies result;
  for (auto const &virtual_node : virtual_nodes.records()) {
    for (auto const &mapping : virtual_node.sample_outputs) {
      if (mapping.node_bundle_ports.empty()) {
        continue;
      }
      auto config =
          node_bundles.resolve_sample_output(mapping.node_bundle_ports.front()).config;
      config.channel_layout = mapping.channel_layout;
      auto const channel_total = channel_count(mapping.channel_layout.channel_type);
      std::vector<GraphBuilderVirtualSampleOutputChannel> channels(channel_total);
      for (auto const bundle_port : mapping.node_bundle_ports) {
        auto const descriptor = node_bundles.resolve_sample_output(bundle_port);
        append_resolved_channels(channels, descriptor.endpoints,
                                 mapping.channel_layout,
                                 [&](ConcretePortId port) { return connected_sample_sources.contains(port); });
      }
      result.families.push_back(GraphBuilderVirtualSampleOutputFamily{
          .virtual_node_id = virtual_node.id,
          .family_ordinal = mapping.ordinal,
          .family_name = mapping.name,
          .config = std::move(config),
          .channel_type = mapping.channel_layout.channel_type,
          .channels = std::move(channels),
      });
    }
  }
  return result;
}

void GraphBuilderConnections::import_child(GraphBuilderConnections const &child,
                                           size_t child_node_offset) {
  for (ConcretePortId const port : child._placed_sample_inputs) {
    if (port.node != GRAPH_ID) {
      _placed_sample_inputs.insert(
          ConcretePortId{child_node_offset + port.node, port.port});
    }
  }
  for (ConcretePortId const port : child._placed_event_inputs) {
    if (port.node != GRAPH_ID) {
      _placed_event_inputs.insert(
          ConcretePortId{child_node_offset + port.node, port.port});
    }
  }
  for (ConcretePortId const port : child._runtime_filled_sample_inputs) {
    if (port.node != GRAPH_ID) {
      _runtime_filled_sample_inputs.insert(
          ConcretePortId{child_node_offset + port.node, port.port});
    }
  }
  for (ConcretePortId const port : child._runtime_filled_event_inputs) {
    if (port.node != GRAPH_ID) {
      _runtime_filled_event_inputs.insert(
          ConcretePortId{child_node_offset + port.node, port.port});
    }
  }
}
} // namespace iv
