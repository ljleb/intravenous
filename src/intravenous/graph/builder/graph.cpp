#include <intravenous/graph/builder.h>

#include <intravenous/graph/builder/embedder.h>
#include <intravenous/graph/builder/lowering.h>

#include <algorithm>
#include <ranges>

namespace iv {
SamplePortRef::SamplePortRef(GraphBuilder& builder, ChannelTypeId type,
                             std::vector<SampleOutputChannelId> channels_)
    : graph_builder(&builder), channel_type(type), channels(std::move(channels_)) {
  if (channels.size() != channel_count(channel_type))
    details::error("sample expression channel count does not match its semantic channel type");
  for (auto channel : channels) {
    auto config = builder._node_bundles.resolve_sample_output(
        {channel.bundle, PortKind::sample, channel.port}).config;
    if (channel.channel >= channel_count(config.channel_layout.channel_type))
      details::error("sample expression channel is out of bounds");
  }
}
SamplePortRef SamplePortRef::_clone_handle() const { return *this; }
SamplePortRef SamplePortRef::select_channel(size_t channel) const {
  if (!graph_builder) details::error("attempted to select a channel from an empty sample port");
  if (channel >= channels.size()) details::error("sample channel ordinal is out of bounds");
  return SamplePortRef(*graph_builder, ChannelTypeId::mono, {channels[channel]});
}
std::string SamplePortRef::to_string() const {
  if (!graph_builder) return "empty sample port";
  if (auto logical = graph_builder->_node_bundles.sample_output_port_for_channels(channel_type, channels))
    return "sample output " + std::to_string(logical->port_ordinal) + " of node bundle " + std::to_string(logical->node_bundle_handle);
  return "structural sample expression with " + std::to_string(channels.size()) + " channel(s) in builder " + graph_builder->_identity.value;
}

EventPortRef::EventPortRef(GraphBuilder& builder, NodeBundlePortId port)
    : graph_builder(&builder) {
  if (port.port_kind != PortKind::event)
    details::error("attempted to create an EventPortRef from a sample NodeBundle port");
  type = builder._node_bundles.resolve_event_output(port).config.type;
  sources = builder._node_bundles.event_output_ports(port);
}
EventPortRef::EventPortRef(GraphBuilder& builder, EventTypeId type_,
                           std::vector<EventOutputPortId> sources_)
    : graph_builder(&builder), type(type_), sources(std::move(sources_)) {
  if (sources.empty()) details::error("event expression has no semantic sources");
  for (auto source : sources) {
    auto config = builder._node_bundles.resolve_event_output(
        {source.bundle, PortKind::event, source.port}).config;
    if (config.type != type) details::error("event expression source does not match its semantic event type");
  }
}
std::string EventPortRef::to_string() const {
  if (!graph_builder) return "empty event";
  if (sources.size() == 1)
    return "event output " + std::to_string(sources.front().port) + " of node bundle " + std::to_string(sources.front().bundle);
  return "structural event expression with " + std::to_string(sources.size()) + " source(s) in builder " + graph_builder->_identity.value;
}

GraphBuilder GraphBuilder::derive_nested_builder() {
  return GraphBuilder(GraphBuilderIdentity(_identity.child_id(_node_bundles.size())));
}

PublicSampleInputRef GraphBuilder::input() { return input(Sample{0.0f}); }
PublicSampleInputRef GraphBuilder::input_named(std::string_view name, Sample value,
                                               std::optional<Sample> min,
                                               std::optional<Sample> max) {
  return PublicSampleInputRef(_public_ports.add_sample_input(*this,_node_bundles,name,value,min,max));
}
PublicSampleInputRef GraphBuilder::input(Sample value,std::optional<Sample> min,std::optional<Sample> max) {
  return PublicSampleInputRef(_public_ports.add_sample_input(*this,_node_bundles,{},value,min,max));
}
PublicEventInputRef GraphBuilder::event_input_named(std::string_view name, EventTypeId type) {
  return PublicEventInputRef(_public_ports.add_event_input(*this,_node_bundles,name,type));
}
PublicEventInputRef GraphBuilder::event_input(EventTypeId type) {
  return PublicEventInputRef(_public_ports.add_event_input(*this,_node_bundles,{},type));
}

void GraphBuilder::annotate_public_sample_input_source_info(
    PublicSampleInputRef const& ref,std::string_view id,std::string_view file,uint32_t begin,uint32_t end) {
  if(id.empty()||ref.port.graph_builder!=this)return;
  auto logical=_node_bundles.sample_output_port_for_channels(ref.port.channel_type,ref.port.channels);
  if(!logical)details::error("PublicSampleInputRef has no logical boundary port");
  if(logical->node_bundle_handle==_public_ports.boundary_handle()) {
    _public_ports.annotate_sample_input_source_info(logical->port_ordinal,id,file,begin,end);
    return;
  }
  auto& boundary=_node_bundles.bundle(logical->node_bundle_handle);
  if(!boundary.is_boundary()||logical->port_ordinal>=boundary.boundary_sample_inputs().size())
    details::error("PublicSampleInputRef does not belong to a valid boundary input");
  SourceInfo info{.declaration_identity=std::string(id),.span={.file_path=std::string(file),.begin=begin,.end=end}};
  auto& infos=boundary.source_annotations().infos;
  if(!std::ranges::contains(infos,info))infos.push_back(std::move(info));
}
void PublicSampleInputRef::_annotate_source_info(std::string_view id,std::string_view file,uint32_t begin,uint32_t end) const { if(port.graph_builder)port.graph_builder->annotate_public_sample_input_source_info(*this,id,file,begin,end); }
void GraphBuilder::annotate_public_event_input_source_info(
    PublicEventInputRef const& ref,std::string_view id,std::string_view file,uint32_t begin,uint32_t end) {
  if(id.empty()||ref.port.graph_builder!=this)return;
  if(ref.port.sources.size()!=1)details::error("PublicEventInputRef has no unique logical boundary port");
  auto source=ref.port.sources.front();
  if(source.bundle==_public_ports.boundary_handle()) {
    _public_ports.annotate_event_input_source_info(source.port,id,file,begin,end);
    return;
  }
  auto& boundary=_node_bundles.bundle(source.bundle);
  if(!boundary.is_boundary()||source.port>=boundary.boundary_event_inputs().size())
    details::error("PublicEventInputRef does not belong to a valid boundary input");
  SourceInfo info{.declaration_identity=std::string(id),.span={.file_path=std::string(file),.begin=begin,.end=end}};
  auto& infos=boundary.source_annotations().infos;
  if(!std::ranges::contains(infos,info))infos.push_back(std::move(info));
}
void PublicEventInputRef::_annotate_source_info(std::string_view id,std::string_view file,uint32_t begin,uint32_t end) const { if(port.graph_builder)port.graph_builder->annotate_public_event_input_source_info(*this,id,file,begin,end); }
void GraphBuilder::annotate_public_sample_output_source_info(std::span<SourceInfo const> infos){for(size_t i=0;i<infos.size();++i)_public_ports.annotate_sample_output_source_info(i,infos[i]);}
void GraphBuilder::annotate_public_event_output_source_info(std::span<SourceInfo const> infos){for(size_t i=0;i<infos.size();++i)_public_ports.annotate_event_output_source_info(i,infos[i]);}

NodeRef GraphBuilder::embed_subgraph(GraphBuilder const& child,std::string_view kind) {
  if(!child._public_ports.sample_outputs_defined())details::error("builder "+child._identity.value+": g.outputs(...) must be called before insertion");
  auto const begin=_node_bundles.size();
  auto const offset=GraphBuilderChildEmbedder::embed(_node_bundles,_connections,_detach,_virtual_nodes,
      child._public_ports,child._node_bundles,child._connections,child._detach,child._virtual_nodes);
  IV_ASSERT(offset==begin,"embedded child bundle offset changed unexpectedly");
  auto const boundary=offset+child._public_ports.boundary_handle();
  auto const count=child._node_bundles.size();
  return NodeRef(*this,_node_bundles.append_subgraph(boundary,begin,count,kind));
}

void GraphBuilder::event_outputs(std::span<EventOutputRefConfig const> refs){_public_ports.define_event_outputs(*this,_node_bundles,_identity,refs);}
constexpr void GraphBuilder::outputs(std::initializer_list<NamedRef> refs){outputs(std::span<NamedRef const>(refs.begin(),refs.size()));}
void GraphBuilder::outputs(std::span<OutputRefConfig const> refs){_public_ports.define_sample_outputs(*this,_node_bundles,_identity,refs);}
constexpr void GraphBuilder::outputs(std::span<NamedRef const> refs){_public_ports.define_sample_outputs_from_named_refs(*this,_node_bundles,_identity,[&](auto&&v){return lift_to_sample_port(std::forward<decltype(v)>(v));},refs);}

GraphBuilder::VacantInputs GraphBuilder::vacant_inputs() const{return _connections.collect_vacant_inputs(_node_bundles,_virtual_nodes);}
GraphBuilder::VirtualInputs GraphBuilder::virtual_inputs() const{return _connections.collect_virtual_inputs(_node_bundles,_virtual_nodes);}
GraphBuilder::VirtualSampleInputFamilies GraphBuilder::virtual_sample_input_families() const{return _connections.collect_virtual_sample_input_families(_node_bundles,_virtual_nodes);}
GraphBuilder::VirtualOutputs GraphBuilder::virtual_outputs() const{return _connections.collect_virtual_outputs(_node_bundles,_virtual_nodes);}
GraphBuilder::VirtualSampleOutputFamilies GraphBuilder::virtual_sample_output_families() const{return _connections.collect_virtual_sample_output_families(_node_bundles,_virtual_nodes);}
GraphBuilder::VirtualPorts GraphBuilder::virtual_ports() const{return _virtual_nodes.ports(_node_bundles);}
GraphBuilderPublicSamplePortFamilies GraphBuilder::public_sample_input_families() const{return _public_ports.sample_input_families(_node_bundles);}
bool GraphBuilder::public_sample_input_is_connected(size_t i) const{auto channels=_node_bundles.sample_output_channels({_public_ports.boundary_handle(),PortKind::sample,i});return std::ranges::any_of(channels,[&](auto c){return _connections.sample_output_is_connected(c);});}
std::vector<GraphBuilderPublicEventInput> GraphBuilder::public_event_inputs() const{return _public_ports.collected_event_inputs(_node_bundles);}
bool GraphBuilder::public_event_input_is_connected(size_t i) const{auto ports=_node_bundles.event_output_ports({_public_ports.boundary_handle(),PortKind::event,i});return std::ranges::any_of(ports,[&](auto p){return _connections.event_output_is_connected(p);});}
std::span<SourceInfo const> GraphBuilder::public_event_input_source_infos(size_t i) const{return _public_ports.event_input_source_infos(i);}
GraphBuilderPublicSamplePortFamilies GraphBuilder::public_sample_output_families() const{return _public_ports.sample_output_families(_node_bundles);}
std::vector<GraphBuilderPublicEventOutput> GraphBuilder::public_event_outputs() const{return _public_ports.collected_event_outputs(_node_bundles);}

void GraphBuilder::record_authored_sample_connection(SampleInputChannelId target,SamplePortRef const& source) {
  if(!source.graph_builder||source.graph_builder!=this)details::error("invalid authored sample channel connection");
  auto channels=_node_bundles.sample_input_channels({target.bundle,PortKind::sample,target.port});
  if(target.channel>=channels.size()||channels[target.channel]!=target)details::error("sample input channel does not belong to its NodeBundle port");
  _connections.record_authored_sample_connection({source.channel_type,source.channels,ChannelTypeId::mono,{target}});
}
void GraphBuilder::record_authored_sample_connection(NodeBundlePortId target,std::span<SamplePortRef const> sources) {
  auto targets=_node_bundles.sample_input_channels(target); auto type=_node_bundles.resolve_sample_input(target).config.channel_layout.channel_type;
  if(sources.size()!=targets.size())details::error("sample channel source count does not match NodeBundle port layout");
  std::vector<SampleOutputChannelId> channels; for(auto const& source:sources){if(source.graph_builder!=this||source.channel_type!=ChannelTypeId::mono||source.channels.size()!=1)details::error("channel-wise sample connection requires scalar sources");channels.push_back(source.channels.front());}
  _connections.record_authored_sample_connection({type,std::move(channels),type,std::move(targets)});
}
void GraphBuilder::record_authored_event_connection(NodeBundlePortId target,EventPortRef const& source) {
  if(target.port_kind!=PortKind::event||source.graph_builder!=this||source.sources.empty())details::error("invalid authored event connection");
  auto target_type=_node_bundles.resolve_event_input(target).config.type;
  _connections.record_authored_event_connection({source.type,source.sources,target_type,_node_bundles.event_input_ports(target)});
}
void GraphBuilder::connect_sample_input(NodeBundlePortId target,std::span<SamplePortRef const> sources){record_authored_sample_connection(target,sources);}
void GraphBuilder::connect_event_input(NodeBundlePortId target,EventPortRef source){record_authored_event_connection(target,source);}
bool GraphBuilder::sample_input_is_connected(NodeBundlePortId target) const{if(target.port_kind!=PortKind::sample)details::error("sample connectivity requested for event port");auto c=_node_bundles.sample_input_channels(target);return std::ranges::any_of(c,[&](auto x){return _connections.sample_input_is_connected(x);});}
bool GraphBuilder::event_input_is_connected(NodeBundlePortId target) const{if(target.port_kind!=PortKind::event)details::error("event connectivity requested for sample port");auto p=_node_bundles.event_input_ports(target);return std::ranges::any_of(p,[&](auto x){return _connections.event_input_is_connected(x);});}
void GraphBuilder::connect_sample_output(NodeBundlePortId source,NodeRef const& target) {
  auto semantic=SamplePortRef(*this,source); auto const& sink=_node_bundles.bundle(target.node_bundle_handle());
  if(!sink.is_concrete())details::error("a graph-service sink must be one concrete node bundle");
  if(semantic.channels.size()!=sink.sample_input_count())details::error("NodeBundle output does not match graph-service sink channel count");
  for(size_t i=0;i<sink.sample_input_count();++i)record_authored_sample_connection({target.node_bundle_handle(),PortKind::sample,i},semantic.select_channel(i));
}
EventPortRef GraphBuilder::event_output(NodeBundlePortId source) const{if(source.port_kind!=PortKind::event)details::error("attempted to read an event output from a sample NodeBundle port");return EventPortRef(const_cast<GraphBuilder&>(*this),source);}
size_t GraphBuilder::sample_port_index(NodeBundleHandle h,bool inputs,std::string_view n) const {
  auto const& candidate=_node_bundles.bundle(h);
  auto const count=inputs?candidate.sample_input_count():candidate.sample_output_count();
  std::optional<size_t> match;
  for(size_t i=0;i<count;++i){
    auto const name=inputs
      ? _node_bundles.resolve_sample_input({h,PortKind::sample,i}).config.name
      : _node_bundles.resolve_sample_output({h,PortKind::sample,i}).config.name;
    if(name!=n)continue;
    if(match)details::error("NodeBundle port name '"+std::string(n)+"' is ambiguous");
    match=i;
  }
  if(!match)details::error("NodeBundle port name '"+std::string(n)+"' does not exist");
  return *match;
}
size_t GraphBuilder::event_port_index(NodeBundleHandle h,bool inputs,std::string_view n) const {
  auto const& candidate=_node_bundles.bundle(h);
  auto const count=inputs?candidate.event_input_count():candidate.event_output_count();
  std::optional<size_t> match;
  for(size_t i=0;i<count;++i){
    auto const name=inputs
      ? _node_bundles.resolve_event_input({h,PortKind::event,i}).config.name
      : _node_bundles.resolve_event_output({h,PortKind::event,i}).config.name;
    if(name!=n)continue;
    if(match)details::error("NodeBundle port name '"+std::string(n)+"' is ambiguous");
    match=i;
  }
  if(!match)details::error("NodeBundle port name '"+std::string(n)+"' does not exist");
  return *match;
}

GraphIntrospectionMetadata GraphBuilder::build_metadata(size_t detach_offset) const {
  auto lowered=GraphBuilderLowering::lower(
      _node_bundles,_connections,_public_ports,_virtual_nodes,_detach);
  auto metadata=GraphBuilderFinalizer::build_metadata(
      _identity,lowered,_node_bundles,_virtual_nodes,_connections,detach_offset);
  auto sample_inputs=public_sample_input_families();
  for(auto& family:sample_inputs.families) {
    family.authored_connected=std::ranges::any_of(
        family.channels,[&](auto const& channel){
          return std::ranges::any_of(channel.port_ordinals,[&](auto ordinal){
            return public_sample_input_is_connected(ordinal);
          });
        });
  }
  metadata.public_sample_inputs=std::move(sample_inputs.families);
  metadata.public_event_inputs=public_event_inputs();
  for(auto& input:metadata.public_event_inputs)
    input.graph_connected=public_event_input_is_connected(input.port_ordinal);
  metadata.public_sample_outputs=public_sample_output_families().families;
  metadata.public_event_outputs=public_event_outputs();
  return metadata;
}
consteval GraphBuilder::RootNodeBuildResult GraphBuilder::build_root_node(size_t detach_offset) const {
  auto lowered=GraphBuilderLowering::lower(
      _node_bundles,_connections,_public_ports,_virtual_nodes,_detach);
  return GraphBuilderFinalizer::build_root_node(
      _identity, lowered, _node_bundles, _virtual_nodes, _public_ports,
      detach_offset);
}
consteval GraphBuilder::RootNodeBuildResult GraphBuilder::build_execution_root_node(
    size_t detach_offset) const {
  auto lowered=GraphBuilderLowering::lower(
      _node_bundles,_connections,_public_ports,_virtual_nodes,_detach,
      true);
  return GraphBuilderFinalizer::build_root_node(
      _identity,lowered,_node_bundles,_virtual_nodes,_public_ports,
      detach_offset,true);
}
} // namespace iv
