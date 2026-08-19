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
#include <intravenous/graph/builder/topology.h>
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
  template <class Derived> friend class NodeRefCrtp;
  template <class Node, class PortProjection> friend class TypedNodeRef;
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
  GraphBuilderTopology _topology;
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
  PublicEventInputRef event_input_named(std::string_view name,
                                        EventTypeId type);

public:
  GraphBuilder();

  GraphBuilder derive_nested_builder();
  bool inside_subgraph_scope() const;
  ScopedSubgraph &current_scope();
  void define_scope_outputs(std::span<OutputRefConfig const> refs);
  void define_scope_event_outputs(std::span<EventOutputRefConfig const> refs);
  std::string node_id(size_t index) const;
  PublicSampleInputRef input();
  template <fixed_string Name>
  PublicSampleInputRef input(Sample default_value = 0.0,
                             std::optional<Sample> min = std::nullopt,
                             std::optional<Sample> max = std::nullopt) {
    return input_named(Name.view(), default_value, min, max);
  }
  PublicSampleInputRef input(Sample default_value,
                             std::optional<Sample> min = std::nullopt,
                             std::optional<Sample> max = std::nullopt);
  template <fixed_string Name>
  PublicEventInputRef event_input(EventTypeId type) {
    return event_input_named(Name.view(), type);
  }
  PublicEventInputRef event_input(EventTypeId type);
  void annotate_public_sample_input_source_info(
      PublicSampleInputRef const &, std::string_view declaration_identity,
      std::string_view file_path, uint32_t begin, uint32_t end);
  void annotate_public_event_input_source_info(
      PublicEventInputRef const &, std::string_view declaration_identity,
      std::string_view file_path, uint32_t begin, uint32_t end);
  void
  annotate_public_sample_output_source_info(std::span<SourceInfo const> infos);
  void
  annotate_public_event_output_source_info(std::span<SourceInfo const> infos);

  template <class Config>
  static void validate_output_port_configs(std::span<Config const> configs,
                                           std::string_view node_label,
                                           std::string_view kind);

  template <class Node, class... Args>
  details::node_ref_for_t<Node> node(Args &&...args);

  template <class Node, class ChannelType, class... Args>
  auto node(Args &&...args);

  template <class ChannelType, class... Refs>
  auto tile(Refs &&...refs);

  NodeRef embed_subgraph(GraphBuilder const &child);

  template <class... Refs> void event_outputs(Refs &&...refs);

  void event_outputs(std::span<EventOutputRefConfig const> refs);

  template <class... Refs> void subgraph_event_outputs(Refs &&...refs);

  void subgraph_event_outputs(std::span<EventOutputRefConfig const> refs);

  template <class Fn>
  NodeRef subgraph(Fn &&fn, std::string_view kind = "Subgraph");

  template <class... Refs> void outputs(Refs &&...refs);

  void outputs(std::initializer_list<NamedRef> refs);
  void outputs(std::span<OutputRefConfig const> refs);
  void outputs(std::span<NamedRef const> refs);

  template <class... Refs> void subgraph_outputs(Refs &&...refs);

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
  std::span<SourceInfo const>
  public_event_input_source_infos(size_t port_ordinal) const;
  GraphBuilderPublicSamplePortFamilies public_sample_output_families() const;
  std::vector<GraphBuilderPublicEventOutput> public_event_outputs() const;
  void connect_sample_input(TopologyPortId target, SamplePortRef source);
  void connect_sample_input(NodeBundlePortId target, SamplePortRef source);
  // Record one scalar source per channel of a builder-visible port. Physical
  // packing/tiled projection is a completion-time lowering concern.
  void connect_sample_input(NodeBundlePortId target,
                            std::span<SamplePortRef const> sources);
  // Compatibility lowering hook. Ordinary authored sample operations must not
  // cross to topology through this API.
  MaterializedSamplePort materialize_sample_output(SamplePortRef source);
  void connect_event_input(TopologyPortId target, EventPortRef source);
  void connect_event_input(NodeBundlePortId target, EventPortRef source);
  void mark_runtime_filled_sample_input(TopologyPortId target);
  void mark_runtime_filled_sample_input(NodeBundlePortId target);
  void mark_runtime_filled_event_input(TopologyPortId target);
  void mark_runtime_filled_event_input(NodeBundlePortId target);
  bool sample_input_is_connected(NodeBundlePortId target) const;
  bool event_input_is_connected(NodeBundlePortId target) const;
  // Keep topology-port resolution inside GraphBuilder when a graph-service
  // node consumes a bundle output.
  void connect_sample_output(NodeBundlePortId source, NodeRef const &target);
  EventPortRef event_output(NodeBundlePortId source) const;
  size_t sample_port_index(NodeBundleHandle, bool inputs, std::string_view name) const;
  size_t event_port_index(NodeBundleHandle, bool inputs, std::string_view name) const;
  GraphIntrospectionMetadata build_metadata(size_t detach_id_offset = 0) const;
  RootNodeBuildResult build_root_node(size_t detach_id_offset = 0) const;
  RootNodeBuildResult
  build_execution_root_node(size_t detach_id_offset = 0) const;

