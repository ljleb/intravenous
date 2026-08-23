#include <intravenous/graph/builder/connections.h>

#include <intravenous/graph/builder/node_bundles.h>
#include <intravenous/graph/builder/virtual_nodes.h>

#include <algorithm>
#include <ranges>

namespace iv {
bool GraphBuilderConnections::sample_input_is_connected(
    SampleInputChannelId target) const {
  return std::ranges::any_of(_authored_sample_connections,
      [&](auto const& c) { return std::ranges::contains(c.target_channels, target); });
}
bool GraphBuilderConnections::sample_output_is_connected(
    SampleOutputChannelId source) const {
  return std::ranges::any_of(_authored_sample_connections,
      [&](auto const& c) { return std::ranges::contains(c.source_channels, source); });
}
bool GraphBuilderConnections::event_input_is_connected(
    EventInputPortId target) const {
  return std::ranges::any_of(_authored_event_connections,
      [&](auto const& c) { return std::ranges::contains(c.targets, target); });
}
bool GraphBuilderConnections::event_output_is_connected(
    EventOutputPortId source) const {
  return std::ranges::any_of(_authored_event_connections,
      [&](auto const& c) { return std::ranges::contains(c.sources, source); });
}

void GraphBuilderConnections::record_authored_sample_connection(
    AuthoredSampleConnection connection) {
  if (connection.source_channels.size() != channel_count(connection.source_type))
    details::error("authored sample connection source does not match its channel type");
  if (connection.target_channels.size() != channel_count(connection.target_type))
    details::error("authored sample connection target does not match its channel type");
  _authored_sample_connections.push_back(std::move(connection));
}
std::span<AuthoredSampleConnection const>
GraphBuilderConnections::authored_sample_connections() const {
  return _authored_sample_connections;
}
void GraphBuilderConnections::record_authored_event_connection(
    AuthoredEventConnection connection) {
  if (connection.sources.empty()) details::error("authored event connection has no source");
  if (connection.targets.empty()) details::error("authored event connection has no target");
  _authored_event_connections.push_back(std::move(connection));
}
std::span<AuthoredEventConnection const>
GraphBuilderConnections::authored_event_connections() const {
  return _authored_event_connections;
}

GraphBuilderVacantInputs GraphBuilderConnections::collect_vacant_inputs(
    GraphBuilderNodeBundles const& bundles,
    GraphBuilderVirtualNodes const& virtual_nodes) const {
  GraphBuilderVacantInputs result;
  for (auto const& virtual_node : virtual_nodes.records()) {
    for (size_t member = 0; member < virtual_node.node_bundle_handles.size(); ++member) {
      auto const handle = virtual_node.node_bundle_handles[member];
      auto const& bundle = bundles.bundle(handle);
      for (size_t input = 0; input < bundle.sample_input_count(); ++input) {
        NodeBundlePortId const target{handle, PortKind::sample, input};
        auto const channels = bundles.sample_input_channels(target);
        if (std::ranges::any_of(channels,
            [&](auto channel) { return sample_input_is_connected(channel); })) continue;
        result.sample.push_back({target, virtual_node.id, member,
                                 bundles.resolve_sample_input(target).config});
      }
      for (size_t input = 0; input < bundle.event_input_count(); ++input) {
        NodeBundlePortId const target{handle, PortKind::event, input};
        auto const ports = bundles.event_input_ports(target);
        if (std::ranges::any_of(ports,
            [&](auto port) { return event_input_is_connected(port); })) continue;
        result.event.push_back({target, virtual_node.id, member,
                                bundles.resolve_event_input(target).config});
      }
    }
  }
  return result;
}

GraphBuilderVirtualInputs GraphBuilderConnections::collect_virtual_inputs(
    GraphBuilderNodeBundles const& bundles,
    GraphBuilderVirtualNodes const& virtual_nodes) const {
  GraphBuilderVirtualInputs result;
  for (auto const& virtual_node : virtual_nodes.records()) {
    for (size_t member = 0; member < virtual_node.node_bundle_handles.size(); ++member) {
      auto const handle = virtual_node.node_bundle_handles[member];
      auto const& bundle = bundles.bundle(handle);
      for (size_t input = 0; input < bundle.sample_input_count(); ++input) {
        NodeBundlePortId const target{handle, PortKind::sample, input};
        auto const channels = bundles.sample_input_channels(target);
        result.sample.push_back({
            target, virtual_node.id, member,
            bundles.resolve_sample_input(target).config,
            std::ranges::any_of(channels, [&](auto c) { return sample_input_is_connected(c); })});
      }
      for (size_t input = 0; input < bundle.event_input_count(); ++input) {
        NodeBundlePortId const target{handle, PortKind::event, input};
        auto const ports = bundles.event_input_ports(target);
        result.event.push_back({
            target, virtual_node.id, member,
            bundles.resolve_event_input(target).config,
            std::ranges::any_of(ports, [&](auto p) { return event_input_is_connected(p); })});
      }
    }
  }
  return result;
}

GraphBuilderVirtualSampleInputFamilies
GraphBuilderConnections::collect_virtual_sample_input_families(
    GraphBuilderNodeBundles const& bundles,
    GraphBuilderVirtualNodes const& virtual_nodes) const {
  GraphBuilderVirtualSampleInputFamilies result;
  for (auto const& virtual_node : virtual_nodes.records()) {
    for (auto const& mapping : virtual_node.sample_inputs) {
      if (mapping.channels.empty()) continue;
      auto const first = mapping.channels.front();
      auto config = bundles.resolve_sample_input(
          {first.bundle, PortKind::sample, first.port}).config;
      config.channel_layout = mapping.channel_layout;
      auto const channel_total = channel_count(mapping.channel_layout.channel_type);
      std::vector<GraphBuilderVirtualSampleInputChannel> channels(channel_total);
      for (auto const target : mapping.channels) {
        if (target.channel >= channel_total)
          details::error("virtual sample input channel does not match its declared layout");
        auto& channel = channels[target.channel];
        if (!std::ranges::contains(channel.targets, target)) channel.targets.push_back(target);
        channel.has_existing_connection |= sample_input_is_connected(target);
      }
      result.families.push_back({
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
    GraphBuilderNodeBundles const& bundles,
    GraphBuilderVirtualNodes const& virtual_nodes) const {
  GraphBuilderVirtualOutputs result;
  for (auto const& virtual_node : virtual_nodes.records()) {
    for (size_t member = 0; member < virtual_node.node_bundle_handles.size(); ++member) {
      auto const handle = virtual_node.node_bundle_handles[member];
      auto const& bundle = bundles.bundle(handle);
      for (size_t output = 0; output < bundle.sample_output_count(); ++output) {
        NodeBundlePortId const source{handle, PortKind::sample, output};
        auto const channels = bundles.sample_output_channels(source);
        result.sample.push_back({
            source, virtual_node.id, member,
            bundles.resolve_sample_output(source).config,
            std::ranges::any_of(channels,
                [&](auto c) { return sample_output_is_connected(c); })});
      }
      for (size_t output = 0; output < bundle.event_output_count(); ++output) {
        NodeBundlePortId const source{handle, PortKind::event, output};
        auto const ports = bundles.event_output_ports(source);
        result.event.push_back({
            source, virtual_node.id, member,
            bundles.resolve_event_output(source).config,
            std::ranges::any_of(ports,
                [&](auto p) { return event_output_is_connected(p); })});
      }
    }
  }
  return result;
}

GraphBuilderVirtualSampleOutputFamilies
GraphBuilderConnections::collect_virtual_sample_output_families(
    GraphBuilderNodeBundles const& bundles,
    GraphBuilderVirtualNodes const& virtual_nodes) const {
  GraphBuilderVirtualSampleOutputFamilies result;
  for (auto const& virtual_node : virtual_nodes.records()) {
    for (auto const& mapping : virtual_node.sample_outputs) {
      if (mapping.channels.empty()) continue;
      auto const first = mapping.channels.front();
      auto config = bundles.resolve_sample_output(
          {first.bundle, PortKind::sample, first.port}).config;
      config.channel_layout = mapping.channel_layout;
      auto const channel_total = channel_count(mapping.channel_layout.channel_type);
      std::vector<GraphBuilderVirtualSampleOutputChannel> channels(channel_total);
      for (auto const source : mapping.channels) {
        if (source.channel >= channel_total)
          details::error("virtual sample output channel does not match its declared layout");
        auto& channel = channels[source.channel];
        if (!std::ranges::contains(channel.sources, source)) channel.sources.push_back(source);
        channel.has_existing_downstream_connection |= sample_output_is_connected(source);
      }
      result.families.push_back({
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
    GraphBuilderConnections const& child, size_t bundle_offset) {
  for (auto connection : child._authored_sample_connections) {
    for (auto& channel : connection.source_channels) channel.bundle += bundle_offset;
    for (auto& channel : connection.target_channels) channel.bundle += bundle_offset;
    _authored_sample_connections.push_back(std::move(connection));
  }
  for (auto connection : child._authored_event_connections) {
    for (auto& source : connection.sources) source.bundle += bundle_offset;
    for (auto& target : connection.targets) target.bundle += bundle_offset;
    _authored_event_connections.push_back(std::move(connection));
  }
}
} // namespace iv
