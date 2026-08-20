#pragma once
#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/channel_ports.h>
#include <intravenous/graph/builder/annotations.h>
#include <intravenous/graph/builder/connections.h>
#include <intravenous/graph/builder/detach.h>
#include <intravenous/graph/builder/finalize.h>
#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_refs.h>
#include <intravenous/graph/builder/node_bundles.h>
#include <intravenous/graph/builder/public_ports.h>
#include <intravenous/graph/builder/subgraphs.h>
#include <intravenous/graph/builder/virtual_nodes.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cxxabi.h>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace iv {
class GraphBuilder;
namespace details {}

class GraphBuilder {
  friend class NodeRef;
  template<class Derived> friend class NodeRefCrtp;
  template<class Node, class PortProjection> friend class TypedNodeRef;
  friend struct SamplePortRef;
  friend struct EventPortRef;
  friend class GraphBuilderChildEmbedder;
  friend class GraphBuilderConnections;
  friend class GraphBuilderDetach;
  friend class GraphBuilderAnnotations;
  friend class GraphBuilderVirtualNodes;
  friend class GraphBuilderPublicPorts;
  friend class SubgraphScopeManager;

  GraphBuilderIdentity _identity;
  GraphBuilderNodeBundles _node_bundles;
  GraphBuilderConnections _connections;
  GraphBuilderPublicPorts _public_ports;
  SubgraphScopeManager _subgraphs;
  GraphBuilderDetach _detach;
  GraphBuilderAnnotations _annotations;
  GraphBuilderVirtualNodes _virtual_nodes;

  explicit GraphBuilder(GraphBuilderIdentity identity);
  PublicSampleInputRef input_named(std::string_view name, Sample default_value,
                                   std::optional<Sample> min,
                                   std::optional<Sample> max);
  PublicEventInputRef event_input_named(std::string_view name, EventTypeId type);

public:
  GraphBuilder();
  GraphBuilder derive_nested_builder();
  bool inside_subgraph_scope() const;
  ScopedSubgraph& current_scope();
  void define_scope_outputs(std::span<OutputRefConfig const> refs);
  void define_scope_event_outputs(std::span<EventOutputRefConfig const> refs);
  PublicSampleInputRef input();
  template<fixed_string Name>
  PublicSampleInputRef input(Sample default_value = 0.0,
                             std::optional<Sample> min = std::nullopt,
                             std::optional<Sample> max = std::nullopt) {
    return input_named(Name.view(), default_value, min, max);
  }
  PublicSampleInputRef input(Sample default_value,
                             std::optional<Sample> min = std::nullopt,
                             std::optional<Sample> max = std::nullopt);
  template<fixed_string Name>
  PublicEventInputRef event_input(EventTypeId type) { return event_input_named(Name.view(), type); }
  PublicEventInputRef event_input(EventTypeId type);

