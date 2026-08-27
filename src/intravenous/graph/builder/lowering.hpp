#pragma once

#include <intravenous/graph/builder/topology.h>
#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/graph/builder/connections.hpp>
#include <intravenous/graph/builder/detach.hpp>
#include <intravenous/graph/builder/public_ports.hpp>
#include <intravenous/graph/builder/virtual_nodes.hpp>
#include <intravenous/graph/connection_node.hpp>
#include <intravenous/graph/runtime_binding_nodes.hpp>

#include <algorithm>
#include <flat_map>
#include <flat_set>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace iv {
class GraphBuilderConnections;
class GraphBuilderPublicPorts;
class GraphBuilderDetach;
class GraphBuilderVirtualNodes;

struct LoweredNodeBundleProjection {
  std::vector<std::vector<TopologyPortId>> sample_inputs{};
  std::vector<std::vector<TopologyPortId>> sample_outputs{};
  std::vector<std::vector<TopologyPortId>> event_inputs{};
  std::vector<std::vector<TopologyPortId>> event_outputs{};
  std::optional<size_t> topology_node{};
};

struct DetachedSamplePortInfo {
  size_t detach_id = 0;
  TopologyPortId original_source{};
  size_t writer_node = std::numeric_limits<size_t>::max();
  TopologyPortId reader_output{};
  size_t loop_extra_latency = 1;
};

struct LoweredBuilderGraph {
  LoweredTopology topology{};
  std::vector<LoweredNodeBundleProjection> bundle_projections{};
  std::vector<std::optional<NodeBundleHandle>> bundle_by_lowered_node{};
  std::flat_map<TopologyPortId, TopologyPortId>
      subgraph_input_of_boundary_source{};
  std::flat_map<TopologyPortId, TopologyPortId>
      subgraph_event_input_of_boundary_source{};
  std::flat_map<TopologyPortId, DetachedSamplePortInfo>
      detached_info_by_source{};
  std::flat_set<TopologyPortId> detached_reader_outputs{};
};

class GraphBuilderLowering {
public:
  static constexpr LoweredBuilderGraph lower(
      GraphBuilderNodeBundles const&, GraphBuilderConnections const&,
      GraphBuilderPublicPorts const&, GraphBuilderVirtualNodes const&,
      GraphBuilderDetach const&,
      bool execution_root = false);
};

namespace {
class Lowerer {
  GraphBuilderNodeBundles const& bundles;
  GraphBuilderConnections const& connections;
  GraphBuilderPublicPorts const& public_ports;
  GraphBuilderVirtualNodes const& virtuals;
  GraphBuilderDetach const& detach;
  bool execution_root = false;
  LoweredBuilderGraph out;
  std::flat_map<NodeBundleHandle, size_t> subgraph_by_boundary;
  std::vector<std::pair<NodeBundlePortId, TopologyPortId>>
      materialized_event_output_ports;

  constexpr size_t append_generated(ConcreteNode node) {
    auto const index = out.topology.append_node(std::move(node));
    if (out.bundle_by_lowered_node.size() <= index)
      out.bundle_by_lowered_node.resize(index + 1);
    out.bundle_by_lowered_node[index] = std::nullopt;
    return index;
  }