private:
  static std::string allocate_root_builder_id();
  SamplePortRef detach_sample_port(SamplePortRef const &sample_port,
                                   size_t loop_extra_latency);
  void record_authored_sample_connection(NodeBundlePortId target,
                                         SamplePortRef const &source);
  void record_authored_sample_connection(SampleInputChannelId target,
                                         SamplePortRef const &source);
  void record_authored_sample_connection(
      NodeBundlePortId target, std::span<SamplePortRef const> sources);
  void record_authored_event_connection(
      NodeBundlePortId target, EventPortRef const& source);
  GraphBuilder completed_sample_builder() const;
  void materialize_authored_sample_connections_for_completion();
  void connect_sample_input(TopologyPortId target, MaterializedSamplePort source);
  // Compatibility lowering only. These overloads deliberately do not append
  // AuthoredSampleConnection records.
  void connect_sample_input_lowered(NodeBundlePortId target, SamplePortRef source);
  void connect_sample_input_lowered(
      NodeBundlePortId target, std::span<SamplePortRef const> sources);
  void connect_event_input_lowered(NodeBundlePortId target, EventPortRef source);
  std::vector<MaterializedSamplePort>
  materialize_bundle_sample_output_channels(NodeBundlePortId source);
  SamplePortRef normalize_sample_output(SamplePortRef source);
  SamplePortRef lift_to_sample_port(SamplePortRef const &sample_port);
  SamplePortRef lift_to_sample_port(SamplePortRef &&sample_port);

  template <class T>
    requires(!std::same_as<std::remove_cvref_t<T>, SamplePortRef> &&
             requires(std::remove_cvref_t<T> const& ref) { ref.node_ref(); } &&
             std::convertible_to<T, SamplePortRef>)
  SamplePortRef lift_to_sample_port(T &&sample_port) {
    return lift_to_sample_port(
        static_cast<SamplePortRef>(std::forward<T>(sample_port)));
  }

  template <class ChannelType, SampleStreamLayout Layout>
  SamplePortRef lift_to_sample_port(
      TypedSamplePortRef<ChannelType, Layout> const &sample_port);

  template <class ChannelType, SampleStreamLayout Layout, class Member>
  SamplePortRef
  lift_to_sample_port(TypedSamplePortChannelRef<ChannelType, Layout,
                                                Member> const &sample_port);

  template <class ChannelType, SampleStreamLayout Layout>
  SamplePortRef lift_to_sample_port(
      TypedSamplePortTileRef<ChannelType, Layout> const &sample_port);

  template <class T>
    requires std::is_arithmetic_v<std::remove_cvref_t<T>> ||
             std::is_same_v<std::remove_cvref_t<T>, Sample>
  SamplePortRef lift_to_sample_port(T value) {
    auto constant = node<Constant>(static_cast<Sample>(value));
    return static_cast<SamplePortRef>(constant);
  }

  SamplePortRef lift_to_sample_port(NamedRef const &ref);
};

template <class ChannelType, SampleStreamLayout Layout>
SamplePortRef GraphBuilder::lift_to_sample_port(
    TypedSamplePortRef<ChannelType, Layout> const &sample_port) {
  return lift_to_sample_port(static_cast<SamplePortRef>(sample_port));
}

template <class ChannelType, SampleStreamLayout Layout, class Member>
SamplePortRef GraphBuilder::lift_to_sample_port(
    TypedSamplePortChannelRef<ChannelType, Layout, Member> const &sample_port) {
  return lift_to_sample_port(sample_port.erased());
}

template <class ChannelType, SampleStreamLayout Layout>
SamplePortRef GraphBuilder::lift_to_sample_port(
    TypedSamplePortTileRef<ChannelType, Layout> const &sample_port) {
  return lift_to_sample_port(sample_port.erased());
}

template <class Config>
void GraphBuilder::validate_output_port_configs(std::span<Config const> configs,
                                                std::string_view node_label,
                                                std::string_view kind) {
  GraphBuilderTopology::validate_output_port_configs(configs, node_label, kind);
}

template <class Node, class... Args>
details::node_ref_for_t<Node> GraphBuilder::node(Args &&...args) {
  auto const concrete_node_index =
      _topology.insert_node<Node>(std::forward<Args>(args)...);
  auto const bundle_handle =
      _node_bundles.append_concrete(_topology, concrete_node_index);
  using StoredNode = std::remove_cvref_t<Node>;
  if constexpr (details::should_preserve_node_type_v<StoredNode>) {
    return TypedNodeRef<StoredNode>(*this, bundle_handle);
  } else {
    return NodeRef(*this, bundle_handle);
  }
}