  void annotate_public_sample_input_source_info(PublicSampleInputRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_public_event_input_source_info(PublicEventInputRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_public_sample_output_source_info(std::span<SourceInfo const> infos);
  void annotate_public_event_output_source_info(std::span<SourceInfo const> infos);

  template<class Config>
  static void validate_output_port_configs(std::span<Config const> configs,
                                           std::string_view node_label,
                                           std::string_view kind);
  template<class Node, class... Args> details::node_ref_for_t<Node> node(Args&&... args);
  template<class Node, class ChannelType, class... Args> auto node(Args&&... args);
  template<class ChannelType, class... Refs> auto tile(Refs&&... refs);
  NodeRef embed_subgraph(GraphBuilder const& child);

  template<class... Refs> void event_outputs(Refs&&... refs);
  void event_outputs(std::span<EventOutputRefConfig const> refs);
  template<class... Refs> void subgraph_event_outputs(Refs&&... refs);
  void subgraph_event_outputs(std::span<EventOutputRefConfig const> refs);
  template<class Fn> NodeRef subgraph(Fn&& fn, std::string_view kind = "Subgraph");
  template<class... Refs> void outputs(Refs&&... refs);
  void outputs(std::initializer_list<NamedRef> refs);
  void outputs(std::span<OutputRefConfig const> refs);
  void outputs(std::span<NamedRef const> refs);
  template<class... Refs> void subgraph_outputs(Refs&&... refs);
  void subgraph_outputs(std::initializer_list<NamedRef> refs);
  void subgraph_outputs(std::span<OutputRefConfig const> refs);
  void subgraph_outputs(std::span<NamedRef const> refs);

  using VacantSampleInput = GraphBuilderVacantSampleInput;
  using VacantEventInput = GraphBuilderVacantEventInput;
  using RootNodeBuildResult = GraphBuilderRootNodeBuildResult;
  using VacantInputs = GraphBuilderVacantInputs;
  using VirtualSampleInput = GraphBuilderVirtualSampleInput;
  using VirtualEventInput = GraphBuilderVirtualEventInput;
  using VirtualInputs = GraphBuilderVirtualInputs;
  using VirtualSampleInputChannel = GraphBuilderVirtualSampleInputChannel;
  using VirtualSampleInputFamily = GraphBuilderVirtualSampleInputFamily;
  using VirtualSampleInputFamilies = GraphBuilderVirtualSampleInputFamilies;
  using VirtualSampleOutput = GraphBuilderVirtualSampleOutput;
  using VirtualEventOutput = GraphBuilderVirtualEventOutput;
  using VirtualOutputs = GraphBuilderVirtualOutputs;
  using VirtualSampleOutputChannel = GraphBuilderVirtualSampleOutputChannel;
  using VirtualSampleOutputFamily = GraphBuilderVirtualSampleOutputFamily;
  using VirtualSampleOutputFamilies = GraphBuilderVirtualSampleOutputFamilies;
  using VirtualPorts = GraphBuilderVirtualPorts;

  VacantInputs vacant_inputs() const;
  VirtualInputs virtual_inputs() const;
  VirtualSampleInputFamilies virtual_sample_input_families() const;
  VirtualOutputs virtual_outputs() const;
  VirtualSampleOutputFamilies virtual_sample_output_families() const;
  VirtualPorts virtual_ports() const;
  GraphBuilderPublicSamplePortFamilies public_sample_input_families() const;
  bool public_sample_input_is_connected(size_t port_ordinal) const;
  std::vector<GraphBuilderPublicEventInput> public_event_inputs() const;
  bool public_event_input_is_connected(size_t port_ordinal) const;
  std::span<SourceInfo const> public_event_input_source_infos(size_t) const;
  GraphBuilderPublicSamplePortFamilies public_sample_output_families() const;
  std::vector<GraphBuilderPublicEventOutput> public_event_outputs() const;

  void connect_sample_input(NodeBundlePortId target, SamplePortRef source);
  void connect_sample_input(NodeBundlePortId target, std::span<SamplePortRef const> sources);
  void connect_event_input(NodeBundlePortId target, EventPortRef source);
  void mark_runtime_filled_sample_input(NodeBundlePortId target);
  void mark_runtime_filled_event_input(NodeBundlePortId target);
  bool sample_input_is_connected(NodeBundlePortId target) const;
  bool event_input_is_connected(NodeBundlePortId target) const;
  void connect_sample_output(NodeBundlePortId source, NodeRef const& target);
  EventPortRef event_output(NodeBundlePortId source) const;
  size_t sample_port_index(NodeBundleHandle, bool inputs, std::string_view name) const;
  size_t event_port_index(NodeBundleHandle, bool inputs, std::string_view name) const;
  GraphIntrospectionMetadata build_metadata(size_t detach_id_offset = 0) const;
  RootNodeBuildResult build_root_node(size_t detach_id_offset = 0) const;
  RootNodeBuildResult build_execution_root_node(size_t detach_id_offset = 0) const;

private:
  static std::string allocate_root_builder_id();
  SamplePortRef detach_sample_port(SamplePortRef const&, size_t loop_extra_latency);
  void record_authored_sample_connection(NodeBundlePortId, SamplePortRef const&);
  void record_authored_sample_connection(SampleInputChannelId, SamplePortRef const&);
  void record_authored_sample_connection(NodeBundlePortId, std::span<SamplePortRef const>);
  void record_authored_event_connection(NodeBundlePortId, EventPortRef const&);
  SamplePortRef lift_to_sample_port(SamplePortRef const& sample_port);
  SamplePortRef lift_to_sample_port(SamplePortRef&& sample_port);
  template<class T>
    requires(!std::same_as<std::remove_cvref_t<T>, SamplePortRef> &&
             requires(std::remove_cvref_t<T> const& ref) { ref.node_ref(); } &&
             std::convertible_to<T, SamplePortRef>)
  SamplePortRef lift_to_sample_port(T&& sample_port) {
    return lift_to_sample_port(static_cast<SamplePortRef>(std::forward<T>(sample_port)));
  }
  template<class ChannelType, SampleStreamLayout Layout>
  SamplePortRef lift_to_sample_port(TypedSamplePortRef<ChannelType, Layout> const& sample_port);
  template<class ChannelType, SampleStreamLayout Layout, class Member>
  SamplePortRef lift_to_sample_port(TypedSamplePortChannelRef<ChannelType, Layout, Member> const& sample_port);
  template<class ChannelType, SampleStreamLayout Layout>
  SamplePortRef lift_to_sample_port(TypedSamplePortTileRef<ChannelType, Layout> const& sample_port);
  template<class T>
    requires std::is_arithmetic_v<std::remove_cvref_t<T>> ||
             std::is_same_v<std::remove_cvref_t<T>, Sample>
  SamplePortRef lift_to_sample_port(T value) {
    auto constant = node<Constant>(static_cast<Sample>(value));
    return static_cast<SamplePortRef>(constant);
  }
  SamplePortRef lift_to_sample_port(NamedRef const& ref);
};

template<class ChannelType, SampleStreamLayout Layout>
SamplePortRef GraphBuilder::lift_to_sample_port(TypedSamplePortRef<ChannelType, Layout> const& p) { return lift_to_sample_port(static_cast<SamplePortRef>(p)); }
template<class ChannelType, SampleStreamLayout Layout, class Member>
SamplePortRef GraphBuilder::lift_to_sample_port(TypedSamplePortChannelRef<ChannelType, Layout, Member> const& p) { return lift_to_sample_port(p.erased()); }
template<class ChannelType, SampleStreamLayout Layout>
SamplePortRef GraphBuilder::lift_to_sample_port(TypedSamplePortTileRef<ChannelType, Layout> const& p) { return lift_to_sample_port(p.erased()); }

template<class Config>
void GraphBuilder::validate_output_port_configs(std::span<Config const> configs,
                                                std::string_view node_label,
                                                std::string_view kind) {
  GraphBuilderNodeBundles::validate_output_port_configs(configs, node_label, kind);
}

template<class Node, class... Args>
details::node_ref_for_t<Node> GraphBuilder::node(Args&&... args) {
  auto const handle = _node_bundles.append_concrete<Node>(std::forward<Args>(args)...);
  using StoredNode = std::remove_cvref_t<Node>;
  if constexpr (details::should_preserve_node_type_v<StoredNode>) return TypedNodeRef<StoredNode>(*this, handle);
  else return NodeRef(*this, handle);
}

template<class Node, class ChannelType, class... Args>
auto GraphBuilder::node(Args&&... args) {
  using StoredNode = std::remove_cvref_t<Node>;
  static_assert(details::has_constexpr_sample_port_configs<StoredNode>,
      "tiled DSP nodes must provide constexpr static inputs()/outputs() configurations");
  static_assert((std::copy_constructible<std::remove_cvref_t<Args>> && ...),
      "tiled-node construction requires reusable constructor arguments");
  static_assert([] consteval {
    if constexpr (requires { StoredNode::inputs(); }) for (auto const& config : StoredNode::inputs()) if (config.channel_layout.channel_type != ChannelTypeId::mono) return false;
    if constexpr (requires { StoredNode::outputs(); }) for (auto const& config : StoredNode::outputs()) if (config.channel_layout.channel_type != ChannelTypeId::mono) return false;
    return true;
  }(), "the tiled-node model requires fully mono concrete sample nodes");
  std::array<NodeBundleHandle, ChannelType::channel_count> members{};
  for (size_t channel=0; channel<ChannelType::channel_count; ++channel)
    members[channel] = _node_bundles.append_concrete<StoredNode>(args...);
  auto const handle = _node_bundles.append_tiled(members,
      ChannelLayout{.channel_type = ChannelTypeTraits<ChannelType>::id,
                    .sample_layout = SampleStreamLayout::planar});
  return TiledNodeRef<StoredNode, ChannelType>(*this, handle);
}

template<class ChannelType, class... Refs>
auto GraphBuilder::tile(Refs&&... refs) {
  static_assert(sizeof...(Refs) == ChannelType::channel_count,
      "g.tile<ChannelType>(...) requires exactly one source per channel");
  std::array<SamplePortRef, ChannelType::channel_count> members{
      lift_to_sample_port(std::forward<Refs>(refs))...};
  for (auto const& member : members)
    if (member.channel_type != ChannelTypeId::mono || member.channels.size()!=1)
      details::error("g.tile<ChannelType>(...) requires scalar sample sources");
  return TypedSamplePortTileRef<ChannelType>{std::move(members)};
}

template<class... Refs>
void GraphBuilder::event_outputs(Refs&&... refs) {
  _public_ports.define_event_outputs_from_args(*this, _node_bundles, _identity,
                                               std::forward<Refs>(refs)...);
}
template<class... Refs>
void GraphBuilder::subgraph_event_outputs(Refs&&... refs) {
  if (!inside_subgraph_scope()) details::error("g.subgraph_event_outputs(...) is only valid inside g.subgraph(...)");
  std::vector<EventOutputRefConfig> outputs; outputs.reserve(sizeof...(Refs));
  constexpr bool require_names = sizeof...(Refs) > 1;
  auto append=[&](auto&& ref){ using Ref=std::remove_cvref_t<decltype(ref)>;
    if constexpr(details::is_named_arg_v<Ref>) outputs.push_back({.ref=static_cast<EventPortRef>(ref.value),.config={.name=std::string(Ref::name.view())}});
    else { if constexpr(require_names)details::error("builder "+_identity.value+": event_outputs(...) requires names when exposing more than one event output"); outputs.push_back({.ref=static_cast<EventPortRef>(ref),.config={}}); }};
  (append(std::forward<Refs>(refs)),...);
  define_scope_event_outputs(outputs);
}
template<class Fn>
NodeRef GraphBuilder::subgraph(Fn&& fn, std::string_view kind) {
  return _subgraphs.run(*this, _node_bundles, std::forward<Fn>(fn), kind);
}
template<class... Refs>
void GraphBuilder::outputs(Refs&&... refs) {
  _public_ports.define_sample_outputs_from_args(*this, _node_bundles, _identity,
      [&](auto&& value){ return lift_to_sample_port(std::forward<decltype(value)>(value)); },
      std::forward<Refs>(refs)...);
}
template<class... Refs>
void GraphBuilder::subgraph_outputs(Refs&&... refs) {
  if (!inside_subgraph_scope()) details::error("g.subgraph_outputs(...) is only valid inside g.subgraph(...)");
  std::vector<OutputRefConfig> outputs; outputs.reserve(sizeof...(Refs));
  constexpr bool require_names = sizeof...(Refs)>1;
  auto append=[&](auto&& ref){ using Ref=std::remove_cvref_t<decltype(ref)>;
    if constexpr(details::is_channel_named_arg_v<Ref>) outputs.push_back({.ref=lift_to_sample_port(ref.value),.config={.name=std::string(Ref::name.view()),.channel_layout={.channel_type=ChannelTypeTraits<typename Ref::channel_type>::id,.sample_layout=SampleStreamLayout::planar}},.public_member={.family_name=std::string(Ref::name.view()),.channel_type=ChannelTypeTraits<typename Ref::channel_type>::id,.channel_index=Ref::channel_ordinal}});
    else if constexpr(details::is_default_channel_named_arg_v<Ref>) outputs.push_back({.ref=lift_to_sample_port(ref.value),.config={.name="main",.channel_layout={.channel_type=ChannelTypeTraits<typename Ref::channel_type>::id,.sample_layout=SampleStreamLayout::planar}},.public_member={.family_name="main",.channel_type=ChannelTypeTraits<typename Ref::channel_type>::id,.channel_index=Ref::channel_ordinal}});
    else if constexpr(details::is_named_arg_v<Ref>) { if constexpr(Ref::name.view().starts_with("__"))details::error("builder "+_identity.value+": generated channel assignments are not public outputs"); using Value=std::remove_cvref_t<decltype(ref.value)>; if constexpr(requires{typename Value::channel_type;{Value::sample_layout}->std::convertible_to<SampleStreamLayout>;}) outputs.push_back({.ref=lift_to_sample_port(ref.value),.config={.name=std::string(Ref::name.view()),.channel_layout={.channel_type=ChannelTypeTraits<typename Value::channel_type>::id,.sample_layout=Value::sample_layout}},.public_member={.family_name=std::string(Ref::name.view()),.channel_type=ChannelTypeTraits<typename Value::channel_type>::id,.whole_stream=true}}); else outputs.push_back({.ref=lift_to_sample_port(ref.value),.config={.name=std::string(Ref::name.view()),.channel_layout=mono_planar_channel_layout},.public_member={.family_name=std::string(Ref::name.view())}}); }
    else { if constexpr(require_names)details::error("builder "+_identity.value+": outputs(...) requires names when exposing more than one sample output"); outputs.push_back({.ref=lift_to_sample_port(std::forward<decltype(ref)>(ref))}); }};
  (append(std::forward<Refs>(refs)),...);
  define_scope_outputs(outputs);
}

// NodeRef and typed-ref definitions are semantic: every operation addresses a
// NodeBundle and no authored ref can be projected to a topology address.
template<class Node, class PortProjection>
inline NodePorts const& TypedNodeRef<Node, PortProjection>::ports() const {
  if (!_graph_builder) details::error("attempted to use a null NodeRef");
  return _graph_builder->_node_bundles.typed_ports(_index);
}
inline NodeRef NodeRef::node_ref() const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); return NodeRef(*_graph_builder,_index); }
inline NodeRef NodeRef::_clone_handle() const { return _graph_builder?NodeRef(*_graph_builder,_index):NodeRef{}; }
inline SamplePortRef NodeRef::operator[](size_t output_port) const {
  if(!_graph_builder)details::error("attempted to use a null NodeRef");
  if(output_port>=_graph_builder->_node_bundles.bundle(_index).sample_output_count())details::error("sample output port is out of bounds on "+to_string());
  return SamplePortRef(*_graph_builder,{_index,PortKind::sample,output_port});
}
inline SamplePortRef NodeRef::operator[](std::string_view name) const { return (*this)[_graph_builder->sample_port_index(_index,false,name)]; }
inline NodeRef::operator SamplePortRef() const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); if(sample_output_count()!=1)details::error(to_string()+" does not have exactly 1 output port"); return (*this)[0]; }
inline size_t NodeRef::sample_input_count() const { return _graph_builder->_node_bundles.bundle(node_bundle_handle()).sample_input_count(); }
inline size_t NodeRef::sample_output_count() const { return _graph_builder->_node_bundles.bundle(node_bundle_handle()).sample_output_count(); }
inline size_t NodeRef::event_input_count() const { return _graph_builder->_node_bundles.bundle(node_bundle_handle()).event_input_count(); }
inline size_t NodeRef::event_output_count() const { return _graph_builder->_node_bundles.bundle(node_bundle_handle()).event_output_count(); }
inline bool NodeRef::input_is_connected(size_t i) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); return _graph_builder->sample_input_is_connected({_index,PortKind::sample,i}); }
inline bool NodeRef::event_input_is_connected(size_t i) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); return _graph_builder->event_input_is_connected({_index,PortKind::event,i}); }
template<class T> inline NodeRef NodeRef::connect_input(size_t i,T&& value) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); auto source=_graph_builder->lift_to_sample_port(std::forward<T>(value)); if(source.graph_builder!=_graph_builder)details::error("sample source belongs to another builder"); _graph_builder->connect_sample_input({_index,PortKind::sample,i},std::move(source)); return _clone_handle(); }
template<class T> inline NodeRef NodeRef::connect_input(std::string_view n,T&& v) const { return connect_input(_graph_builder->sample_port_index(_index,true,n),std::forward<T>(v)); }
template<class... Args> inline NodeRef NodeRef::operator()(Args&&... args) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); size_t ps=0,pe=0; auto connect=[&](auto&& arg){using A=std::remove_cvref_t<decltype(arg)>; if constexpr(details::is_named_arg_v<A>){if constexpr(A::kind==NamedPortKind::sample)connect_input(_graph_builder->sample_port_index(_index,true,A::name.view()),std::forward<decltype(arg)>(arg).value);else connect_event_input(_graph_builder->event_port_index(_index,true,A::name.view()),static_cast<EventPortRef>(std::forward<decltype(arg)>(arg).value));}else if constexpr(std::convertible_to<A,EventPortRef>)connect_event_input(pe++,static_cast<EventPortRef>(std::forward<decltype(arg)>(arg)));else connect_input(ps++,std::forward<decltype(arg)>(arg));};(connect(std::forward<Args>(args)),...);return _clone_handle(); }
inline NodeRef NodeRef::connect_event_input(size_t i,EventPortRef value) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); if(value.graph_builder!=_graph_builder)details::error("event source belongs to another builder"); _graph_builder->connect_event_input({_index,PortKind::event,i},std::move(value)); return _clone_handle(); }
inline NodeRef NodeRef::connect_event_input(std::string_view n,EventPortRef v) const { return connect_event_input(_graph_builder->event_port_index(_index,true,n),std::move(v)); }
inline EventPortRef NodeRef::event_port(size_t i) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); return _graph_builder->event_output({_index,PortKind::event,i}); }
inline EventPortRef NodeRef::event_port(std::string_view n) const { return event_port(_graph_builder->event_port_index(_index,false,n)); }
inline EventPortRef NodeRef::event_port() const { if(event_output_count()!=1)details::error(to_string()+" does not have exactly 1 event output port"); return event_port(0); }
inline NodeRef NodeRef::ttl(size_t ttl_samples) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); _graph_builder->_node_bundles.apply_ttl(_index,ttl_samples); return _clone_handle(); }
inline NodeRef NodeRef::no_ttl() const { return ttl(std::numeric_limits<size_t>::max()); }
inline std::string NodeRef::to_string() const { return _graph_builder?"node bundle "+std::to_string(_index):"empty node"; }