  constexpr void project_bundles() {
    out.bundle_projections.resize(bundles.size());
    auto const root_boundary = public_ports.boundary_handle();
    for (NodeBundleHandle handle=0; handle<bundles.size(); ++handle) {
      auto const& bundle = bundles.bundle(handle);
      auto& p = out.bundle_projections[handle];
      if (bundle.is_concrete()) {
        auto node = out.topology.append_node(bundles.lowered_concrete(handle));
        p.topology_node = node;
        if (out.bundle_by_lowered_node.size() <= node) out.bundle_by_lowered_node.resize(node+1);
        out.bundle_by_lowered_node[node] = handle;
        for(size_t i=0;i<bundle.sample_input_count();++i)p.sample_inputs.push_back({{node,i}});
        for(size_t i=0;i<bundle.sample_output_count();++i)p.sample_outputs.push_back({{node,i}});
        for(size_t i=0;i<bundle.event_input_count();++i)p.event_inputs.push_back({{node,i}});
        for(size_t i=0;i<bundle.event_output_count();++i)p.event_outputs.push_back({{node,i}});
      } else if (bundle.is_tiled()) {
        auto members = bundle.tiled_members();
        for (auto const member : members) {
          auto const member_node = out.bundle_projections.at(member).topology_node;
          if (!member_node) details::error("tiled member has no lowered concrete node");
          out.bundle_by_lowered_node.at(*member_node) = handle;
        }
        for(size_t port=0;port<bundle.sample_input_count();++port){
          std::vector<TopologyPortId> endpoints;
          for(auto member:members) endpoints.push_back(out.bundle_projections.at(member).sample_inputs.at(port).at(0));
          p.sample_inputs.push_back(std::move(endpoints));
        }
        for(size_t port=0;port<bundle.sample_output_count();++port){
          std::vector<TopologyPortId> endpoints;
          for(auto member:members) endpoints.push_back(out.bundle_projections.at(member).sample_outputs.at(port).at(0));
          p.sample_outputs.push_back(std::move(endpoints));
        }
        for(size_t port=0;port<bundle.event_input_count();++port){
          std::vector<TopologyPortId> endpoints;
          for(auto member:members) endpoints.push_back(out.bundle_projections.at(member).event_inputs.at(port).at(0));
          p.event_inputs.push_back(std::move(endpoints));
        }
        for(size_t port=0;port<bundle.event_output_count();++port){
          std::vector<TopologyPortId> endpoints;
          for(auto member:members) endpoints.push_back(out.bundle_projections.at(member).event_outputs.at(port).at(0));
          p.event_outputs.push_back(std::move(endpoints));
        }
      } else if (bundle.is_boundary()) {
        if (handle == root_boundary) {
          for(size_t i=0;i<bundle.sample_input_count();++i)p.sample_inputs.push_back({{GRAPH_ID,i}});
          for(size_t i=0;i<bundle.sample_output_count();++i)p.sample_outputs.push_back({{GRAPH_ID,i}});
          for(size_t i=0;i<bundle.event_input_count();++i)p.event_inputs.push_back({{GRAPH_ID,i}});
          for(size_t i=0;i<bundle.event_output_count();++i)p.event_outputs.push_back({{GRAPH_ID,i}});
        } else {
          p.sample_inputs.resize(bundle.sample_input_count());
          p.event_inputs.resize(bundle.event_input_count());
          for(size_t i=0;i<bundle.sample_output_count();++i) {
            auto config=bundles.resolve_sample_output({handle,PortKind::sample,i}).config;
            p.sample_outputs.push_back({out.topology.append_scope_sample_input(config)});
          }
          for(size_t i=0;i<bundle.event_output_count();++i) {
            auto config=bundles.resolve_event_output({handle,PortKind::event,i}).config;
            p.event_outputs.push_back({out.topology.append_scope_event_input(config)});
          }
        }
      } else if (bundle.is_subgraph()) {
        auto info=bundles.subgraph_info(handle);
        auto const& boundary=bundles.bundle(info.boundary);
        size_t begin=out.topology.node_count(), end=begin;
        bool found=false;
        for(size_t child=info.child_begin; child<info.child_begin+info.child_count; ++child) {
          if (auto n=out.bundle_projections[child].topology_node) {
            if(!found){begin=*n;found=true;} end=std::max(end,*n+1);
          }
        }
        if(!found) begin=out.topology.node_count(), end=begin;
        auto node=out.topology.append_lowered_subgraph_node(
            info.kind,
            std::vector<InputConfig>(boundary.boundary_sample_inputs().begin(),boundary.boundary_sample_inputs().end()),
            std::vector<OutputConfig>(boundary.boundary_sample_outputs().begin(),boundary.boundary_sample_outputs().end()),
            std::vector<EventInputConfig>(boundary.boundary_event_inputs().begin(),boundary.boundary_event_inputs().end()),
            std::vector<EventOutputConfig>(boundary.boundary_event_outputs().begin(),boundary.boundary_event_outputs().end()),
            begin,end-begin,
            std::vector<std::vector<TopologyPortId>>(boundary.boundary_sample_inputs().size()),
            std::vector<TopologyPortId>(boundary.boundary_sample_outputs().size()),
            std::vector<std::vector<TopologyPortId>>(boundary.boundary_event_inputs().size()),
            std::vector<TopologyPortId>(boundary.boundary_event_outputs().size()));
        out.topology.subgraph_node(node).lifetime=info.lifetime;
        p.topology_node=node;
        if(out.bundle_by_lowered_node.size()<=node)out.bundle_by_lowered_node.resize(node+1);
        out.bundle_by_lowered_node[node]=handle;
        for(size_t i=0;i<bundle.sample_input_count();++i)p.sample_inputs.push_back({{node,i}});
        for(size_t i=0;i<bundle.sample_output_count();++i)p.sample_outputs.push_back({{node,i}});
        for(size_t i=0;i<bundle.event_input_count();++i)p.event_inputs.push_back({{node,i}});
        for(size_t i=0;i<bundle.event_output_count();++i)p.event_outputs.push_back({{node,i}});
        if (subgraph_by_boundary.contains(info.boundary))
          details::error("boundary belongs to more than one subgraph");
        subgraph_by_boundary.emplace(info.boundary, node);
        auto const& bp=out.bundle_projections[info.boundary];
        for(size_t i=0;i<bp.sample_outputs.size();++i)
          if(!bp.sample_outputs[i].empty())out.subgraph_input_of_boundary_source.emplace(bp.sample_outputs[i].front(),TopologyPortId{node,i});
        for(size_t i=0;i<bp.event_outputs.size();++i)
          if(!bp.event_outputs[i].empty())out.subgraph_event_input_of_boundary_source.emplace(bp.event_outputs[i].front(),TopologyPortId{node,i});
      }
    }
  }

  struct ResolvedSampleSourceChannel {
    TopologyPortId port{};
    OutputConfig config{};
    size_t channel = 0;
  };

  struct ResolvedSampleTargetChannel {
    std::optional<TopologyPortId> port{};
    InputConfig config{};
    size_t channel = 0;
  };

  constexpr ResolvedSampleSourceChannel resolve_sample_source_channel(
      SampleOutputChannelId channel) const {
    NodeBundlePortId logical{channel.bundle, PortKind::sample, channel.port};
    auto const config = bundles.resolve_sample_output(logical).config;
    auto const count = channel_count(config.channel_layout.channel_type);
    auto const& endpoints = out.bundle_projections.at(channel.bundle).sample_outputs.at(channel.port);
    if (channel.channel >= count) details::error("sample source channel out of bounds");
    if (endpoints.size() == 1) return {endpoints.front(), config, channel.channel};
    if (endpoints.size() == count && bundles.bundle(channel.bundle).is_tiled()) {
      auto const member = bundles.tiled_member(channel.bundle, channel.channel);
      auto const member_config = bundles.resolve_sample_output({member, PortKind::sample, channel.port}).config;
      if (channel_count(member_config.channel_layout.channel_type) != 1)
        details::error("tiled sample source member must expose one concrete channel");
      return {endpoints[channel.channel], member_config, 0};
    }
    details::error("sample source has invalid lowered endpoint count");
  }