template <class Node, class ChannelType, class... Args>
auto GraphBuilder::node(Args &&...args) {
  using StoredNode = std::remove_cvref_t<Node>;
  static_assert(details::has_constexpr_sample_port_configs<StoredNode>,
                "tiled DSP nodes must provide constexpr static inputs()/outputs() configurations");
  static_assert((std::copy_constructible<std::remove_cvref_t<Args>> && ...),
                "tiled-node construction requires reusable constructor arguments");
  static_assert([] consteval {
    if constexpr (requires { StoredNode::inputs(); }) {
      for (auto const &config : StoredNode::inputs()) {
        if (config.channel_layout.channel_type != ChannelTypeId::mono) {
          return false;
        }
      }
    }
    if constexpr (requires { StoredNode::outputs(); }) {
      for (auto const &config : StoredNode::outputs()) {
        if (config.channel_layout.channel_type != ChannelTypeId::mono) {
          return false;
        }
      }
    }
    return true;
  }(), "the initial tiled-node model only supports fully mono sample nodes");

  std::array<size_t, ChannelType::channel_count> concrete_node_indices{};
  std::array<NodeBundleHandle, ChannelType::channel_count> member_bundles{};
  for (size_t channel = 0; channel < ChannelType::channel_count; ++channel) {
    auto &concrete_node_index = concrete_node_indices[channel];
    concrete_node_index = _topology.insert_node<StoredNode>(args...);
    // A tiled ref owns these member handles.  They preserve the concrete node
    // type selected by `tiled[channel]`; the promoted bundle below is only
    // the common, builder-facing bundle used by NodeRef.
    member_bundles[channel] = _node_bundles.append_concrete(
        _topology, concrete_node_index);
  }
  auto const bundle_handle = _node_bundles.append_tiled(
      _topology, concrete_node_indices, member_bundles,
      ChannelLayout{.channel_type = ChannelTypeTraits<ChannelType>::id,
                    .sample_layout = SampleStreamLayout::planar});
  return TiledNodeRef<StoredNode, ChannelType>(
      *this, bundle_handle, std::move(member_bundles));
}

template <class ChannelType, class... Refs>
auto GraphBuilder::tile(Refs &&...refs) {
  static_assert(
      sizeof...(Refs) == ChannelType::channel_count,
      "g.tile<ChannelType>(...) requires exactly one source per channel");

  std::array<SamplePortRef, ChannelType::channel_count> members{
      lift_to_sample_port(std::forward<Refs>(refs))...};
  for (auto const &member : members) {
    if (member.channel_type != ChannelTypeId::mono || member.channels.size() != 1) {
      details::error("g.tile<ChannelType>(...) requires scalar sample sources");
    }
  }
  return TypedSamplePortTileRef<ChannelType>{std::move(members)};
}

template <class... Refs> void GraphBuilder::event_outputs(Refs &&...refs) {
  _public_ports.define_event_outputs_from_args(*this, _topology, _node_bundles, _identity,
                                               std::forward<Refs>(refs)...);
}

template <class... Refs>
void GraphBuilder::subgraph_event_outputs(Refs &&...refs) {
  if (!inside_subgraph_scope()) {
    details::error(
        "g.subgraph_event_outputs(...) is only valid inside g.subgraph(...)");
  }
  {
    std::vector<EventOutputRefConfig> output_refs;
    output_refs.reserve(sizeof...(Refs));
    constexpr bool require_names = (sizeof...(Refs) > 1);
    auto const append_ref = [&](auto &&ref) {
      using RefT = std::remove_cvref_t<decltype(ref)>;
      if constexpr (details::is_named_arg_v<RefT>) {
        output_refs.push_back(EventOutputRefConfig{
            .ref = static_cast<EventPortRef>(ref.value),
            .config = EventOutputConfig{.name = std::string(RefT::name.view())},
        });
      } else {
        if constexpr (require_names) {
          details::error("builder " + _identity.value +
                         ": event_outputs(...) requires names when exposing "
                         "more than one event output");
        } else {
          output_refs.push_back(EventOutputRefConfig{
              .ref = static_cast<EventPortRef>(ref),
              .config = EventOutputConfig{},
          });
        }
      }
    };
    (append_ref(std::forward<Refs>(refs)), ...);
    define_scope_event_outputs(std::span<EventOutputRefConfig const>(
        output_refs.data(), output_refs.size()));
  }
}