inline NodeRef& NodeRef::operator=(NodeRef const& rhs) {
  if(this==&rhs)return *this;
  if(_allows_single_assignment){if(!rhs._graph_builder)details::error("cannot initialize a virtual-empty NodeRef from an empty NodeRef");_graph_builder=rhs._graph_builder;_index=rhs._index;if(!_virtual_declaration_id.empty())_graph_builder->_annotations.attach_virtual_node(_graph_builder->_node_bundles,_graph_builder->_virtual_nodes,_index,_virtual_declaration_id);_allows_single_assignment=false;return *this;}
  if(_graph_builder||!_virtual_declaration_id.empty())details::error(
    "cannot assign to NodeRef '" + _virtual_declaration_id +
    "' after it has already been initialized");
  _graph_builder=rhs._graph_builder;_index=rhs._index;_virtual_declaration_id=rhs._virtual_declaration_id;_allows_single_assignment=rhs._allows_single_assignment;return *this;
}
inline NodeRef& NodeRef::operator=(NodeRef&& rhs) {
  if(this==&rhs)return *this;
  if(_allows_single_assignment){if(!rhs._graph_builder)details::error("cannot initialize a virtual-empty NodeRef from an empty NodeRef");_graph_builder=rhs._graph_builder;_index=rhs._index;if(!_virtual_declaration_id.empty())_graph_builder->_annotations.attach_virtual_node(_graph_builder->_node_bundles,_graph_builder->_virtual_nodes,_index,_virtual_declaration_id);_allows_single_assignment=false;rhs._graph_builder=nullptr;rhs._index=0;rhs._virtual_declaration_id.clear();rhs._allows_single_assignment=false;return *this;}
  if(_graph_builder||!_virtual_declaration_id.empty())details::error(
    "cannot assign to NodeRef '" + _virtual_declaration_id +
    "' after it has already been initialized");
  _graph_builder=rhs._graph_builder;_index=rhs._index;_virtual_declaration_id=std::move(rhs._virtual_declaration_id);_allows_single_assignment=rhs._allows_single_assignment;rhs._graph_builder=nullptr;rhs._index=0;rhs._virtual_declaration_id.clear();rhs._allows_single_assignment=false;return *this;
}
inline void NodeRef::_annotate_source_info(std::string_view declaration_identity,std::string_view file_path,uint32_t begin,uint32_t end) const {
  if(!declaration_identity.empty())_virtual_declaration_id=declaration_identity;
  if(!_graph_builder)return;
  _graph_builder->_annotations.annotate_node_source_info(_graph_builder->_node_bundles,_graph_builder->_virtual_nodes,_graph_builder->_identity,_index,declaration_identity,file_path,begin,end);
}