  constexpr ResolvedSampleTargetChannel resolve_sample_target_channel(
      SampleInputChannelId channel) const {
    NodeBundlePortId logical{channel.bundle, PortKind::sample, channel.port};
    auto const config = bundles.resolve_sample_input(logical).config;
    auto const count = channel_count(config.channel_layout.channel_type);
    auto const& endpoints = out.bundle_projections.at(channel.bundle).sample_inputs.at(channel.port);
    if (channel.channel >= count) details::error("sample target channel out of bounds");
    if (endpoints.empty()) {
      if (!subgraph_by_boundary.contains(channel.bundle)) details::error("sample target has no lowered endpoint");
      return {std::nullopt, config, channel.channel};
    }
    if (endpoints.size() == 1) return {endpoints.front(), config, channel.channel};
    if (endpoints.size() == count && bundles.bundle(channel.bundle).is_tiled()) {
      auto const member = bundles.tiled_member(channel.bundle, channel.channel);
      auto const member_config = bundles.resolve_sample_input({member, PortKind::sample, channel.port}).config;
      if (channel_count(member_config.channel_layout.channel_type) != 1)
        details::error("tiled sample target member must expose one concrete channel");
      return {endpoints[channel.channel], member_config, 0};
    }
    details::error("sample target has invalid lowered endpoint count");
  }

  constexpr std::optional<TopologyPortId> direct_sample_source(
      ChannelTypeId type, std::span<SampleOutputChannelId const> channels) const {
    if (channels.empty()) details::error("sample source has no channels");
    if (auto logical = bundles.sample_output_port_for_channels(type, channels)) {
      auto const& endpoints = out.bundle_projections.at(logical->node_bundle_handle).sample_outputs.at(logical->port_ordinal);
      if (endpoints.size() == 1) return endpoints.front();
    }
    if (type == ChannelTypeId::mono && channels.size() == 1) {
      auto const resolved = resolve_sample_source_channel(channels.front());
      if (resolved.channel == 0 && channel_count(resolved.config.channel_layout.channel_type) == 1)
        return resolved.port;
    }
    return std::nullopt;
  }

  constexpr ConcreteNode make_connection_node(
      std::vector<ConnectionNodeInputConfig> inputs,
      std::vector<ConnectionNodeEphemeralPortConfig> ephemeral_ports,
      OutputConfig output, Sample default_value,
      std::string runtime_binding_id = {},
      size_t runtime_source_channel_offset = 0) const {
    std::vector<InputConfig> input_configs;
    input_configs.reserve(inputs.size());
    for (auto const& input : inputs) input_configs.push_back(input.input);
    std::vector<OutputConfig> output_configs{output};
    return ConcreteNode{
        .ports = NodePorts{
            .sample_inputs = std::vector<InputConfig>(input_configs.begin(), input_configs.end()),
            .sample_outputs = std::vector<OutputConfig>(output_configs.begin(), output_configs.end()),
        },
        .type_identity = NodeTypeIdentity{.value = "iv::ConnectionNode"},
        .generated_node = ConnectionNodeSpec{
            .input_configs = std::move(inputs),
            .ephemeral_port_configs = std::move(ephemeral_ports),
            .output_config = std::move(output),
            .default_value = default_value,
            .runtime_binding_id = std::move(runtime_binding_id),
            .runtime_source_channel_offset = runtime_source_channel_offset,
        },
    };
  }

  template<class Spec>
  constexpr ConcreteNode make_generated_node(Spec spec, std::string type_identity) const {
    auto inputs = get_inputs(spec);
    auto outputs = get_outputs(spec);
    auto event_inputs = get_event_inputs(spec);
    auto event_outputs = get_event_outputs(spec);
    return ConcreteNode{
        .ports = NodePorts{
            .sample_inputs = std::vector<InputConfig>(inputs.begin(), inputs.end()),
            .sample_outputs = std::vector<OutputConfig>(outputs.begin(), outputs.end()),
            .event_input_configs = std::vector<EventInputConfig>(event_inputs.begin(), event_inputs.end()),
            .event_output_configs = std::vector<EventOutputConfig>(event_outputs.begin(), event_outputs.end()),
        },
        .type_identity = NodeTypeIdentity{.value = std::move(type_identity)},
        .generated_node = std::move(spec),
    };
  }

  struct SampleGroup {
    NodeBundlePortId target{};
    std::vector<AuthoredSampleConnection const*> connections{};
  };

  constexpr std::vector<SampleGroup> sample_groups() const {
    std::vector<SampleGroup> groups;
    for (auto const& c : connections.authored_sample_connections()) {
      if (c.target_channels.empty()) details::error("sample connection has no target");
      auto first = c.target_channels.front();
      if (!std::ranges::all_of(c.target_channels, [&](auto x) { return x.bundle == first.bundle && x.port == first.port; }))
        details::error("sample target spans bundle ports");
      NodeBundlePortId target{first.bundle, PortKind::sample, first.port};
      auto it = std::find_if(groups.begin(), groups.end(), [&](auto const& g) { return g.target == target; });
      if (it == groups.end()) {
        groups.push_back({target, {}});
        it = std::prev(groups.end());
      }
      it->connections.push_back(&c);
    }
    return groups;
  }

  struct LoweredConnection {
    TopologyPortId output{};
    std::optional<TopologyPortId> target{};
  };

  struct RuntimeSampleBindingRef {
    std::string binding_id{};
    size_t source_channel_offset = 0;
  };