template <class Fn>
NodeRef GraphBuilder::subgraph(Fn &&fn, std::string_view kind) {
  return _subgraphs.run(*this, _topology, _node_bundles,
                        std::forward<Fn>(fn), kind);
}

template <class... Refs> void GraphBuilder::outputs(Refs &&...refs) {
  _public_ports.define_sample_outputs_from_args(
      *this, _topology, _node_bundles, _identity,
      [&](auto &&value) {
        return lift_to_sample_port(std::forward<decltype(value)>(value));
      },
      std::forward<Refs>(refs)...);
}

template <class... Refs> void GraphBuilder::subgraph_outputs(Refs &&...refs) {
  if (!inside_subgraph_scope()) {
    details::error(
        "g.subgraph_outputs(...) is only valid inside g.subgraph(...)");
  }
  {
    std::vector<OutputRefConfig> output_refs;
    output_refs.reserve(sizeof...(Refs));
    constexpr bool require_names = (sizeof...(Refs) > 1);
    auto const append_ref = [&](auto &&ref) {
      using RefT = std::remove_cvref_t<decltype(ref)>;
      if constexpr (details::is_channel_named_arg_v<RefT>) {
        output_refs.push_back(OutputRefConfig{
            .ref = lift_to_sample_port(ref.value),
            .config =
                OutputConfig{
                    .name = std::string(RefT::name.view()),
                    .channel_layout =
                        ChannelLayout{
                            .channel_type = ChannelTypeTraits<
                                typename RefT::channel_type>::id,
                            .sample_layout = SampleStreamLayout::planar,
                        },
                },
            .public_member =
                PublicSamplePortMember{
                    .family_name = std::string(RefT::name.view()),
                    .channel_type =
                        ChannelTypeTraits<typename RefT::channel_type>::id,
                    .channel_index = RefT::channel_ordinal,
                },
        });
      } else if constexpr (details::is_default_channel_named_arg_v<RefT>) {
        output_refs.push_back(OutputRefConfig{
            .ref = lift_to_sample_port(ref.value),
            .config =
                OutputConfig{
                    .name = "main",
                    .channel_layout =
                        ChannelLayout{
                            .channel_type = ChannelTypeTraits<
                                typename RefT::channel_type>::id,
                            .sample_layout = SampleStreamLayout::planar,
                        },
                },
            .public_member =
                PublicSamplePortMember{
                    .family_name = "main",
                    .channel_type =
                        ChannelTypeTraits<typename RefT::channel_type>::id,
                    .channel_index = RefT::channel_ordinal,
                },
        });
      } else if constexpr (details::is_named_arg_v<RefT>) {
        if constexpr (RefT::name.view().starts_with("__")) {
          details::error("builder " + _identity.value +
                         ": generated channel assignments are not public "
                         "outputs; use \"name\"_P[channel] = value");
        }
        using Value = std::remove_cvref_t<decltype(ref.value)>;
        if constexpr (requires {
            typename Value::channel_type;
            { Value::sample_layout } -> std::convertible_to<SampleStreamLayout>;
        }) {
          output_refs.push_back(OutputRefConfig{
              .ref = lift_to_sample_port(ref.value),
              .config = OutputConfig{
                  .name = std::string(RefT::name.view()),
                  .channel_layout = ChannelLayout{
                      .channel_type = ChannelTypeTraits<typename Value::channel_type>::id,
                      .sample_layout = Value::sample_layout,
                  },
              },
              .public_member = PublicSamplePortMember{
                  .family_name = std::string(RefT::name.view()),
                  .channel_type = ChannelTypeTraits<typename Value::channel_type>::id,
                  .whole_stream = true,
              },
          });
        } else {
          output_refs.push_back(OutputRefConfig{
              .ref = lift_to_sample_port(ref.value),
              .config = OutputConfig{
                  .name = std::string(RefT::name.view()),
                  .channel_layout = mono_planar_channel_layout,
              },
              .public_member = PublicSamplePortMember{
                  .family_name = std::string(RefT::name.view()),
              },
          });
        }
      } else {
        if constexpr (require_names) {
          details::error("builder " + _identity.value +
                         ": outputs(...) requires names when exposing more "
                         "than one sample output");
        } else {
          output_refs.push_back(OutputRefConfig{
              .ref = lift_to_sample_port(std::forward<decltype(ref)>(ref)),
              .config = OutputConfig{},
          });
        }
      }
    };
    (append_ref(std::forward<Refs>(refs)), ...);
    define_scope_outputs(std::span<OutputRefConfig const>(output_refs.data(),
                                                          output_refs.size()));
  }
}

template <class Node, class PortProjection>
inline NodePorts const &TypedNodeRef<Node, PortProjection>::ports() const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  return _graph_builder->_topology.ports(
      _graph_builder->_node_bundles.bundle(_index).single_concrete_node());
}