template<class Node,class PortProjection> inline SamplePortRef TypedNodeRef<Node,PortProjection>::operator[](size_t i) const { return NodeRef::operator[](i); }
template<class Node,class PortProjection> inline SamplePortRef TypedNodeRef<Node,PortProjection>::operator[](std::string_view n) const { if(!_graph_builder)details::error("attempted to use a null NodeRef");auto outputs=get_outputs(ports());std::optional<size_t> match;for(size_t i=0;i<outputs.size();++i)if(outputs[i].name==n){if(match)details::error("output name is ambiguous");match=i;}if(match)return NodeRef::operator[](*match);details::error("output port does not exist on "+to_string()); }
template<class Node,class PortProjection> inline EventPortRef TypedNodeRef<Node,PortProjection>::event_port(size_t i) const { if(i>=ports().event_outputs().size())details::error("event output port out of bounds");return NodeRef::event_port(i); }
template<class Node,class PortProjection> inline EventPortRef TypedNodeRef<Node,PortProjection>::event_port(std::string_view n) const { auto const& outputs=ports().event_outputs();std::optional<size_t> match;for(size_t i=0;i<outputs.size();++i)if(outputs[i].name==n){if(match)details::error("event output name is ambiguous");match=i;}if(match)return NodeRef::event_port(*match);details::error("event output port does not exist on "+to_string()); }
template<class Node,class PortProjection> inline EventPortRef TypedNodeRef<Node,PortProjection>::event_port() const { if(ports().event_outputs().size()!=1)details::error(to_string()+" does not have exactly 1 event output port");return event_port(0); }
template<class Node,class PortProjection> inline TypedNodeRef<Node,PortProjection>::operator SamplePortRef() const { if(get_num_outputs(ports())!=1)details::error(to_string()+" does not have exactly 1 output port");return SamplePortRef(*_graph_builder,{_index,PortKind::sample,0}); }