  constexpr RuntimeSampleBindingRef runtime_sample_binding_for(
      NodeBundlePortId semantic_target,
      std::optional<TopologyPortId> concrete_target) const {
    auto const virtual_ports = virtuals.ports(bundles);
    for (auto const& input : virtual_ports.sample_inputs) {
      for (size_t member = 0; member < input.node_bundle_ports.size(); ++member) {
        auto const target = input.node_bundle_ports[member];
        if (target.port_kind != PortKind::sample) continue;
        if (target != semantic_target) {
          auto const& endpoints = out.bundle_projections.at(target.node_bundle_handle).sample_inputs.at(target.port_ordinal);
          if (!concrete_target || !std::ranges::contains(endpoints, *concrete_target)) continue;
        }
        size_t source_channel_offset = 0;
        auto const& endpoints = out.bundle_projections.at(target.node_bundle_handle).sample_inputs.at(target.port_ordinal);
        if (concrete_target && endpoints.size() > 1) {
          auto const endpoint = std::ranges::find(endpoints, *concrete_target);
          if (endpoint == endpoints.end()) details::error("runtime sample target endpoint is not in its member");
          source_channel_offset = static_cast<size_t>(endpoint - endpoints.begin());
        }
        return {
            .binding_id = runtime_virtual_port_key(true, PortKind::sample, input.id.virtual_node_id, member, input.id.port_ordinal),
            .source_channel_offset = source_channel_offset,
        };
      }
    }
    return {};
  }

  constexpr bool has_runtime_sample_capability(
      NodeBundlePortId semantic_target,
      std::optional<TopologyPortId> concrete_target) const {
    auto const virtual_ports = virtuals.ports(bundles);
    for (auto const& input : virtual_ports.sample_inputs) {
      for (auto const target : input.node_bundle_ports) {
        if (target == semantic_target) return true;
        auto const& endpoints = out.bundle_projections.at(target.node_bundle_handle).sample_inputs.at(target.port_ordinal);
        if (concrete_target && std::ranges::contains(endpoints, *concrete_target)) return true;
      }
    }
    return false;
  }

  constexpr LoweredConnection lower_connection_node(
      SampleGroup const& group,
      std::optional<TopologyPortId> target_endpoint) {
    std::vector<TopologyPortId> input_sources;
    std::vector<ConnectionNodeInputConfig> input_configs;
    std::vector<ConnectionNodeEphemeralPortConfig> ephemeral_configs;
    InputConfig target_config{};
    if (!group.connections.empty())
      target_config = bundles.resolve_sample_input(group.target).config;
    else if (target_endpoint && target_endpoint->node < out.topology.node_count() && !out.topology.is_subgraph_node(target_endpoint->node))
      target_config = out.topology.concrete_node(target_endpoint->node).ports.sample_inputs.at(target_endpoint->port);
    else
      details::error("vacant ConnectionNode has no concrete target config");
    if (target_endpoint) {
      bool found = false;
      for (auto const* connection : group.connections) {
        for (auto const target_channel : connection->target_channels) {
          auto const target = resolve_sample_target_channel(target_channel);
          if (target.port == target_endpoint) {
            target_config = target.config;
            found = true;
            break;
          }
        }
        if (found) break;
      }
      if (!found && target_endpoint->node < out.topology.node_count() && !out.topology.is_subgraph_node(target_endpoint->node))
        target_config = out.topology.concrete_node(target_endpoint->node).ports.sample_inputs.at(target_endpoint->port);
    }
    auto projection_input = [&](ResolvedSampleSourceChannel const& source) {
      auto it = std::ranges::find(input_sources, source.port);
      if (it != input_sources.end()) return static_cast<size_t>(it - input_sources.begin());
      input_sources.push_back(source.port);
      input_configs.push_back(ConnectionNodeInputConfig{.input = InputConfig{.channel_layout = source.config.channel_layout}});
      return input_sources.size() - 1;
    };
    for (auto const* connection : group.connections) {
      if (connection->source_channels.size() != channel_count(connection->source_type))
        details::error("sample source channel count does not match its semantic type");
      if (connection->target_channels.size() != channel_count(connection->target_type))
        details::error("sample target channel count does not match its semantic type");
      auto const ephemeral = ephemeral_configs.size();
      ConnectionNodeEphemeralPortConfig ephemeral_config{
          .channel_layout = ChannelLayout{.channel_type = connection->source_type, .sample_layout = SampleStreamLayout::planar},
          .conversion = ChannelConversionRegistry::plan(
              ChannelLayout{.channel_type = connection->source_type, .sample_layout = SampleStreamLayout::planar},
              ChannelLayout{.channel_type = connection->target_type, .sample_layout = SampleStreamLayout::planar}),
      };
      for (size_t source_i = 0; source_i < connection->source_channels.size(); ++source_i) {
        auto const source = resolve_sample_source_channel(connection->source_channels[source_i]);
        auto const input = projection_input(source);
        input_configs[input].channel_copies.push_back({.input_channel = source.channel, .ephemeral_port = ephemeral, .ephemeral_channel = source_i});
      }
      for (size_t target_i = 0; target_i < connection->target_channels.size(); ++target_i) {
        auto const target = resolve_sample_target_channel(connection->target_channels[target_i]);
        if (target.port == target_endpoint)
          ephemeral_config.output_channel_copies.push_back({.converted_channel = target_i, .output_channel = target.channel});
      }
      if (!ephemeral_config.output_channel_copies.empty())
        ephemeral_configs.push_back(std::move(ephemeral_config));
      else
        for (auto& input : input_configs)
          std::erase_if(input.channel_copies, [&](auto const& copy) { return copy.ephemeral_port == ephemeral; });
    }
    std::vector<TopologyPortId> used_input_sources;
    std::vector<ConnectionNodeInputConfig> used_input_configs;
    for (size_t input = 0; input < input_configs.size(); ++input) {
      if (input_configs[input].channel_copies.empty()) continue;
      used_input_sources.push_back(input_sources[input]);
      used_input_configs.push_back(std::move(input_configs[input]));
    }
    auto const runtime_binding = runtime_sample_binding_for(group.target, target_endpoint);
    auto const node = append_generated(make_connection_node(
        std::move(used_input_configs), std::move(ephemeral_configs),
        OutputConfig{.name = target_config.name, .channel_layout = target_config.channel_layout},
        target_config.default_value, runtime_binding.binding_id, runtime_binding.source_channel_offset));
    for (size_t input = 0; input < used_input_sources.size(); ++input)
      out.topology.add_sample_edge({used_input_sources[input], {node, input}});
    TopologyPortId const source{node, 0};
    if (target_endpoint) out.topology.add_sample_edge({source, *target_endpoint});
    return {.output = source, .target = target_endpoint};
  }

