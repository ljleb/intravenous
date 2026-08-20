#include <intravenous/graph/builder/lowering.h>

#include <intravenous/basic_nodes/routing.h>
#include <intravenous/graph/builder/connections.h>
#include <intravenous/graph/builder/detach.h>
#include <intravenous/graph/builder/public_ports.h>

#include <algorithm>
#include <array>
#include <functional>
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
  std::unordered_map<NodeBundlePortId, std::vector<TopologyPortId>,
      std::function<size_t(NodeBundlePortId const&)>> unpacked_cache{
        0, [](NodeBundlePortId const& p) {
          return std::hash<size_t>{}(p.node_bundle_handle) ^
                 (std::hash<size_t>{}(p.port_ordinal) << 1);
        }};

  size_t append_generated(ConcreteNode node) {
    auto const index = out.topology.append_node(std::move(node));
    if (out.bundle_by_lowered_node.size() <= index)
      out.bundle_by_lowered_node.resize(index + 1);
    out.bundle_by_lowered_node[index] = std::nullopt;
    return index;
  }

  std::vector<TopologyPortId> ports_for_node(size_t node, size_t count) const {
    std::vector<TopologyPortId> result; result.reserve(count);
    for (size_t i=0;i<count;++i) result.push_back({node,i});
    return result;
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

  TopologyPortId sample_channel_source(SampleOutputChannelId channel) {
    NodeBundlePortId logical{channel.bundle,PortKind::sample,channel.port};
    auto const config=bundles.resolve_sample_output(logical).config;
    auto const count=channel_count(config.channel_layout.channel_type);
    auto const& endpoints=out.bundle_projections.at(channel.bundle).sample_outputs.at(channel.port);
    if(channel.channel>=count)details::error("sample source channel out of bounds");
    if(endpoints.size()==count)return endpoints[channel.channel];
    if(endpoints.size()!=1)details::error("sample source has invalid lowered endpoint count");
    if(count==1)return endpoints.front();
    auto found=unpacked_cache.find(logical);
    if(found==unpacked_cache.end()) {
      if(config.channel_layout.channel_type!=ChannelTypeId::stereo)details::error("unsupported sample channel type for unpack");
      auto node=append_generated(GraphBuilderNodeBundles::make_concrete_node<ChannelUnpack<stereo>>());
      out.topology.add_sample_edge({endpoints.front(),{node,0}});
      found=unpacked_cache.emplace(logical,ports_for_node(node,count)).first;
    }
    return found->second.at(channel.channel);
  }

  TopologyPortId materialize_sample_source(ChannelTypeId type,
      std::span<SampleOutputChannelId const> channels) {
    if(channels.empty())details::error("sample source has no channels");
    auto const first=channels.front();
    bool one_port=std::ranges::all_of(channels,[&](auto c){return c.bundle==first.bundle&&c.port==first.port;});
    if(one_port) {
      NodeBundlePortId logical{first.bundle,PortKind::sample,first.port};
      auto const declared=bundles.resolve_sample_output(logical).config.channel_layout.channel_type;
      if(declared==type && bundles.sample_output_channels(logical)==std::vector<SampleOutputChannelId>(channels.begin(),channels.end())) {
        auto const& endpoints=out.bundle_projections.at(first.bundle).sample_outputs.at(first.port);
        if(endpoints.size()==1)return endpoints.front();
      }
    }
    if(type==ChannelTypeId::mono && channels.size()==1)return sample_channel_source(channels.front());
    if(type!=ChannelTypeId::stereo || channels.size()!=2)details::error("unsupported structural sample source layout");
    auto node=append_generated(GraphBuilderNodeBundles::make_concrete_node<ChannelPack<stereo>>());
    for(size_t i=0;i<channels.size();++i)out.topology.add_sample_edge({sample_channel_source(channels[i]),{node,i}});
    return {node,0};
  }

  struct SampleGroup { NodeBundlePortId target{}; std::vector<AuthoredSampleConnection const*> connections{}; };
  std::vector<SampleGroup> sample_groups() const {
    std::vector<SampleGroup> groups;
    for(auto const& c:connections.authored_sample_connections()) {
      if(c.target_channels.empty())details::error("sample connection has no target");
      auto first=c.target_channels.front();
      if(!std::ranges::all_of(c.target_channels,[&](auto x){return x.bundle==first.bundle&&x.port==first.port;}))details::error("sample target spans bundle ports");
      NodeBundlePortId target{first.bundle,PortKind::sample,first.port};
      auto all=bundles.sample_input_channels(target);
      bool whole=c.target_type==bundles.resolve_sample_input(target).config.channel_layout.channel_type&&all==c.target_channels;
      if(whole){groups.push_back({target,{&c}});continue;}
      auto it=std::find_if(groups.begin(),groups.end(),[&](auto const& g){
        if(g.target!=target)return false; auto const& x=*g.connections.front(); return x.target_channels!=all;});
      if(it==groups.end()){groups.push_back({target,{}});it=std::prev(groups.end());}
      it->connections.push_back(&c);
    }
    return groups;
  }

  TopologyPortId source_for_group(SampleGroup const& group) {
    auto target_channels=bundles.sample_input_channels(group.target);
    auto target_type=bundles.resolve_sample_input(group.target).config.channel_layout.channel_type;
    if(group.connections.size()==1) {
      auto const& c=*group.connections.front();
      if(c.target_type==target_type&&c.target_channels==target_channels)
        return materialize_sample_source(c.source_type,c.source_channels);
    }
    if(target_type!=ChannelTypeId::stereo)details::error("partial sample target requires supported multichannel layout");
    std::array<std::optional<TopologyPortId>,2> sources;
    for(auto const* c:group.connections) {
      if(c->target_type!=ChannelTypeId::mono||c->target_channels.size()!=1)details::error("sample target mixes whole and channel contributors");
      auto it=std::ranges::find(target_channels,c->target_channels.front());
      if(it==target_channels.end())details::error("sample channel contributor outside target");
      auto channel=static_cast<size_t>(it-target_channels.begin());
      if(sources[channel])details::error("sample target channel has multiple sources");
      sources[channel]=materialize_sample_source(c->source_type,c->source_channels);
    }
    auto node=append_generated(GraphBuilderNodeBundles::make_concrete_node<ChannelPack<stereo>>());
    for(size_t i=0;i<2;++i)if(sources[i])out.topology.add_sample_edge({*sources[i],{node,i}});
    return {node,0};
  }

  void lower_samples() {
    std::vector<NodeBundlePortId> assigned_subgraph_outputs;
    auto groups=sample_groups();
    for(auto const& group:groups) {
      auto const& endpoints=out.bundle_projections.at(group.target.node_bundle_handle).sample_inputs.at(group.target.port_ordinal);
      auto target_channels=bundles.sample_input_channels(group.target);
      auto target_type=bundles.resolve_sample_input(group.target).config.channel_layout.channel_type;
      if(auto it=subgraph_by_boundary.find(group.target.node_bundle_handle);it!=subgraph_by_boundary.end()) {
        auto source=source_for_group(group); auto& binding=out.topology.subgraph_node(it->second).lowered_subgraph;
        if(group.target.port_ordinal>=binding.sample_output_sources.size())details::error("subgraph sample output out of bounds");
        if (std::ranges::contains(assigned_subgraph_outputs, group.target))
          details::error("subgraph sample output has more than one source");
        assigned_subgraph_outputs.push_back(group.target);
        binding.sample_output_sources[group.target.port_ordinal]=source; continue;
      }
      if(endpoints.empty())details::error("sample target has no lowered endpoint");
      if(endpoints.size()==1) { out.topology.add_sample_edge({source_for_group(group),endpoints.front()}); continue; }
      // A tiled input exposes one lowered endpoint per semantic channel.
      if(group.connections.size()==1) {
        auto const& c=*group.connections.front();
        if(c.target_type==target_type&&c.target_channels==target_channels) {
          if(c.source_type==ChannelTypeId::mono) {
            auto s=materialize_sample_source(c.source_type,c.source_channels); for(auto t:endpoints)out.topology.add_sample_edge({s,t});
          } else {
            if(c.source_channels.size()!=endpoints.size())details::error("tiled source/target channel count mismatch");
            for(size_t i=0;i<endpoints.size();++i)out.topology.add_sample_edge({sample_channel_source(c.source_channels[i]),endpoints[i]});
          }
          continue;
        }
      }
      for(auto const* c:group.connections) {
        if(c->target_type!=ChannelTypeId::mono||c->target_channels.size()!=1)details::error("invalid tiled partial connection");
        auto it=std::ranges::find(target_channels,c->target_channels.front()); if(it==target_channels.end())details::error("partial target not in tiled input");
        out.topology.add_sample_edge({materialize_sample_source(c->source_type,c->source_channels),endpoints[static_cast<size_t>(it-target_channels.begin())]});
      }
    }
    for(auto const& [boundary,node]:subgraph_by_boundary) {
      auto& binding=out.topology.subgraph_node(node).lowered_subgraph;
      auto const& p=out.bundle_projections.at(boundary);
      for(size_t i=0;i<p.sample_outputs.size();++i) {
        if(p.sample_outputs[i].empty())continue; auto source=p.sample_outputs[i].front();
        out.topology.for_each_sample_edge([&](TopologyEdge const& edge){ if(edge.source==source && !std::ranges::contains(binding.sample_input_targets[i],edge.target))binding.sample_input_targets[i].push_back(edge.target); });
      }
      for (size_t output = 0; output < binding.sample_output_sources.size(); ++output) {
        if (!std::ranges::contains(assigned_subgraph_outputs,
              NodeBundlePortId{boundary, PortKind::sample, output}))
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
      auto reader=sample_channel_source(info.reader_channel);
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