inline NodeRef NodeRef::node_ref() const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  return NodeRef(*_graph_builder, _index);
}

inline NodeRef NodeRef::_clone_handle() const {
  if (!_graph_builder) {
    return NodeRef{};
  }
  return NodeRef(*_graph_builder, _index);
}

inline SamplePortRef NodeRef::operator[](size_t output_port) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  auto const &bundle = _graph_builder->_node_bundles.bundle(_index);
  if (output_port >= bundle.sample_output_count()) {
    details::error("sample output port " + std::to_string(output_port) +
                   " is out of bounds on " + to_string());
  }
  return SamplePortRef(*_graph_builder,
                       NodeBundlePortId{_index, PortKind::sample, output_port});
}

inline SamplePortRef NodeRef::operator[](std::string_view output_name) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  return (*this)[_graph_builder->sample_port_index(_index, false, output_name)];
}

inline NodeRef::operator SamplePortRef() const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  if (_graph_builder->_node_bundles.bundle(_index).sample_output_count() != 1) {
    details::error(to_string() +
                   " does not have exactly 1 output port: it cannot be implicitly converted to SamplePortRef");
  }
  return (*this)[0];
}

inline size_t NodeRef::sample_input_count() const {
  auto const handle = node_bundle_handle();
  return _graph_builder->_node_bundles.bundle(handle).sample_input_count();
}

inline size_t NodeRef::sample_output_count() const {
  auto const handle = node_bundle_handle();
  return _graph_builder->_node_bundles.bundle(handle).sample_output_count();
}

inline size_t NodeRef::event_input_count() const {
  auto const handle = node_bundle_handle();
  return _graph_builder->_node_bundles.bundle(handle).event_input_count();
}

inline size_t NodeRef::event_output_count() const {
  auto const handle = node_bundle_handle();
  return _graph_builder->_node_bundles.bundle(handle).event_output_count();
}

inline bool NodeRef::input_is_connected(size_t input_port) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  return _graph_builder->sample_input_is_connected(
      NodeBundlePortId{_index, PortKind::sample, input_port});
}

inline bool NodeRef::event_input_is_connected(size_t input_port) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  return _graph_builder->event_input_is_connected(
      NodeBundlePortId{_index, PortKind::event, input_port});
}

template <class T>
inline NodeRef NodeRef::connect_input(size_t input_port, T &&value) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  auto source = _graph_builder->lift_to_sample_port(std::forward<T>(value));
  if (source.graph_builder != _graph_builder) {
    details::error(source.to_string() + " does not belong to the same builder as " +
                   to_string());
  }
  _graph_builder->connect_sample_input(
      NodeBundlePortId{_index, PortKind::sample, input_port}, std::move(source));
  return _clone_handle();
}

template <class T>
inline NodeRef NodeRef::connect_input(std::string_view input_name, T &&value) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  return connect_input(
      _graph_builder->sample_port_index(_index, true, input_name),
      std::forward<T>(value));
}

template <class... Args>
inline NodeRef NodeRef::operator()(Args &&...args) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  size_t positional_sample = 0;
  size_t positional_event = 0;
  auto connect = [&](auto &&arg) {
    using Arg = std::remove_cvref_t<decltype(arg)>;
    if constexpr (details::is_named_arg_v<Arg>) {
      if constexpr (Arg::kind == NamedPortKind::sample) {
        connect_input(_graph_builder->sample_port_index(
            _index, true, Arg::name.view()),
            std::forward<decltype(arg)>(arg).value);
      } else {
        connect_event_input(_graph_builder->event_port_index(
            _index, true, Arg::name.view()),
            static_cast<EventPortRef>(std::forward<decltype(arg)>(arg).value));
      }
    } else if constexpr (std::convertible_to<Arg, EventPortRef>) {
      connect_event_input(positional_event++,
                          static_cast<EventPortRef>(std::forward<decltype(arg)>(arg)));
    } else {
      connect_input(positional_sample++, std::forward<decltype(arg)>(arg));
    }
  };
  (connect(std::forward<Args>(args)), ...);
  return _clone_handle();
}

inline NodeRef NodeRef::connect_event_input(size_t input_port,
                                             EventPortRef value) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  if (value.graph_builder != _graph_builder) {
    details::error(value.to_string() + " does not belong to the same builder as " +
                   to_string());
  }
  _graph_builder->connect_event_input(
      NodeBundlePortId{_index, PortKind::event, input_port}, value);
  return _clone_handle();
}

inline NodeRef NodeRef::connect_event_input(std::string_view input_name,
                                             EventPortRef value) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  return connect_event_input(
      _graph_builder->event_port_index(_index, true, input_name),
      std::move(value));
}