  constexpr bool lower_tiled_group_direct(SampleGroup const& group, std::span<TopologyPortId const> endpoints) {
    auto const target_channels = bundles.sample_input_channels(group.target);
    auto const target_type = bundles.resolve_sample_input(group.target).config.channel_layout.channel_type;
    if (group.connections.size() == 1) {
      auto const& connection = *group.connections.front();
      bool const whole_target = connection.target_type == target_type && connection.target_channels == target_channels;
      if (whole_target && connection.source_type == ChannelTypeId::mono) {
        if (auto source = direct_sample_source(connection.source_type, connection.source_channels)) {
          for (auto const target : endpoints) out.topology.add_sample_edge({*source, target});
          return true;
        }
      }
      if (whole_target && connection.source_type == target_type && connection.source_channels.size() == endpoints.size()) {
        std::vector<TopologyPortId> sources;
        sources.reserve(connection.source_channels.size());
        for (auto const source_channel : connection.source_channels) {
          auto const resolved = resolve_sample_source_channel(source_channel);
          if (resolved.channel != 0 || channel_count(resolved.config.channel_layout.channel_type) != 1) return false;
          sources.push_back(resolved.port);
        }
        for (size_t channel = 0; channel < endpoints.size(); ++channel) out.topology.add_sample_edge({sources[channel], endpoints[channel]});
        return true;
      }
    }
    std::vector<std::pair<TopologyPortId, TopologyPortId>> edges;
    std::vector<bool> claimed(endpoints.size(), false);
    for (auto const* connection : group.connections) {
      if (connection->target_type != ChannelTypeId::mono || connection->target_channels.size() != 1) return false;
      auto it = std::ranges::find(target_channels, connection->target_channels.front());
      if (it == target_channels.end()) details::error("partial target not in tiled input");
      auto const channel = static_cast<size_t>(it - target_channels.begin());
      if (claimed[channel]) details::error("sample target channel has multiple sources");
      auto source = direct_sample_source(connection->source_type, connection->source_channels);
      if (!source) return false;
      claimed[channel] = true;
      edges.push_back({*source, endpoints[channel]});
    }
    for (auto const& [source, target] : edges) out.topology.add_sample_edge({source, target});
    return !edges.empty();
  }

  constexpr void lower_samples() {
    std::vector<NodeBundlePortId> assigned_subgraph_outputs;
    std::vector<TopologyPortId> bound_targets;
    auto mark_bound = [&](TopologyPortId target) {
      if (!std::ranges::contains(bound_targets, target)) bound_targets.push_back(target);
    };
    auto const authored_topology_node_count = out.topology.node_count();
    auto groups = sample_groups();
    for (auto const& group : groups) {
      auto const& endpoints = out.bundle_projections.at(group.target.node_bundle_handle).sample_inputs.at(group.target.port_ordinal);
      auto const target_channels = bundles.sample_input_channels(group.target);
      auto const target_type = bundles.resolve_sample_input(group.target).config.channel_layout.channel_type;
      if (auto it = subgraph_by_boundary.find(group.target.node_bundle_handle); it != subgraph_by_boundary.end()) {
        auto const subgraph_node = it->second;
        if (group.target.port_ordinal >= out.topology.subgraph_node(subgraph_node).lowered_subgraph.sample_output_sources.size())
          details::error("subgraph sample output out of bounds");
        if (std::ranges::contains(assigned_subgraph_outputs, group.target)) details::error("subgraph sample output has more than one source");
        assigned_subgraph_outputs.push_back(group.target);
        std::optional<TopologyPortId> source;
        if (group.connections.size() == 1) {
          auto const& connection = *group.connections.front();
          if (connection.target_type == target_type && connection.target_channels == target_channels)
            source = direct_sample_source(connection.source_type, connection.source_channels);
        }
        if (!source) source = lower_connection_node(group, std::nullopt).output;
        out.topology.subgraph_node(subgraph_node).lowered_subgraph.sample_output_sources[group.target.port_ordinal] = *source;
        continue;
      }
      if (endpoints.empty()) details::error("sample target has no lowered endpoint");
      if (endpoints.size() == 1) {
        auto const runtime_capable = has_runtime_sample_capability(group.target, endpoints.front());
        if (group.connections.size() == 1) {
          auto const& connection = *group.connections.front();
          if (connection.target_type == target_type && connection.target_channels == target_channels) {
            if (auto source = direct_sample_source(connection.source_type, connection.source_channels)) {
              auto const source_config = resolve_sample_source_channel(connection.source_channels.front()).config;
              auto const target_config = resolve_sample_target_channel(connection.target_channels.front()).config;
              if (!runtime_capable && effective_channel_layout(source_config) == effective_channel_layout(target_config)) {
                out.topology.add_sample_edge({*source, endpoints.front()});
                mark_bound(endpoints.front());
                continue;
              }
            }
          }
        }
        lower_connection_node(group, endpoints.front());
        mark_bound(endpoints.front());
        continue;
      }
      auto const runtime_capable = std::ranges::any_of(endpoints, [&](auto endpoint) { return has_runtime_sample_capability(group.target, endpoint); });
      if (!runtime_capable && lower_tiled_group_direct(group, endpoints)) {
        for (auto const endpoint : endpoints) mark_bound(endpoint);
        continue;
      }
      for (auto const endpoint : endpoints) {
        lower_connection_node(group, endpoint);
        mark_bound(endpoint);
      }
    }
    for (size_t node = 0; node < authored_topology_node_count; ++node) {
      if (out.topology.is_subgraph_node(node)) continue;
      auto const inputs = out.topology.concrete_node(node).ports.sample_inputs;
      for (size_t input = 0; input < inputs.size(); ++input) {
        TopologyPortId const target{node, input};
        if (std::ranges::contains(bound_targets, target)) continue;
        SampleGroup vacant;
        lower_connection_node(vacant, target);
        mark_bound(target);
      }
    }
    for (auto const& [boundary, node] : subgraph_by_boundary) {
      auto& binding = out.topology.subgraph_node(node).lowered_subgraph;
      auto const& p = out.bundle_projections.at(boundary);
      for (size_t i = 0; i < p.sample_outputs.size(); ++i) {
        if (p.sample_outputs[i].empty()) continue;
        auto source = p.sample_outputs[i].front();
        out.topology.for_each_sample_edge([&](TopologyEdge const& edge) {
          if (edge.source == source && !std::ranges::contains(binding.sample_input_targets[i], edge.target)) binding.sample_input_targets[i].push_back(edge.target);
        });
      }
      for (size_t output = 0; output < binding.sample_output_sources.size(); ++output)
        if (!std::ranges::contains(assigned_subgraph_outputs, NodeBundlePortId{boundary, PortKind::sample, output}))
          details::error("subgraph sample output has no authored source");
    }
  }

