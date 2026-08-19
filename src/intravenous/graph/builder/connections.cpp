#include <intravenous/graph/builder/connections.h>

#include <intravenous/graph/builder/topology.h>
#include <intravenous/graph/builder/node_bundles.h>
#include <intravenous/graph/builder/virtual_nodes.h>

#include <algorithm>
#include <ranges>
#include <unordered_map>

namespace iv {
namespace {
bool topology_sample_input_is_connected(
    GraphBuilderConnections const& connections,
    GraphBuilderNodeBundles const& node_bundles, TopologyPortId target) {
  auto const channels =
      node_bundles.sample_input_channels_for_topology_port(target);
  return std::ranges::any_of(channels, [&](auto channel) {
    return connections.sample_input_is_connected(channel);
  });
}

bool topology_sample_input_is_runtime_filled(
    GraphBuilderConnections const& connections,
    GraphBuilderNodeBundles const& node_bundles, TopologyPortId target) {
  auto const channels =
      node_bundles.sample_input_channels_for_topology_port(target);
  return std::ranges::any_of(channels, [&](auto channel) {
    return connections.sample_input_is_runtime_filled(channel);
  });
}

bool topology_sample_output_is_connected(
    GraphBuilderConnections const& connections,
    GraphBuilderNodeBundles const& node_bundles, TopologyPortId source) {
  auto const channels =
      node_bundles.sample_output_channels_for_topology_port(source);
  return std::ranges::any_of(channels, [&](auto channel) {
    return connections.sample_output_is_connected(channel);
  });
}
} // namespace

bool GraphBuilderConnections::sample_input_is_connected(
    SampleInputChannelId target) const {
  return std::ranges::any_of(
      _authored_sample_connections, [&](auto const& connection) {
        return std::ranges::contains(connection.target_channels, target);
      });
}

bool GraphBuilderConnections::sample_output_is_connected(
    SampleOutputChannelId source) const {
  return std::ranges::any_of(
      _authored_sample_connections, [&](auto const& connection) {
        return std::ranges::contains(connection.source_channels, source);
      });
}

bool GraphBuilderConnections::sample_input_is_runtime_filled(
    SampleInputChannelId target) const {
  return std::ranges::contains(_runtime_filled_sample_channels, target);
}

bool GraphBuilderConnections::sample_input_is_connected(TopologyPortId target) const {
  return _placed_sample_inputs.contains(target);
}

bool GraphBuilderConnections::event_input_is_connected(TopologyPortId target) const {
  return _placed_event_inputs.contains(target);
}

void GraphBuilderConnections::record_authored_sample_connection(
    AuthoredSampleConnection connection) {
  if (connection.source_channels.size() !=
      channel_count(connection.source_type)) {
    details::error(
        "authored sample connection source does not match its channel type");
  }
  if (connection.target_channels.size() !=
      channel_count(connection.target_type)) {
    details::error(
        "authored sample connection target does not match its channel type");
  }
  _authored_sample_connections.push_back(std::move(connection));
}

std::span<AuthoredSampleConnection const>
GraphBuilderConnections::authored_sample_connections() const {
  return _authored_sample_connections;
}

void GraphBuilderConnections::record_authored_event_connection(
    AuthoredEventConnection connection) {
  if (connection.sources.empty()) {
    details::error("authored event connection has no source");
  }
  if (connection.targets.empty()) {
    details::error("authored event connection has no target");
  }
  _authored_event_connections.push_back(std::move(connection));
}

std::span<AuthoredEventConnection const>
GraphBuilderConnections::authored_event_connections() const {
  return _authored_event_connections;
}

void GraphBuilderConnections::connect_sample_input(
    GraphBuilderTopology &topology, GraphBuilderIdentity const &identity,
    TopologyPortId target, TopologyPortId source) {
  if (target.node >= topology.node_count() ||
      target.port >= topology.ports(target.node).inputs().size()) {
    details::error("sample input target is out of bounds in builder " +
                   identity.value);
  }
  _placed_sample_inputs.insert(target);
  topology.add_sample_edge(TopologyEdge{source, target});
}

void GraphBuilderConnections::connect_event_input(
    GraphBuilderTopology &topology,
    std::span<EventInputConfig const> graph_event_inputs,
    GraphBuilderIdentity const &identity, TopologyPortId target, EventPortRef source) {
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
  topology.add_event_edge(TopologyEventEdge{
      static_cast<TopologyPortId>(source), target,
      EventConversionRegistry::instance().plan(source_type, target_type)});
}

void GraphBuilderConnections::mark_runtime_filled_sample_input(
    SampleInputChannelId target) {
  if (!std::ranges::contains(_runtime_filled_sample_channels, target)) {
    _runtime_filled_sample_channels.push_back(target);
  }
}

void GraphBuilderConnections::mark_runtime_filled_sample_input(TopologyPortId target) {
  _runtime_filled_sample_inputs.insert(target);
}

void GraphBuilderConnections::mark_runtime_filled_event_input(TopologyPortId target) {
  _runtime_filled_event_inputs.insert(target);
}

GraphBuilderVacantInputs GraphBuilderConnections::collect_vacant_inputs(
    GraphBuilderTopology const &topology,
    GraphBuilderNodeBundles const &node_bundles,
    GraphBuilderVirtualNodes const &virtual_nodes) const {
  GraphBuilderVacantInputs result;
  for (auto const &virtual_node : virtual_nodes.records()) {
    for (size_t member_ordinal = 0;
         member_ordinal < virtual_node.concrete_node_indices.size();
         ++member_ordinal) {
      size_t const node_i = virtual_node.concrete_node_indices[member_ordinal];
      auto const &node = topology.concrete_node(node_i);
      for (size_t input_i = 0; input_i < node.inputs().size(); ++input_i) {
        TopologyPortId const target_port{node_i, input_i};
        if (topology_sample_input_is_connected(
                *this, node_bundles, target_port)) {
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
        TopologyPortId const target_port{node_i, input_i};
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
    GraphBuilderNodeBundles const &node_bundles,
    GraphBuilderVirtualNodes const &virtual_nodes) const {
  GraphBuilderVirtualInputs result;
  for (auto const &virtual_node : virtual_nodes.records()) {
    for (size_t member_ordinal = 0;
         member_ordinal < virtual_node.concrete_node_indices.size();
         ++member_ordinal) {
      size_t const node_i = virtual_node.concrete_node_indices[member_ordinal];
      auto const &node = topology.concrete_node(node_i);
      for (size_t input_i = 0; input_i < node.inputs().size(); ++input_i) {
        TopologyPortId const target_port{node_i, input_i};
        result.sample.push_back(GraphBuilderVirtualSampleInput{
            .target = target_port,
            .virtual_node_id = virtual_node.id,
            .member_ordinal = member_ordinal,
            .config = node.inputs()[input_i],
            .has_existing_connection = topology_sample_input_is_connected(
                *this, node_bundles, target_port),
            .runtime_filled = topology_sample_input_is_runtime_filled(
                *this, node_bundles, target_port),
        });
      }
      for (size_t input_i = 0; input_i < node.event_inputs().size();
           ++input_i) {
        TopologyPortId const target_port{node_i, input_i};
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
      if (mapping.channels.empty()) {
        continue;
      }
      auto const first = mapping.channels.front();
      auto config = node_bundles.resolve_sample_input(
          {first.bundle, PortKind::sample, first.port}).config;
      config.channel_layout = mapping.channel_layout;
      auto const channel_total = channel_count(mapping.channel_layout.channel_type);
      std::vector<GraphBuilderVirtualSampleInputChannel> channels(channel_total);
      for (auto const target : mapping.channels) {
        if (target.channel >= channel_total) {
          details::error(
              "virtual sample input channel does not match its declared layout");
        }
        auto& channel = channels[target.channel];
        if (!std::ranges::contains(channel.targets, target)) {
          channel.targets.push_back(target);
        }
        channel.has_existing_connection = channel.has_existing_connection
            || sample_input_is_connected(target);
        channel.runtime_filled = channel.runtime_filled
            || sample_input_is_runtime_filled(target);
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
    GraphBuilderNodeBundles const &node_bundles,
    GraphBuilderVirtualNodes const &virtual_nodes) const {
  GraphBuilderVirtualOutputs result;

  std::unordered_set<TopologyPortId> connected_event_sources;
  topology.for_each_event_edge([&](TopologyEventEdge const &edge) {
    connected_event_sources.insert(edge.source);
  });

  for (auto const &virtual_node : virtual_nodes.records()) {
    for (size_t member_ordinal = 0;
         member_ordinal < virtual_node.concrete_node_indices.size();
         ++member_ordinal) {
      size_t const node_i = virtual_node.concrete_node_indices[member_ordinal];
      auto const &node = topology.concrete_node(node_i);
      for (size_t output_i = 0; output_i < node.outputs().size(); ++output_i) {
        TopologyPortId const source_port{node_i, output_i};
        result.sample.push_back(GraphBuilderVirtualSampleOutput{
            .source = source_port,
            .virtual_node_id = virtual_node.id,
            .member_ordinal = member_ordinal,
            .config = node.outputs()[output_i],
            .has_existing_downstream_connection =
                topology_sample_output_is_connected(
                    *this, node_bundles, source_port),
        });
      }
      for (size_t output_i = 0; output_i < node.event_outputs().size();
           ++output_i) {
        TopologyPortId const source_port{node_i, output_i};
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
    GraphBuilderNodeBundles const &node_bundles,
    GraphBuilderVirtualNodes const &virtual_nodes) const {
  GraphBuilderVirtualSampleOutputFamilies result;
  for (auto const &virtual_node : virtual_nodes.records()) {
    for (auto const &mapping : virtual_node.sample_outputs) {
      if (mapping.channels.empty()) {
        continue;
      }
      auto const first = mapping.channels.front();
      auto config = node_bundles.resolve_sample_output(
          {first.bundle, PortKind::sample, first.port}).config;
      config.channel_layout = mapping.channel_layout;
      auto const channel_total = channel_count(mapping.channel_layout.channel_type);
      std::vector<GraphBuilderVirtualSampleOutputChannel> channels(channel_total);
      for (auto const source : mapping.channels) {
        if (source.channel >= channel_total) {
          details::error(
              "virtual sample output channel does not match its declared layout");
        }
        auto& channel = channels[source.channel];
        if (!std::ranges::contains(channel.sources, source)) {
          channel.sources.push_back(source);
        }
        channel.has_existing_downstream_connection =
            channel.has_existing_downstream_connection
            || sample_output_is_connected(source);
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

void GraphBuilderConnections::import_child(
    GraphBuilderConnections const &child, size_t child_node_offset,
    size_t child_node_bundle_offset) {
  for (auto connection : child._authored_sample_connections) {
    for (auto &channel : connection.source_channels) {
      channel.bundle += child_node_bundle_offset;
    }
    for (auto &channel : connection.target_channels) {
      channel.bundle += child_node_bundle_offset;
    }
    _authored_sample_connections.push_back(std::move(connection));
  }
  for (auto connection : child._authored_event_connections) {
    for (auto &source : connection.sources) {
      source.bundle += child_node_bundle_offset;
    }
    for (auto &target : connection.targets) {
      target.bundle += child_node_bundle_offset;
    }
    _authored_event_connections.push_back(std::move(connection));
  }
  for (auto channel : child._runtime_filled_sample_channels) {
    channel.bundle += child_node_bundle_offset;
    if (!std::ranges::contains(_runtime_filled_sample_channels, channel)) {
      _runtime_filled_sample_channels.push_back(channel);
    }
  }

  for (TopologyPortId const port : child._placed_sample_inputs) {
    if (port.node != GRAPH_ID) {
      _placed_sample_inputs.insert(
          TopologyPortId{child_node_offset + port.node, port.port});
    }
  }
  for (TopologyPortId const port : child._placed_event_inputs) {
    if (port.node != GRAPH_ID) {
      _placed_event_inputs.insert(
          TopologyPortId{child_node_offset + port.node, port.port});
    }
  }
  for (TopologyPortId const port : child._runtime_filled_sample_inputs) {
    if (port.node != GRAPH_ID) {
      _runtime_filled_sample_inputs.insert(
          TopologyPortId{child_node_offset + port.node, port.port});
    }
  }
  for (TopologyPortId const port : child._runtime_filled_event_inputs) {
    if (port.node != GRAPH_ID) {
      _runtime_filled_event_inputs.insert(
          TopologyPortId{child_node_offset + port.node, port.port});
    }
  }
}
} // namespace iv