inline EventPortRef NodeRef::event_port(size_t output_port) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  return _graph_builder->event_output(
      NodeBundlePortId{_index, PortKind::event, output_port});
}

inline EventPortRef NodeRef::event_port(std::string_view output_name) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  return event_port(_graph_builder->event_port_index(_index, false, output_name));
}

inline EventPortRef NodeRef::event_port() const {
  if (event_output_count() != 1) {
    details::error(to_string() + " does not have exactly 1 event output port");
  }
  return event_port(0);
}

inline NodeRef NodeRef::ttl(size_t ttl_samples) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  _graph_builder->_node_bundles.bundle(_index).for_each_topology_node(
      [&](size_t node) { _graph_builder->_topology.apply_ttl(node, ttl_samples); });
  return _clone_handle();
}

inline NodeRef NodeRef::no_ttl() const {
  return ttl(std::numeric_limits<size_t>::max());
}

inline std::string NodeRef::to_string() const {
  if (!_graph_builder) {
    return "empty node";
  }
  return "node bundle " + std::to_string(_index);
}

inline NodeRef &NodeRef::operator=(NodeRef const &rhs) {
  auto const &rhs_base = rhs;
  if (this == &rhs) {
    return *this;
  }

  if (_allows_single_assignment) {
    if (!rhs_base._graph_builder) {
      details::error(
          "cannot initialize a virtual-empty NodeRef from an empty NodeRef");
    }
    _graph_builder = rhs_base._graph_builder;
    _index = rhs_base._index;
    if (!_virtual_declaration_id.empty() && _graph_builder) {
      _graph_builder->_annotations.attach_virtual_node(
          _graph_builder->_topology, _graph_builder->_node_bundles,
          _graph_builder->_virtual_nodes, _index,
          _virtual_declaration_id);
    }
    _allows_single_assignment = false;
    return *this;
  }

  if (_graph_builder || !_virtual_declaration_id.empty()) {
    details::error("cannot assign to NodeRef '" + _virtual_declaration_id +
                   "' after it has already been initialized");
  }

  _graph_builder = rhs_base._graph_builder;
  _index = rhs_base._index;
  _virtual_declaration_id = rhs_base._virtual_declaration_id;
  _allows_single_assignment = rhs_base._allows_single_assignment;
  return *this;
}

inline NodeRef &NodeRef::operator=(NodeRef &&rhs) {
  auto &rhs_base = rhs;
  if (this == &rhs) {
    return *this;
  }

  if (_allows_single_assignment) {
    if (!rhs_base._graph_builder) {
      details::error(
          "cannot initialize a virtual-empty NodeRef from an empty NodeRef");
    }
    _graph_builder = rhs_base._graph_builder;
    _index = rhs_base._index;
    if (!_virtual_declaration_id.empty() && _graph_builder) {
      _graph_builder->_annotations.attach_virtual_node(
          _graph_builder->_topology, _graph_builder->_node_bundles,
          _graph_builder->_virtual_nodes, _index,
          _virtual_declaration_id);
    }
    _allows_single_assignment = false;
    rhs_base._graph_builder = nullptr;
    rhs_base._index = 0;
    rhs_base._virtual_declaration_id.clear();
    rhs_base._allows_single_assignment = false;
    return *this;
  }

  if (_graph_builder || !_virtual_declaration_id.empty()) {
    details::error("cannot assign to NodeRef '" + _virtual_declaration_id +
                   "' after it has already been initialized");
  }

  _graph_builder = rhs_base._graph_builder;
  _index = rhs_base._index;
  _virtual_declaration_id = std::move(rhs_base._virtual_declaration_id);
  _allows_single_assignment = rhs_base._allows_single_assignment;
  rhs_base._graph_builder = nullptr;
  rhs_base._index = 0;
  rhs_base._virtual_declaration_id.clear();
  rhs_base._allows_single_assignment = false;
  return *this;
}

inline void NodeRef::_annotate_source_info(
    std::string_view declaration_identity, std::string_view file_path,
    uint32_t begin, uint32_t end) const {
  if (!declaration_identity.empty()) {
    _virtual_declaration_id = declaration_identity;
  }
  if (!_graph_builder) {
    return;
  }
  _graph_builder->_annotations.annotate_node_source_info(
      _graph_builder->_topology, _graph_builder->_node_bundles,
      _graph_builder->_virtual_nodes,
      _graph_builder->_identity, _index, declaration_identity,
      file_path, begin, end);
}

template <class Node, class PortProjection>
inline SamplePortRef
TypedNodeRef<Node, PortProjection>::operator[](size_t output_index) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  return NodeRef::operator[](output_index);
}