  constexpr TopologyPortId materialize_event_output_port(
      NodeBundlePortId logical, EventTypeId type) {
    if (auto const existing = std::ranges::find_if(
            materialized_event_output_ports,
            [&](auto const& entry) { return entry.first == logical; });
        existing != materialized_event_output_ports.end())
      return existing->second;
    auto const& endpoints = out.bundle_projections.at(
        logical.node_bundle_handle).event_outputs.at(logical.port_ordinal);
    if (endpoints.empty())
      details::error("virtual event output member has no lowered endpoint");
    if (endpoints.size() == 1) return endpoints.front();
    auto const node = append_generated(
        GraphBuilderNodeBundles::make_concrete_node<EventConcatenation>(
            endpoints.size(), type));
    for (size_t input = 0; input < endpoints.size(); ++input)
      out.topology.add_event_edge({
          endpoints[input], {node, input},
          EventConversionRegistry::instance().plan(type, type)});
    return materialized_event_output_ports.emplace_back(
        logical, TopologyPortId{node, 0}).second;
  }

  constexpr TopologyPortId materialize_event_source(AuthoredEventConnection const& c) {
    std::vector<TopologyPortId> endpoints;
    for(auto source:c.sources) {
      NodeBundlePortId const logical{
          source.bundle, PortKind::event, source.port};
      auto const config=bundles.resolve_event_output(logical).config;
      if(config.type!=c.source_type)details::error("event source type changed before lowering");
      endpoints.push_back(materialize_event_output_port(logical, c.source_type));
    }
    if(endpoints.empty())details::error("event source has no lowered endpoint");
    if(endpoints.size()==1)return endpoints.front();
    auto node=append_generated(GraphBuilderNodeBundles::make_concrete_node<EventConcatenation>(endpoints.size(),c.source_type));
    for(size_t i=0;i<endpoints.size();++i)out.topology.add_event_edge({endpoints[i],{node,i},EventConversionRegistry::instance().plan(c.source_type,c.source_type)});
    return {node,0};
  }

  constexpr void lower_runtime_event_inputs() {
    auto const virtual_ports = virtuals.ports(bundles);
    for (auto const& input : virtual_ports.event_inputs) {
      for (size_t member = 0; member < input.node_bundle_ports.size(); ++member) {
        auto const target = input.node_bundle_ports[member];
        auto const node = append_generated(make_generated_node(
            RuntimeEventInputNodeSpec{.type = input.config.type, .binding_id = runtime_virtual_port_key(true, PortKind::event, input.id.virtual_node_id, member, input.id.port_ordinal)},
            "iv::RuntimeEventInputNode"));
        auto const& endpoints = out.bundle_projections.at(target.node_bundle_handle).event_inputs.at(target.port_ordinal);
        if (endpoints.empty()) details::error("runtime-capable event target has no lowered endpoint");
        for (auto const endpoint : endpoints)
          out.topology.add_event_edge({{node, 0}, endpoint, EventConversionRegistry::instance().plan(input.config.type, input.config.type)});
      }
    }
  }

