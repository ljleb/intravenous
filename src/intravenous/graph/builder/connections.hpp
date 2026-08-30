#pragma once

#include <intravenous/graph/builder/port_refs.h>
#include <intravenous/graph/builder/virtual_nodes.hpp>
#include <intravenous/channel_layout.h>

#include <algorithm>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace iv {
class GraphBuilderNodeBundles;
class GraphBuilderVirtualNodes;

struct GraphBuilderVacantSampleInput {
  NodeBundlePortId target{};
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  InputConfig config{};
};
struct GraphBuilderVacantEventInput {
  NodeBundlePortId target{};
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  EventInputConfig config{};
};
struct GraphBuilderVacantInputs {
  std::vector<GraphBuilderVacantSampleInput> sample{};
  std::vector<GraphBuilderVacantEventInput> event{};
};

struct GraphBuilderVirtualSampleInput {
  NodeBundlePortId target{};
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  InputConfig config{};
  bool has_existing_connection = false;
};
struct GraphBuilderVirtualEventInput {
  NodeBundlePortId target{};
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  EventInputConfig config{};
  bool has_existing_connection = false;
};
struct GraphBuilderVirtualInputs {
  std::vector<GraphBuilderVirtualSampleInput> sample{};
  std::vector<GraphBuilderVirtualEventInput> event{};
};

struct GraphBuilderVirtualSampleInputChannel {
  std::vector<SampleInputChannelId> targets{};
  bool has_existing_connection = false;
};
struct GraphBuilderVirtualSampleInputFamily {
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  size_t family_ordinal = 0;
  std::string family_name{};
  InputConfig config{};
  ChannelTypeId channel_type = ChannelTypeId::mono;
  std::vector<GraphBuilderVirtualSampleInputChannel> channels{};
};
struct GraphBuilderVirtualSampleInputFamilies {
  std::vector<GraphBuilderVirtualSampleInputFamily> families{};
};

struct GraphBuilderVirtualSampleOutput {
  NodeBundlePortId source{};
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  OutputConfig config{};
  bool has_existing_downstream_connection = false;
};
struct GraphBuilderVirtualEventOutput {
  NodeBundlePortId source{};
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  EventOutputConfig config{};
  bool has_existing_downstream_connection = false;
};
struct GraphBuilderVirtualOutputs {
  std::vector<GraphBuilderVirtualSampleOutput> sample{};
  std::vector<GraphBuilderVirtualEventOutput> event{};
};

struct GraphBuilderVirtualSampleOutputChannel {
  std::vector<SampleOutputChannelId> sources{};
  bool has_existing_downstream_connection = false;
};
struct GraphBuilderVirtualSampleOutputFamily {
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  size_t family_ordinal = 0;
  std::string family_name{};
  OutputConfig config{};
  ChannelTypeId channel_type = ChannelTypeId::mono;
  std::vector<GraphBuilderVirtualSampleOutputChannel> channels{};
};
struct GraphBuilderVirtualSampleOutputFamilies {
  std::vector<GraphBuilderVirtualSampleOutputFamily> families{};
};

struct AuthoredSampleConnection {
  ChannelTypeId source_type = ChannelTypeId::mono;
  std::vector<SampleOutputChannelId> source_channels{};
  ChannelTypeId target_type = ChannelTypeId::mono;
  std::vector<SampleInputChannelId> target_channels{};
  bool operator==(AuthoredSampleConnection const&) const = default;
};

struct SampleLoweringGroup {
  NodeBundlePortId target{};
  std::vector<AuthoredSampleConnection const*> connections{};
};

struct SampleLoweringPlan {
  std::vector<SampleLoweringGroup> groups{};
};

struct AuthoredEventConnection {
  EventTypeId source_type = EventTypeId::empty;
  std::vector<EventOutputPortId> sources{};
  EventTypeId target_type = EventTypeId::empty;
  std::vector<EventInputPortId> targets{};
  bool operator==(AuthoredEventConnection const&) const = default;
};

class GraphBuilderConnections {
public:
  constexpr void record_authored_sample_connection(
      AuthoredSampleConnection connection)
  {
    if (connection.source_channels.size()
        != channel_count(connection.source_type))
      details::error(
          "authored sample connection source does not match its channel type");
    if (connection.target_channels.size()
        != channel_count(connection.target_type))
      details::error(
          "authored sample connection target does not match its channel type");
    _authored_sample_connections.push_back(std::move(connection));
  }
  constexpr std::span<AuthoredSampleConnection const>
      authored_sample_connections() const;
  constexpr SampleLoweringPlan sample_lowering_plan() const;
  constexpr void record_authored_event_connection(AuthoredEventConnection);
  constexpr std::span<AuthoredEventConnection const>
      authored_event_connections() const;