template<class Node,class PortProjection>
template<class... Args>
  requires(details::node_call_enabled<std::remove_cvref_t<Node>,Args...>)
inline TypedNodeRef<Node,PortProjection> TypedNodeRef<Node,PortProjection>::operator()(Args&&... args) const {
  if(!_graph_builder)details::error("attempted to use a null NodeRef"); auto inputs=get_inputs(ports()); auto const& events=ports().event_inputs();
  auto sample=[&](SamplePortRef const& ref,size_t i){if(i>=inputs.size())details::error("too many sample inputs");if(ref.graph_builder!=_graph_builder)details::error("sample source belongs to another builder");_graph_builder->connect_sample_input({_index,PortKind::sample,i},ref);};
  auto event=[&](EventPortRef ref,size_t i){if(i>=events.size())details::error("too many event inputs");if(ref.graph_builder!=_graph_builder)details::error("event source belongs to another builder");_graph_builder->connect_event_input({_index,PortKind::event,i},ref);};
  size_t ps=0,pe=0; auto process=[&](auto&& arg){using A=std::remove_cvref_t<decltype(arg)>;if constexpr(details::is_named_arg_v<A>){if constexpr(A::kind==NamedPortKind::event){for(size_t i=0;i<events.size();++i)if(events[i].name==A::name.view()){event(lift_event_operand(arg.value),i);return;}details::error("named event input does not exist");}else{for(size_t i=0;i<inputs.size();++i)if(inputs[i].name==A::name.view()){sample(_graph_builder->lift_to_sample_port(arg.value),i);return;}details::error("named sample input does not exist");}}else if constexpr(details::graph_builder_event_port_like<decltype(arg)>)event(static_cast<EventPortRef>(std::forward<decltype(arg)>(arg)),pe++);else sample(_graph_builder->lift_to_sample_port(std::forward<decltype(arg)>(arg)),ps++);};
  (process(std::forward<Args>(args)),...); return this->_clone_handle();
}
template<class Node,class PortProjection> template<class T> inline TypedNodeRef<Node,PortProjection> TypedNodeRef<Node,PortProjection>::connect_input(size_t i,T&& value) const {auto inputs=get_inputs(ports());if(i>=inputs.size())details::error("sample input out of bounds");auto ref=_graph_builder->lift_to_sample_port(std::forward<T>(value));_graph_builder->connect_sample_input({_index,PortKind::sample,i},ref);return this->_clone_handle();}
template<class Node,class PortProjection> template<class T> inline TypedNodeRef<Node,PortProjection> TypedNodeRef<Node,PortProjection>::connect_input(std::string_view n,T&& value) const {auto inputs=get_inputs(ports());std::optional<size_t> m;for(size_t i=0;i<inputs.size();++i)if(inputs[i].name==n){if(m)details::error("input name is ambiguous");m=i;}if(m)return connect_input(*m,std::forward<T>(value));details::error("input port does not exist");}
template<class Node,class PortProjection> inline TypedNodeRef<Node,PortProjection> TypedNodeRef<Node,PortProjection>::connect_event_input(size_t i,EventPortRef value) const {if(i>=ports().event_inputs().size())details::error("event input out of bounds");_graph_builder->connect_event_input({_index,PortKind::event,i},value);return this->_clone_handle();}
template<class Node,class PortProjection> inline TypedNodeRef<Node,PortProjection> TypedNodeRef<Node,PortProjection>::connect_event_input(std::string_view n,EventPortRef value) const {auto const& inputs=ports().event_inputs();std::optional<size_t> m;for(size_t i=0;i<inputs.size();++i)if(inputs[i].name==n){if(m)details::error("event input name is ambiguous");m=i;}if(m)return connect_event_input(*m,value);details::error("event input port does not exist");}
template<class Node,class PortProjection> inline SamplePortRef TypedNodeRef<Node,PortProjection>::detach(size_t latency) const {return static_cast<SamplePortRef>(*this).detach(latency);}
} // namespace iv