  constexpr void lower_events() {
    std::vector<NodeBundlePortId> assigned_subgraph_outputs;
    for(auto const& c:connections.authored_event_connections()) {
      auto source=materialize_event_source(c);
      for(auto target:c.targets) {
        auto config=bundles.resolve_event_input({target.bundle,PortKind::event,target.port}).config;
        if(config.type!=c.target_type)details::error("event target type changed before lowering");
        if(auto it=subgraph_by_boundary.find(target.bundle);it!=subgraph_by_boundary.end()) {
          NodeBundlePortId const logical{target.bundle, PortKind::event, target.port};
          if (std::ranges::contains(assigned_subgraph_outputs, logical)) details::error("subgraph event output has more than one source");
          assigned_subgraph_outputs.push_back(logical);
          out.topology.subgraph_node(it->second).lowered_subgraph.event_output_sources.at(target.port)=source;
          continue;
        }
        auto const& endpoints=out.bundle_projections.at(target.bundle).event_inputs.at(target.port);
        if(endpoints.empty())details::error("event target has no lowered endpoint");
        for(auto endpoint:endpoints)out.topology.add_event_edge({source,endpoint,EventConversionRegistry::instance().plan(c.source_type,c.target_type)});
      }
    }
    lower_runtime_event_inputs();
    for(auto const& [boundary,node]:subgraph_by_boundary) {
      auto& binding=out.topology.subgraph_node(node).lowered_subgraph;
      auto const& p=out.bundle_projections.at(boundary);
      for(size_t i=0;i<p.event_outputs.size();++i) {
        if(p.event_outputs[i].empty())continue;
        auto source=p.event_outputs[i].front();
        out.topology.for_each_event_edge([&](TopologyEventEdge const& edge){if(edge.source==source&&!std::ranges::contains(binding.event_input_targets[i],edge.target))binding.event_input_targets[i].push_back(edge.target);});
      }
      for (size_t output = 0; output < binding.event_output_sources.size(); ++output)
        if (!std::ranges::contains(assigned_subgraph_outputs, NodeBundlePortId{boundary, PortKind::event, output})) details::error("subgraph event output has no authored source");
    }
  }

  constexpr void lower_detach() {
    for(auto const& info:detach.authored_infos()) {
      auto writer=out.bundle_projections.at(info.writer_bundle).topology_node;
      if(!writer)details::error("detach writer is not a concrete lowered node");
      std::optional<TopologyPortId> source;
      out.topology.for_each_sample_edge([&](TopologyEdge const& edge){if(edge.target==TopologyPortId{*writer,0}){if(source&&*source!=edge.source)details::error("detach writer has multiple sources");source=edge.source;}});
      if(!source)details::error("detach writer has no lowered source");
      auto const reader_channel = resolve_sample_source_channel(info.reader_channel);
      if (reader_channel.channel != 0 || channel_count(reader_channel.config.channel_layout.channel_type) != 1) details::error("detach reader output must be one concrete channel");
      auto const reader = reader_channel.port;
      out.detached_info_by_source.emplace(*source,DetachedSamplePortInfo{info.detach_id,*source,*writer,reader,info.loop_extra_latency});
      out.detached_reader_outputs.emplace(reader);
    }
  }

  constexpr TopologyPortId materialize_sample_output_channels(
      ChannelTypeId semantic_type,
      std::span<SampleOutputChannelId const> semantic_channels,
      OutputConfig output_config) {
    if (semantic_channels.size() != channel_count(semantic_type))
      details::error("virtual sample output member channel count changed");
    if (semantic_type != output_config.channel_layout.channel_type)
      details::error("virtual sample output member channel type changed");
    if (auto logical = bundles.sample_output_port_for_channels(
            semantic_type, semantic_channels)) {
      auto const& endpoints = out.bundle_projections.at(
          logical->node_bundle_handle).sample_outputs.at(logical->port_ordinal);
      auto const resolved_config = bundles.resolve_sample_output(*logical).config;
      if (endpoints.size() == 1
          && effective_channel_layout(resolved_config)
              == effective_channel_layout(output_config))
        return endpoints.front();
    }
    std::vector<TopologyPortId> input_sources;
    std::vector<ConnectionNodeInputConfig> input_configs;
    ConnectionNodeEphemeralPortConfig ephemeral{
        .channel_layout = ChannelLayout{.channel_type = output_config.channel_layout.channel_type, .sample_layout = SampleStreamLayout::planar},
        .conversion = ChannelConversionRegistry::plan(
            ChannelLayout{.channel_type = output_config.channel_layout.channel_type, .sample_layout = SampleStreamLayout::planar},
            output_config.channel_layout),
    };
    for (size_t channel = 0; channel < semantic_channels.size(); ++channel) {
      auto const resolved = resolve_sample_source_channel(semantic_channels[channel]);
      auto it = std::ranges::find(input_sources, resolved.port);
      size_t input = 0;
      if (it == input_sources.end()) {
        input = input_sources.size();
        input_sources.push_back(resolved.port);
        input_configs.push_back(ConnectionNodeInputConfig{.input = InputConfig{.channel_layout = resolved.config.channel_layout}});
      } else input = static_cast<size_t>(it - input_sources.begin());
      input_configs[input].channel_copies.push_back({.input_channel = resolved.channel, .ephemeral_port = 0, .ephemeral_channel = channel});
      ephemeral.output_channel_copies.push_back({.converted_channel = channel, .output_channel = channel});
    }
    auto const node = append_generated(make_connection_node(std::move(input_configs), std::vector<ConnectionNodeEphemeralPortConfig>{std::move(ephemeral)}, std::move(output_config), Sample{0.0f}));
    for (size_t input = 0; input < input_sources.size(); ++input) out.topology.add_sample_edge({input_sources[input], {node, input}});
    return {node, 0};
  }

  constexpr TopologyPortId materialize_sample_output_port(
      NodeBundlePortId logical, OutputConfig output_config) {
    auto const semantic_channels = bundles.sample_output_channels(logical);
    return materialize_sample_output_channels(
        bundles.resolve_sample_output(logical).config.channel_layout.channel_type,
        semantic_channels, std::move(output_config));
  }