template <class Node, class PortProjection>
inline SamplePortRef
TypedNodeRef<Node, PortProjection>::operator[](std::string_view output_name) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  auto outputs = get_outputs(ports());
  std::optional<size_t> matched_output;
  for (size_t output_port = 0; output_port < outputs.size(); ++output_port) {
    if (outputs[output_port].name == output_name) {
      if (matched_output.has_value()) {
        details::error("output name '" + std::string(output_name) +
                       "' is ambiguous on " + to_string());
      }
      matched_output = output_port;
    }
  }

  if (matched_output.has_value()) {
    return NodeRef::operator[](*matched_output);
  }

  details::error("an output port named '" + std::string(output_name) +
                 "' "
                 "does not exist on " +
                 to_string());
}

template <class Node, class PortProjection>
inline EventPortRef
TypedNodeRef<Node, PortProjection>::event_port(size_t output_index) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }

  auto const &outputs = ports().event_outputs();
  if (output_index >= outputs.size()) {
    details::error("event output port " + std::to_string(output_index) +
                   " of " + to_string() + " is out of bounds");
  }
  return NodeRef::event_port(output_index);
}

template <class Node, class PortProjection>
inline EventPortRef
TypedNodeRef<Node, PortProjection>::event_port(std::string_view output_name) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }

  auto const &outputs = ports().event_outputs();
  std::optional<size_t> matched_output;
  for (size_t output_port = 0; output_port < outputs.size(); ++output_port) {
    if (outputs[output_port].name == output_name) {
      if (matched_output.has_value()) {
        details::error("event output name '" + std::string(output_name) +
                       "' is ambiguous on " + to_string());
      }
      matched_output = output_port;
    }
  }

  if (matched_output.has_value()) {
    return NodeRef::event_port(*matched_output);
  }

  details::error("an event output port named '" + std::string(output_name) +
                 "' "
                 "does not exist on " +
                 to_string());
}

template <class Node, class PortProjection>
inline EventPortRef TypedNodeRef<Node, PortProjection>::event_port() const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }

  auto const &outputs = ports().event_outputs();
  if (outputs.size() != 1) {
    details::error(to_string() + " "
                                 "does not have exactly 1 event output port");
  }
  return event_port(0);
}

template <class Node, class PortProjection>
inline TypedNodeRef<Node, PortProjection>::operator SamplePortRef() const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }
  if (get_num_outputs(ports()) != 1) {
    details::error(to_string() +
                   " "
                   "does not have exactly 1 output port: it cannot be "
                   "implicitly converted to SamplePortRef");
  }
  return SamplePortRef(*_graph_builder,
                       NodeBundlePortId{_index, PortKind::sample, 0});
}

template <class Node, class PortProjection>
template <class... Args>
  requires(details::node_call_enabled<std::remove_cvref_t<Node>, Args...>)
inline TypedNodeRef<Node, PortProjection> TypedNodeRef<Node, PortProjection>::operator()(Args &&...args) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }

  auto const inputs = get_inputs(ports());
  auto const &event_inputs = ports().event_inputs();

  auto connect_input = [&](SamplePortRef const &ref, size_t input_port) {
    if (input_port >= inputs.size()) {
      details::error(to_string() +
                     " "
                     "has at most " +
                     std::to_string(inputs.size()) +
                     " inputs, "
                     "got " +
                     std::to_string(input_port + 1));
    }

    if (ref.graph_builder != _graph_builder) {
      details::error(ref.to_string() +
                     " "
                     "does not belong to the same builder as " +
                     to_string());
    }

    _graph_builder->connect_sample_input(
        NodeBundlePortId{_index, PortKind::sample, input_port}, ref);
  };

  auto connect_event = [&](EventPortRef ref, size_t input_port) {
    if (input_port >= event_inputs.size()) {
      details::error(to_string() +
                     " "
                     "has at most " +
                     std::to_string(event_inputs.size()) +
                     " event inputs, "
                     "got " +
                     std::to_string(input_port + 1));
    }
    if (ref.graph_builder != _graph_builder) {
      details::error(ref.to_string() +
                     " "
                     "does not belong to the same builder as " +
                     to_string());
    }

    _graph_builder->connect_event_input(
        NodeBundlePortId{_index, PortKind::event, input_port}, ref);
  };

  auto connect_named = [&](auto &&named_arg) {
    using NamedArgT = std::remove_cvref_t<decltype(named_arg)>;
    constexpr std::string_view name = NamedArgT::name.view();

    if constexpr (NamedArgT::kind == NamedPortKind::event) {
      for (size_t input_port = 0; input_port < event_inputs.size();
           ++input_port) {
        if (event_inputs[input_port].name == name) {
          connect_event(lift_event_operand(named_arg.value), input_port);
          return;
        }
      }
    } else {
      for (size_t input_port = 0; input_port < inputs.size(); ++input_port) {
        if (inputs[input_port].name == name) {
          connect_input(_graph_builder->lift_to_sample_port(named_arg.value),
                        input_port);
          return;
        }
      }
    }

    details::error("an input port named '" + std::string(name) +
                   "' "
                   "does not exist on " +
                   to_string());
  };

  size_t positional_sample_port = 0;
  size_t positional_event_port = 0;
  auto const process_arg = [&](auto &&arg) {
    if constexpr (details::is_named_arg_v<decltype(arg)>) {
      connect_named(std::forward<decltype(arg)>(arg));
    } else if constexpr (details::graph_builder_event_port_like<
                             decltype(arg)>) {
      connect_event(static_cast<EventPortRef>(std::forward<decltype(arg)>(arg)),
                    positional_event_port++);
    } else {
      connect_input(
          _graph_builder->lift_to_sample_port(std::forward<decltype(arg)>(arg)),
          positional_sample_port++);
    }
  };

  (process_arg(std::forward<Args>(args)), ...);

  return this->_clone_handle();
}