  constexpr bool sample_input_is_connected(SampleInputChannelId) const;
  constexpr bool sample_output_is_connected(SampleOutputChannelId) const;
  constexpr bool event_input_is_connected(EventInputPortId) const;
  constexpr bool event_output_is_connected(EventOutputPortId) const;

  constexpr GraphBuilderVacantInputs collect_vacant_inputs(
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&) const;
  constexpr GraphBuilderVirtualInputs collect_virtual_inputs(
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&) const;
  constexpr GraphBuilderVirtualSampleInputFamilies
      collect_virtual_sample_input_families(
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&) const;
  constexpr GraphBuilderVirtualOutputs collect_virtual_outputs(
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&) const;
  constexpr GraphBuilderVirtualSampleOutputFamilies
      collect_virtual_sample_output_families(
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&) const;

  constexpr void import_child(
      GraphBuilderConnections const&, size_t node_bundle_offset);

private:
  std::vector<AuthoredSampleConnection> _authored_sample_connections{};
  std::vector<AuthoredEventConnection> _authored_event_connections{};
};
} // namespace iv
namespace iv {
constexpr bool GraphBuilderConnections::sample_input_is_connected(
    SampleInputChannelId target) const {
  return std::ranges::any_of(_authored_sample_connections,
      [&](auto const& c) { return std::ranges::contains(c.target_channels, target); });
}
constexpr bool GraphBuilderConnections::sample_output_is_connected(
    SampleOutputChannelId source) const {
  return std::ranges::any_of(_authored_sample_connections,
      [&](auto const& c) { return std::ranges::contains(c.source_channels, source); });
}
constexpr bool GraphBuilderConnections::event_input_is_connected(
    EventInputPortId target) const {
  return std::ranges::any_of(_authored_event_connections,
      [&](auto const& c) { return std::ranges::contains(c.targets, target); });
}
constexpr bool GraphBuilderConnections::event_output_is_connected(
    EventOutputPortId source) const {
  return std::ranges::any_of(_authored_event_connections,
      [&](auto const& c) { return std::ranges::contains(c.sources, source); });
}

constexpr std::span<AuthoredSampleConnection const>
GraphBuilderConnections::authored_sample_connections() const {
  return _authored_sample_connections;
}
constexpr SampleLoweringPlan GraphBuilderConnections::sample_lowering_plan() const {
  SampleLoweringPlan plan;
  for (auto const& connection : _authored_sample_connections) {
    if (connection.target_channels.empty())
      details::error("sample connection has no target");
    auto const first = connection.target_channels.front();
    if (!std::ranges::all_of(connection.target_channels, [&](auto target) {
          return target.bundle == first.bundle && target.port == first.port;
        }))
      details::error("sample target spans bundle ports");
    NodeBundlePortId const target{first.bundle, PortKind::sample, first.port};
    auto group = std::find_if(
        plan.groups.begin(), plan.groups.end(), [&](auto const& candidate) {
          return candidate.target == target;
        });
    if (group == plan.groups.end()) {
      plan.groups.push_back({target, {}});
      group = std::prev(plan.groups.end());
    }
    group->connections.push_back(&connection);
  }
  return plan;
}
constexpr void GraphBuilderConnections::record_authored_event_connection(
    AuthoredEventConnection connection) {
  if (connection.sources.empty()) details::error("authored event connection has no source");
  if (connection.targets.empty()) details::error("authored event connection has no target");
  _authored_event_connections.push_back(std::move(connection));
}
constexpr std::span<AuthoredEventConnection const>
GraphBuilderConnections::authored_event_connections() const {
  return _authored_event_connections;
}

constexpr GraphBuilderVacantInputs
GraphBuilderConnections::collect_vacant_inputs(
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

constexpr GraphBuilderVirtualInputs
GraphBuilderConnections::collect_virtual_inputs(
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

constexpr GraphBuilderVirtualSampleInputFamilies
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

constexpr GraphBuilderVirtualOutputs
GraphBuilderConnections::collect_virtual_outputs(
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

constexpr GraphBuilderVirtualSampleOutputFamilies
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

constexpr void GraphBuilderConnections::import_child(
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