  constexpr void lower_runtime_output_observers() {
    auto const virtual_ports = virtuals.ports(bundles);
    for (auto const& output : virtual_ports.sample_outputs) {
      std::vector<TopologyPortId> sources;
      std::vector<InputConfig> inputs;
      std::vector<std::string> member_binding_ids;
      sources.reserve(output.member_channels.size());
      inputs.reserve(output.member_channels.size());
      member_binding_ids.reserve(output.member_channels.size());
      for (size_t member = 0; member < output.member_channels.size(); ++member) {
        sources.push_back(materialize_sample_output_channels(
            output.config.channel_layout.channel_type,
            output.member_channels[member], output.config));
        inputs.push_back(InputConfig{.name = output.config.name, .channel_layout = output.config.channel_layout});
        member_binding_ids.push_back(runtime_virtual_port_key(false, PortKind::sample, output.id.virtual_node_id, member, output.id.port_ordinal));
      }
      auto const node = append_generated(make_generated_node(
          RuntimeSampleOutputFamilyNodeSpec{
              .input_configs = std::move(inputs),
              .member_binding_ids = std::move(member_binding_ids),
              .aggregate_binding_id = runtime_virtual_port_key(false, PortKind::sample, output.id.virtual_node_id, std::nullopt, output.id.port_ordinal),
          },
          "iv::RuntimeSampleOutputFamilyNode"));
      for (size_t input = 0; input < sources.size(); ++input) out.topology.add_sample_edge({sources[input], {node, input}});
    }
    for (auto const& output : virtual_ports.event_outputs) {
      std::vector<TopologyPortId> sources;
      std::vector<std::string> member_binding_ids;
      sources.reserve(output.node_bundle_ports.size());
      member_binding_ids.reserve(output.node_bundle_ports.size());
      for (size_t member = 0; member < output.node_bundle_ports.size(); ++member) {
        sources.push_back(materialize_event_output_port(output.node_bundle_ports[member], output.config.type));
        member_binding_ids.push_back(runtime_virtual_port_key(false, PortKind::event, output.id.virtual_node_id, member, output.id.port_ordinal));
      }
      auto const node = append_generated(make_generated_node(
          RuntimeEventOutputFamilyNodeSpec{
              .type = output.config.type,
              .member_count = sources.size(),
              .member_binding_ids = std::move(member_binding_ids),
              .aggregate_binding_id = runtime_virtual_port_key(false, PortKind::event, output.id.virtual_node_id, std::nullopt, output.id.port_ordinal),
          },
          "iv::RuntimeEventOutputFamilyNode"));
      for (size_t input = 0; input < sources.size(); ++input)
        out.topology.add_event_edge({sources[input], {node, input}, EventConversionRegistry::instance().plan(output.config.type, output.config.type)});
    }
  }

  constexpr void lower_execution_root_ports() {
    if (!execution_root) return;
    auto const sample_inputs = public_ports.sample_inputs(bundles);
    for (size_t port = 0; port < sample_inputs.size(); ++port) {
      auto const& input = sample_inputs[port];
      auto node = append_generated(make_generated_node(
          RuntimeSampleInputNodeSpec{
              .output = OutputConfig{.name = input.name, .channel_layout = input.channel_layout},
              .default_value = input.default_value,
              .binding_id = runtime_public_port_key(true, PortKind::sample, port),
          }, "iv::RuntimeSampleInputNode"));
      out.topology.replace_sample_source({GRAPH_ID, port}, {node, 0});
    }
    auto const event_inputs = public_ports.event_inputs(bundles);
    for (size_t port = 0; port < event_inputs.size(); ++port) {
      auto node = append_generated(make_generated_node(
          RuntimeEventInputNodeSpec{.type = event_inputs[port].type, .binding_id = runtime_public_port_key(true, PortKind::event, port)},
          "iv::RuntimeEventInputNode"));
      out.topology.replace_event_source({GRAPH_ID, port}, {node, 0});
    }
    auto const sample_outputs = public_ports.sample_outputs(bundles);
    for (size_t port = 0; port < sample_outputs.size(); ++port) {
      auto const& output = sample_outputs[port];
      auto node = append_generated(make_generated_node(
          RuntimeSampleOutputNodeSpec{
              .input = InputConfig{.name = output.name, .channel_layout = output.channel_layout},
              .binding_id = runtime_public_port_key(false, PortKind::sample, port),
          }, "iv::RuntimeSampleOutputNode"));
      out.topology.replace_sample_target({GRAPH_ID, port}, {node, 0});
    }
    auto const event_outputs = public_ports.event_outputs(bundles);
    for (size_t port = 0; port < event_outputs.size(); ++port) {
      auto node = append_generated(make_generated_node(
          RuntimeEventOutputNodeSpec{.type = event_outputs[port].type, .binding_id = runtime_public_port_key(false, PortKind::event, port)},
          "iv::RuntimeEventOutputNode"));
      out.topology.replace_event_target({GRAPH_ID, port}, {node, 0});
    }
  }

public:
  constexpr Lowerer(GraphBuilderNodeBundles const& b, GraphBuilderConnections const& c,
          GraphBuilderPublicPorts const& p, GraphBuilderVirtualNodes const& v,
          GraphBuilderDetach const& d, bool is_execution_root)
      : bundles(b),connections(c),public_ports(p),virtuals(v),detach(d),execution_root(is_execution_root) {}

  constexpr LoweredBuilderGraph run() {
    project_bundles();
    lower_samples();
    lower_events();
    lower_detach();
    lower_runtime_output_observers();
    lower_execution_root_ports();
    out.topology.normalize_edges();
    return std::move(out);
  }
};
} // namespace

constexpr LoweredBuilderGraph GraphBuilderLowering::lower(
    GraphBuilderNodeBundles const& bundles, GraphBuilderConnections const& connections,
    GraphBuilderPublicPorts const& ports, GraphBuilderVirtualNodes const& virtuals,
    GraphBuilderDetach const& detach, bool execution_root) {
  return Lowerer(bundles,connections,ports,virtuals,detach,execution_root).run();
}
} // namespace iv
