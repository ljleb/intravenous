#include <intravenous/graph/builder/lowering.h>

#include <intravenous/basic_nodes/routing.h>
#include <intravenous/graph/builder/connections.h>
#include <intravenous/graph/builder/detach.h>
#include <intravenous/graph/builder/public_ports.h>
#include <intravenous/graph/sample_projection_node.h>

#include <algorithm>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace iv {
namespace {
class Lowerer {
  GraphBuilderNodeBundles const& bundles;
  GraphBuilderConnections const& connections;
  GraphBuilderPublicPorts const& public_ports;
  GraphBuilderDetach const& detach;
  LoweredBuilderGraph out;
  std::unordered_map<NodeBundleHandle, size_t> subgraph_by_boundary;

  size_t append_generated(ConcreteNode node) {
    auto const index = out.topology.append_node(std::move(node));
    if (out.bundle_by_lowered_node.size() <= index)
      out.bundle_by_lowered_node.resize(index + 1);
    out.bundle_by_lowered_node[index] = std::nullopt;
    return index;
  }

  void project_bundles() {
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
        // The concrete member bundles are an execution detail of the tiled
        // authored bundle. Preserve the current metadata ownership rule: the
        // lowered tile nodes belong to the tiled bundle as one semantic/UI
        // member, not to separately visible concrete member bundles.
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
        if(subgraph_by_boundary.contains(info.boundary))details::error("boundary belongs to more than one subgraph");
        subgraph_by_boundary[info.boundary]=node;
        auto const& bp=out.bundle_projections[info.boundary];
        for(size_t i=0;i<bp.sample_outputs.size();++i)
          if(!bp.sample_outputs[i].empty())out.subgraph_input_of_boundary_source[bp.sample_outputs[i].front()]={node,i};
        for(size_t i=0;i<bp.event_outputs.size();++i)
          if(!bp.event_outputs[i].empty())out.subgraph_event_input_of_boundary_source[bp.event_outputs[i].front()]={node,i};
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

  ResolvedSampleSourceChannel resolve_sample_source_channel(
      SampleOutputChannelId channel) const {
    NodeBundlePortId logical{
        channel.bundle, PortKind::sample, channel.port};
    auto const config = bundles.resolve_sample_output(logical).config;
    auto const count = channel_count(config.channel_layout.channel_type);
    auto const& endpoints =
        out.bundle_projections.at(channel.bundle).sample_outputs.at(channel.port);
    if (channel.channel >= count)
      details::error("sample source channel out of bounds");

    if (endpoints.size() == 1)
      return {endpoints.front(), config, channel.channel};

    if (endpoints.size() == count && bundles.bundle(channel.bundle).is_tiled()) {
      auto const member =
          bundles.tiled_member(channel.bundle, channel.channel);
      auto const member_config =
          bundles.resolve_sample_output(
              {member, PortKind::sample, channel.port}).config;
      if (channel_count(member_config.channel_layout.channel_type) != 1)
        details::error(
            "tiled sample source member must expose one concrete channel");
      return {endpoints[channel.channel], member_config, 0};
    }

    details::error("sample source has invalid lowered endpoint count");
  }

  ResolvedSampleTargetChannel resolve_sample_target_channel(
      SampleInputChannelId channel) const {
    NodeBundlePortId logical{
        channel.bundle, PortKind::sample, channel.port};
    auto const config = bundles.resolve_sample_input(logical).config;
    auto const count = channel_count(config.channel_layout.channel_type);
    auto const& endpoints =
        out.bundle_projections.at(channel.bundle).sample_inputs.at(channel.port);
    if (channel.channel >= count)
      details::error("sample target channel out of bounds");

    if (endpoints.empty()) {
      if (!subgraph_by_boundary.contains(channel.bundle))
        details::error("sample target has no lowered endpoint");
      return {std::nullopt, config, channel.channel};
    }

    if (endpoints.size() == 1)
      return {endpoints.front(), config, channel.channel};

    if (endpoints.size() == count && bundles.bundle(channel.bundle).is_tiled()) {
      auto const member =
          bundles.tiled_member(channel.bundle, channel.channel);
      auto const member_config =
          bundles.resolve_sample_input(
              {member, PortKind::sample, channel.port}).config;
      if (channel_count(member_config.channel_layout.channel_type) != 1)
        details::error(
            "tiled sample target member must expose one concrete channel");
      return {endpoints[channel.channel], member_config, 0};
    }

    details::error("sample target has invalid lowered endpoint count");
  }

  std::optional<TopologyPortId> direct_sample_source(
      ChannelTypeId type,
      std::span<SampleOutputChannelId const> channels) const {
    if (channels.empty())
      details::error("sample source has no channels");

    if (auto logical = bundles.sample_output_port_for_channels(type, channels)) {
      auto const& endpoints =
          out.bundle_projections.at(logical->node_bundle_handle)
              .sample_outputs.at(logical->port_ordinal);
      if (endpoints.size() == 1)
        return endpoints.front();
    }

    // Selecting a channel from a tiled bundle is still a direct full mono
    // execution port. Selecting a channel from a native multichannel port is
    // not: that requires a projection.
    if (type == ChannelTypeId::mono && channels.size() == 1) {
      auto const resolved = resolve_sample_source_channel(channels.front());
      if (resolved.channel == 0 &&
          channel_count(resolved.config.channel_layout.channel_type) == 1)
        return resolved.port;
    }

    return std::nullopt;
  }

  ConcreteNode make_sample_projection_node(
      std::vector<InputConfig> inputs,
      std::vector<OutputConfig> outputs,
      std::vector<SampleProjectionNode::Route> routes) const {
    SampleProjectionNode node(
        std::move(inputs), std::move(outputs), std::move(routes));
    auto const input_configs = node.inputs();
    auto const output_configs = node.outputs();

    return ConcreteNode{
        .ports = NodePorts{
            .sample_inputs =
                std::vector<InputConfig>(
                    input_configs.begin(), input_configs.end()),
            .sample_outputs =
                std::vector<OutputConfig>(
                    output_configs.begin(), output_configs.end()),
        },
        .materialization = NodeMaterialization{
            .factory =
                [node = std::move(node)](size_t) {
                  return TypeErasedNode(node);
                },
        },
        .type_identity =
            NodeTypeIdentity{.value = "iv::SampleProjectionNode"},
    };
  }

  struct SampleGroup {
    NodeBundlePortId target{};
    std::vector<AuthoredSampleConnection const*> connections{};
  };

  std::vector<SampleGroup> sample_groups() const {
    std::vector<SampleGroup> groups;
    for (auto const& c : connections.authored_sample_connections()) {
      if (c.target_channels.empty())
        details::error("sample connection has no target");
      auto first = c.target_channels.front();
      if (!std::ranges::all_of(c.target_channels, [&](auto x) {
            return x.bundle == first.bundle && x.port == first.port;
          }))
        details::error("sample target spans bundle ports");

      NodeBundlePortId target{first.bundle, PortKind::sample, first.port};
      auto all = bundles.sample_input_channels(target);
      bool whole =
          c.target_type ==
              bundles.resolve_sample_input(target).config.channel_layout.channel_type &&
          all == c.target_channels;
      if (whole) {
        groups.push_back({target, {&c}});
        continue;
      }

      auto it = std::find_if(groups.begin(), groups.end(), [&](auto const& g) {
        if (g.target != target)
          return false;
        auto const& x = *g.connections.front();
        return x.target_channels != all;
      });
      if (it == groups.end()) {
        groups.push_back({target, {}});
        it = std::prev(groups.end());
      }
      it->connections.push_back(&c);
    }
    return groups;
  }

  struct ProjectionOutput {
    std::optional<TopologyPortId> target{};
    OutputConfig config{};
  };

  struct ProjectedSampleGroup {
    std::vector<TopologyPortId> unbound_outputs{};
  };

  ProjectedSampleGroup project_sample_group(SampleGroup const& group) {
    std::vector<TopologyPortId> input_sources;
    std::vector<InputConfig> input_configs;
    std::vector<ProjectionOutput> projection_outputs;
    std::vector<SampleProjectionNode::Route> routes;
    std::vector<std::vector<bool>> claimed_output_channels;

    auto projection_input = [&](ResolvedSampleSourceChannel const& source) {
      auto it = std::ranges::find(input_sources, source.port);
      if (it != input_sources.end())
        return static_cast<size_t>(it - input_sources.begin());

      input_sources.push_back(source.port);
      input_configs.push_back(InputConfig{
          .channel_layout = source.config.channel_layout,
      });
      return input_sources.size() - 1;
    };

    auto projection_output = [&](ResolvedSampleTargetChannel const& target) {
      auto it = std::find_if(
          projection_outputs.begin(), projection_outputs.end(),
          [&](ProjectionOutput const& output) {
            return output.target == target.port;
          });
      if (it != projection_outputs.end()) {
        if (it->config.channel_layout != target.config.channel_layout)
          details::error(
              "sample projection target endpoint changed channel layout");
        return static_cast<size_t>(it - projection_outputs.begin());
      }

      projection_outputs.push_back(ProjectionOutput{
          .target = target.port,
          .config = OutputConfig{
              .channel_layout = target.config.channel_layout,
          },
      });
      claimed_output_channels.emplace_back(
          channel_count(target.config.channel_layout.channel_type), false);
      return projection_outputs.size() - 1;
    };

    for (auto const* connection : group.connections) {
      if (connection->source_channels.size() !=
          channel_count(connection->source_type))
        details::error(
            "sample source channel count does not match its semantic type");
      if (connection->target_channels.size() !=
          channel_count(connection->target_type))
        details::error(
            "sample target channel count does not match its semantic type");

      SampleProjectionNode::Route route{
          .source_type = connection->source_type,
          .target_type = connection->target_type,
      };

      for (auto const source_channel : connection->source_channels) {
        auto const source = resolve_sample_source_channel(source_channel);
        route.sources.push_back({
            .port = projection_input(source),
            .channel = source.channel,
        });
      }

      for (auto const target_channel : connection->target_channels) {
        auto const target = resolve_sample_target_channel(target_channel);
        auto const output = projection_output(target);
        if (target.channel >= claimed_output_channels[output].size())
          details::error("sample projection target channel out of bounds");
        if (claimed_output_channels[output][target.channel])
          details::error("sample target channel has multiple sources");
        claimed_output_channels[output][target.channel] = true;
        route.targets.push_back({
            .port = output,
            .channel = target.channel,
        });
      }

      routes.push_back(std::move(route));
    }

    if (projection_outputs.empty())
      details::error("sample projection has no outputs");

    std::vector<OutputConfig> output_configs;
    output_configs.reserve(projection_outputs.size());
    for (auto const& output : projection_outputs)
      output_configs.push_back(output.config);

    auto const node = append_generated(make_sample_projection_node(
        std::move(input_configs), std::move(output_configs),
        std::move(routes)));

    for (size_t input = 0; input < input_sources.size(); ++input)
      out.topology.add_sample_edge({input_sources[input], {node, input}});

    ProjectedSampleGroup result;
    for (size_t output = 0; output < projection_outputs.size(); ++output) {
      TopologyPortId const source{node, output};
      if (projection_outputs[output].target)
        out.topology.add_sample_edge(
            {source, *projection_outputs[output].target});
      else
        result.unbound_outputs.push_back(source);
    }
    return result;
  }

  bool lower_tiled_group_direct(
      SampleGroup const& group,
      std::span<TopologyPortId const> endpoints) {
    auto const target_channels = bundles.sample_input_channels(group.target);
    auto const target_type =
        bundles.resolve_sample_input(group.target).config.channel_layout.channel_type;

    if (group.connections.size() == 1) {
      auto const& connection = *group.connections.front();
      bool const whole_target =
          connection.target_type == target_type &&
          connection.target_channels == target_channels;

      if (whole_target && connection.source_type == ChannelTypeId::mono) {
        if (auto source = direct_sample_source(
                connection.source_type, connection.source_channels)) {
          for (auto const target : endpoints)
            out.topology.add_sample_edge({*source, target});
          return true;
        }
      }

      if (whole_target && connection.source_type == target_type &&
          connection.source_channels.size() == endpoints.size()) {
        std::vector<TopologyPortId> sources;
        sources.reserve(connection.source_channels.size());
        for (auto const source_channel : connection.source_channels) {
          auto const resolved = resolve_sample_source_channel(source_channel);
          if (resolved.channel != 0 ||
              channel_count(resolved.config.channel_layout.channel_type) != 1)
            return false;
          sources.push_back(resolved.port);
        }
        for (size_t channel = 0; channel < endpoints.size(); ++channel)
          out.topology.add_sample_edge({sources[channel], endpoints[channel]});
        return true;
      }
    }

    std::vector<std::pair<TopologyPortId, TopologyPortId>> edges;
    std::vector<bool> claimed(endpoints.size(), false);
    for (auto const* connection : group.connections) {
      if (connection->target_type != ChannelTypeId::mono ||
          connection->target_channels.size() != 1)
        return false;

      auto it = std::ranges::find(
          target_channels, connection->target_channels.front());
      if (it == target_channels.end())
        details::error("partial target not in tiled input");
      auto const channel =
          static_cast<size_t>(it - target_channels.begin());
      if (claimed[channel])
        details::error("sample target channel has multiple sources");

      auto source = direct_sample_source(
          connection->source_type, connection->source_channels);
      if (!source)
        return false;

      claimed[channel] = true;
      edges.push_back({*source, endpoints[channel]});
    }

    for (auto const& [source, target] : edges)
      out.topology.add_sample_edge({source, target});
    return !edges.empty();
  }

  void lower_samples() {
    std::vector<NodeBundlePortId> assigned_subgraph_outputs;
    auto groups = sample_groups();

    for (auto const& group : groups) {
      auto const& endpoints =
          out.bundle_projections.at(group.target.node_bundle_handle)
              .sample_inputs.at(group.target.port_ordinal);
      auto const target_channels = bundles.sample_input_channels(group.target);
      auto const target_type =
          bundles.resolve_sample_input(group.target)
              .config.channel_layout.channel_type;

      if (auto it =
              subgraph_by_boundary.find(group.target.node_bundle_handle);
          it != subgraph_by_boundary.end()) {
        auto& binding =
            out.topology.subgraph_node(it->second).lowered_subgraph;
        if (group.target.port_ordinal >= binding.sample_output_sources.size())
          details::error("subgraph sample output out of bounds");
        if (std::ranges::contains(assigned_subgraph_outputs, group.target))
          details::error("subgraph sample output has more than one source");
        assigned_subgraph_outputs.push_back(group.target);

        std::optional<TopologyPortId> source;
        if (group.connections.size() == 1) {
          auto const& connection = *group.connections.front();
          if (connection.target_type == target_type &&
              connection.target_channels == target_channels)
            source = direct_sample_source(
                connection.source_type, connection.source_channels);
        }

        if (!source) {
          auto projected = project_sample_group(group);
          if (projected.unbound_outputs.size() != 1)
            details::error(
                "subgraph sample projection must produce one output");
          source = projected.unbound_outputs.front();
        }

        binding.sample_output_sources[group.target.port_ordinal] = *source;
        continue;
      }

      if (endpoints.empty())
        details::error("sample target has no lowered endpoint");

      if (endpoints.size() == 1) {
        if (group.connections.size() == 1) {
          auto const& connection = *group.connections.front();
          if (connection.target_type == target_type &&
              connection.target_channels == target_channels) {
            if (auto source = direct_sample_source(
                    connection.source_type, connection.source_channels)) {
              out.topology.add_sample_edge({*source, endpoints.front()});
              continue;
            }
          }
        }

        auto projected = project_sample_group(group);
        if (!projected.unbound_outputs.empty())
          details::error(
              "concrete sample projection left an output unbound");
        continue;
      }

      if (lower_tiled_group_direct(group, endpoints))
        continue;

      auto projected = project_sample_group(group);
      if (!projected.unbound_outputs.empty())
        details::error("tiled sample projection left an output unbound");
    }

    for (auto const& [boundary, node] : subgraph_by_boundary) {
      auto& binding = out.topology.subgraph_node(node).lowered_subgraph;
      auto const& p = out.bundle_projections.at(boundary);
      for (size_t i = 0; i < p.sample_outputs.size(); ++i) {
        if (p.sample_outputs[i].empty())
          continue;
        auto source = p.sample_outputs[i].front();
        out.topology.for_each_sample_edge([&](TopologyEdge const& edge) {
          if (edge.source == source &&
              !std::ranges::contains(
                  binding.sample_input_targets[i], edge.target))
            binding.sample_input_targets[i].push_back(edge.target);
        });
      }
      for (size_t output = 0;
           output < binding.sample_output_sources.size(); ++output) {
        if (!std::ranges::contains(
                assigned_subgraph_outputs,
                NodeBundlePortId{
                    boundary, PortKind::sample, output}))
          details::error("subgraph sample output has no authored source");
      }
    }
  }

  TopologyPortId materialize_event_source(AuthoredEventConnection const& c) {
    std::vector<TopologyPortId> endpoints;
    for(auto source:c.sources) {
      auto const config=bundles.resolve_event_output({source.bundle,PortKind::event,source.port}).config;
      if(config.type!=c.source_type)details::error("event source type changed before lowering");
      auto const& p=out.bundle_projections.at(source.bundle).event_outputs.at(source.port);
      endpoints.insert(endpoints.end(),p.begin(),p.end());
    }
    if(endpoints.empty())details::error("event source has no lowered endpoint");
    if(endpoints.size()==1)return endpoints.front();
    auto node=append_generated(GraphBuilderNodeBundles::make_concrete_node<EventConcatenation>(endpoints.size(),c.source_type));
    for(size_t i=0;i<endpoints.size();++i)out.topology.add_event_edge({endpoints[i],{node,i},EventConversionRegistry::instance().plan(c.source_type,c.source_type)});
    return {node,0};
  }

  void lower_events() {
    std::vector<NodeBundlePortId> assigned_subgraph_outputs;
    for(auto const& c:connections.authored_event_connections()) {
      auto source=materialize_event_source(c);
      for(auto target:c.targets) {
        auto config=bundles.resolve_event_input({target.bundle,PortKind::event,target.port}).config;
        if(config.type!=c.target_type)details::error("event target type changed before lowering");
        if(auto it=subgraph_by_boundary.find(target.bundle);it!=subgraph_by_boundary.end()) {
          NodeBundlePortId const logical{target.bundle, PortKind::event, target.port};
          if (std::ranges::contains(assigned_subgraph_outputs, logical))
            details::error("subgraph event output has more than one source");
          assigned_subgraph_outputs.push_back(logical);
          out.topology.subgraph_node(it->second).lowered_subgraph.event_output_sources.at(target.port)=source; continue;
        }
        auto const& endpoints=out.bundle_projections.at(target.bundle).event_inputs.at(target.port);
        if(endpoints.empty())details::error("event target has no lowered endpoint");
        for(auto endpoint:endpoints)out.topology.add_event_edge({source,endpoint,EventConversionRegistry::instance().plan(c.source_type,c.target_type)});
      }
    }
    for(auto const& [boundary,node]:subgraph_by_boundary) {
      auto& binding=out.topology.subgraph_node(node).lowered_subgraph;
      auto const& p=out.bundle_projections.at(boundary);
      for(size_t i=0;i<p.event_outputs.size();++i) {
        if(p.event_outputs[i].empty())continue; auto source=p.event_outputs[i].front();
        out.topology.for_each_event_edge([&](TopologyEventEdge const& edge){if(edge.source==source&&!std::ranges::contains(binding.event_input_targets[i],edge.target))binding.event_input_targets[i].push_back(edge.target);});
      }
      for (size_t output = 0; output < binding.event_output_sources.size(); ++output) {
        if (!std::ranges::contains(assigned_subgraph_outputs,
              NodeBundlePortId{boundary, PortKind::event, output}))
          details::error("subgraph event output has no authored source");
      }
    }
  }

  void lower_runtime_filled() {
    for(auto channel:connections.runtime_filled_sample_channels()) {
      auto const& p=out.bundle_projections.at(channel.bundle).sample_inputs.at(channel.port);
      auto count=channel_count(bundles.resolve_sample_input({channel.bundle,PortKind::sample,channel.port}).config.channel_layout.channel_type);
      if(channel.channel>=count)details::error("runtime-filled sample channel out of bounds");
      if(p.size()==count)out.runtime_filled_sample_inputs.insert(p[channel.channel]);
      else if(p.size()==1)out.runtime_filled_sample_inputs.insert(p.front());
    }
    for(auto port:connections.runtime_filled_event_ports()) {
      for(auto endpoint:out.bundle_projections.at(port.bundle).event_inputs.at(port.port))out.runtime_filled_event_inputs.insert(endpoint);
    }
  }

  void lower_detach() {
    for(auto const& info:detach.authored_infos()) {
      auto writer=out.bundle_projections.at(info.writer_bundle).topology_node;
      if(!writer)details::error("detach writer is not a concrete lowered node");
      std::optional<TopologyPortId> source;
      out.topology.for_each_sample_edge([&](TopologyEdge const& edge){if(edge.target==TopologyPortId{*writer,0}){if(source&&*source!=edge.source)details::error("detach writer has multiple sources");source=edge.source;}});
      if(!source)details::error("detach writer has no lowered source");
      auto const reader_channel =
          resolve_sample_source_channel(info.reader_channel);
      if (reader_channel.channel != 0 ||
          channel_count(reader_channel.config.channel_layout.channel_type) != 1)
        details::error("detach reader output must be one concrete channel");
      auto const reader = reader_channel.port;
      out.detached_info_by_source.emplace(*source,DetachedSamplePortInfo{info.detach_id,*source,*writer,reader,info.loop_extra_latency});
      out.detached_reader_outputs.insert(reader);
    }
  }

public:
  Lowerer(GraphBuilderNodeBundles const& b, GraphBuilderConnections const& c,
          GraphBuilderPublicPorts const& p, GraphBuilderDetach const& d)
      : bundles(b),connections(c),public_ports(p),detach(d) {}
  LoweredBuilderGraph run(){project_bundles();lower_samples();lower_events();lower_runtime_filled();lower_detach();return std::move(out);}
};
} // namespace

LoweredBuilderGraph GraphBuilderLowering::lower(
    GraphBuilderNodeBundles const& bundles, GraphBuilderConnections const& connections,
    GraphBuilderPublicPorts const& ports, GraphBuilderDetach const& detach) {
  return Lowerer(bundles,connections,ports,detach).run();
}
} // namespace iv