template <class Node, class PortProjection>
template <class T>
inline TypedNodeRef<Node, PortProjection> TypedNodeRef<Node, PortProjection>::connect_input(size_t input_port,
                                                         T &&value) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }

  auto const inputs = get_inputs(ports());
  if (input_port >= inputs.size()) {
    details::error(to_string() +
                   " "
                   "has at most " +
                   std::to_string(inputs.size()) +
                   " inputs, "
                   "got " +
                   std::to_string(input_port + 1));
  }

  SamplePortRef ref =
      _graph_builder->lift_to_sample_port(std::forward<T>(value));
  if (ref.graph_builder != _graph_builder) {
    details::error(ref.to_string() +
                   " "
                   "does not belong to the same builder as " +
                   to_string());
  }

  _graph_builder->connect_sample_input(
      NodeBundlePortId{_index, PortKind::sample, input_port}, ref);
  return this->_clone_handle();
}

template <class Node, class PortProjection>
template <class T>
inline TypedNodeRef<Node, PortProjection>
TypedNodeRef<Node, PortProjection>::connect_input(std::string_view input_name,
                                          T &&value) const {
  auto const inputs = get_inputs(ports());
  std::optional<size_t> matched_input;
  for (size_t input_port = 0; input_port < inputs.size(); ++input_port) {
    if (inputs[input_port].name == input_name) {
      if (matched_input.has_value()) {
        details::error("input name '" + std::string(input_name) +
                       "' is ambiguous on " + to_string());
      }
      matched_input = input_port;
    }
  }

  if (matched_input.has_value()) {
    return connect_input(*matched_input, std::forward<T>(value));
  }

  details::error("an input port named '" + std::string(input_name) +
                 "' "
                 "does not exist on " +
                 to_string());
}

template <class Node, class PortProjection>
inline TypedNodeRef<Node, PortProjection>
TypedNodeRef<Node, PortProjection>::connect_event_input(size_t input_port,
                                                EventPortRef value) const {
  if (!_graph_builder) {
    details::error("attempted to use a null NodeRef");
  }

  auto const &event_inputs = ports().event_inputs();
  if (input_port >= event_inputs.size()) {
    details::error(to_string() +
                   " "
                   "has at most " +
                   std::to_string(event_inputs.size()) +
                   " event inputs, "
                   "got " +
                   std::to_string(input_port + 1));
  }
  if (value.graph_builder != _graph_builder) {
    details::error(value.to_string() +
                   " "
                   "does not belong to the same builder as " +
                   to_string());
  }

  _graph_builder->connect_event_input(
      NodeBundlePortId{_index, PortKind::event, input_port}, value);
  return this->_clone_handle();
}

template <class Node, class PortProjection>
inline TypedNodeRef<Node, PortProjection>
TypedNodeRef<Node, PortProjection>::connect_event_input(std::string_view input_name,
                                                EventPortRef value) const {
  auto const &event_inputs = ports().event_inputs();
  std::optional<size_t> matched_input;
  for (size_t input_port = 0; input_port < event_inputs.size(); ++input_port) {
    if (event_inputs[input_port].name == input_name) {
      if (matched_input.has_value()) {
        details::error("event input name '" + std::string(input_name) +
                       "' is ambiguous on " + to_string());
      }
      matched_input = input_port;
    }
  }

  if (matched_input.has_value()) {
    return connect_event_input(*matched_input, value);
  }

  details::error("an event input port named '" + std::string(input_name) +
                 "' "
                 "does not exist on " +
                 to_string());
}

template <class Node, class PortProjection>
inline SamplePortRef
TypedNodeRef<Node, PortProjection>::detach(size_t loop_extra_latency) const {
  return static_cast<SamplePortRef>(*this).detach(loop_extra_latency);
}

} // namespace iv
