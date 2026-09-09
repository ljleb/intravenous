#pragma once

// Module-facing builder facade. It owns no graph containers: all mutable
// graph state belongs to the opaque BuilderSession in libiv_builder.

#include <intravenous/basic_nodes/constant.h>
#include <intravenous/channel_ports.h>
#include <intravenous/graph/builder/node_refs.h>
#include <intravenous/graph/builder/output_refs.h>
#include <intravenous/graph/builder/subgraphs.hpp>
#include <intravenous/graph/source_info.h>
#include <intravenous/node/build_request.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace iv {
struct AuthoredGraph;
class GraphBuilder;
class GraphBuilderState;

namespace details {
struct BuilderSession;
GraphBuilderState& builder_graph_state(GraphBuilder&);
NodeBundleHandle iv_builder_append_node(
    GraphBuilder&, NodeBuildRequest const&);
NodeBundleHandle iv_builder_append_tiled_node(
    GraphBuilder&, NodeBuildRequest const&, ChannelLayout);
void* iv_builder_allocate_node_config(
    BuilderSession*, std::size_t size, std::size_t alignment);
void iv_builder_discard_node_config(BuilderSession*, void* storage) noexcept;

}

class GraphBuilder {
  friend struct details::BuilderSession;
  friend GraphBuilderState& details::builder_graph_state(GraphBuilder&);
  friend NodeBundleHandle details::iv_builder_append_node(
      GraphBuilder&, details::NodeBuildRequest const&);
  friend NodeBundleHandle details::iv_builder_append_tiled_node(
      GraphBuilder&, details::NodeBuildRequest const&, ChannelLayout);
  friend class SubgraphBuilder;

  details::BuilderSession* _session = nullptr;
  bool _owns_session = false;

  explicit GraphBuilder(details::BuilderSession*, bool owns_session) noexcept;

public:
  GraphBuilder();
  explicit GraphBuilder(details::BuilderSession*) noexcept;
  ~GraphBuilder();
  GraphBuilder(GraphBuilder const&) = delete;
  GraphBuilder& operator=(GraphBuilder const&) = delete;
  GraphBuilder(GraphBuilder&&) noexcept;
  GraphBuilder& operator=(GraphBuilder&&) noexcept;

  bool valid() const noexcept { return _session != nullptr; }

  PublicSampleInputRef input();
  template<fixed_string Name, class ChannelType = mono>
  TypedPublicSampleInputRef<ChannelType> input(Sample default_value = 0.0,
      std::optional<Sample> min = std::nullopt,
      std::optional<Sample> max = std::nullopt) {
    return TypedPublicSampleInputRef<ChannelType>{input_named(Name.view(), {
        .channel_type = ChannelTypeTraits<ChannelType>::id,
        .sample_layout = SampleStreamLayout::planar,
      }, default_value, min, max)};
  }
  PublicSampleInputRef input(Sample default_value,
      std::optional<Sample> min = std::nullopt,
      std::optional<Sample> max = std::nullopt);
  template<fixed_string Name>
  PublicEventInputRef event_input(EventTypeId type) {
    return event_input_named(Name.view(), type);
  }
  PublicEventInputRef event_input(EventTypeId type);

  template<class Node, class... Args>
  details::node_ref_for_t<Node> node(Args&&... args) {
    using StoredNode = std::remove_cvref_t<Node>;
    static_assert(std::is_trivially_copyable_v<StoredNode>,
        "node values must be trivially copyable");
    auto* value = static_cast<StoredNode*>(
        details::iv_builder_allocate_node_config(
            _session, sizeof(StoredNode), alignof(StoredNode)));
    try {
      std::construct_at(value, std::forward<Args>(args)...);
      auto handle = details::iv_builder_append_node(
          *this, details::make_node_build_request(*value));
      if constexpr (details::should_preserve_node_type_v<StoredNode>)
        return TypedNodeRef<StoredNode>(*this, handle);
      else
        return NodeRef(*this, handle);
    } catch (...) {
      details::iv_builder_discard_node_config(_session, value);
      throw;
    }
  }

  template<class Node, class ChannelType, class... Args>
  auto node(Args&&... args) {
    using StoredNode = std::remove_cvref_t<Node>;
    static_assert(std::is_trivially_copyable_v<StoredNode>,
        "node values must be trivially copyable");
    static_assert((std::copy_constructible<std::remove_cvref_t<Args>> && ...),
        "tiled-node construction requires reusable constructor arguments");
    auto* value = static_cast<StoredNode*>(
        details::iv_builder_allocate_node_config(
            _session, sizeof(StoredNode), alignof(StoredNode)));
    try {
      std::construct_at(value, args...);
      auto handle = details::iv_builder_append_tiled_node(
          *this, details::make_node_build_request(*value), {
            .channel_type = ChannelTypeTraits<ChannelType>::id,
            .sample_layout = SampleStreamLayout::planar,
          });
      return TiledNodeRef<StoredNode, ChannelType>(*this, handle);
    } catch (...) {
      details::iv_builder_discard_node_config(_session, value);
      throw;
    }
  }

  template<class ChannelType, class... Refs>
  auto tile(Refs&&... refs) {
    static_assert(sizeof...(Refs) == ChannelType::channel_count,
        "g.tile<ChannelType>(...) requires exactly one source per channel");
    std::array<SamplePortRef, ChannelType::channel_count> members{
        lift_to_sample_port(std::forward<Refs>(refs))...};
    for (auto const& member : members)
      if (member.channel_type != ChannelTypeId::mono || member.channels().size() != 1)
        details::error("g.tile<ChannelType>(...) requires scalar sample sources");
    return TypedSamplePortTileRef<ChannelType>{std::move(members)};
  }

  template<class... Refs>
  void outputs(Refs&&... refs);
  void outputs(std::initializer_list<NamedRef>);
  void outputs(std::span<NamedRef const>);
  void outputs(std::span<SampleOutputRequest const>);

  template<class... Refs>
  void event_outputs(Refs&&... refs);
  void event_outputs(std::span<EventOutputRequest const>);

  template<auto Module>
  NodeRef module(std::string_view kind = "Module") {
    static_assert(std::invocable<decltype(Module), GraphBuilder&>);
    static_assert(std::same_as<std::invoke_result_t<decltype(Module), GraphBuilder&>, void>);
    GraphBuilder child;
    std::invoke(Module, child);
    return embed_child(child, kind);
  }

  template<class Fn>
  NodeRef subgraph(Fn&& fn, std::string_view kind = "Subgraph") {
    auto* scope = begin_subgraph();
    if constexpr (std::invocable<Fn, SubgraphBuilder&>) {
      try {
        SubgraphBuilder subgraph(*this, scope);
        std::invoke(std::forward<Fn>(fn), subgraph);
      } catch (...) {
        abandon_subgraph(scope);
        throw;
      }
    } else if constexpr (std::invocable<Fn>) {
      try {
        std::invoke(std::forward<Fn>(fn));
      } catch (...) {
        abandon_subgraph(scope);
        throw;
      }
    } else {
      abandon_subgraph(scope);
      static_assert(std::invocable<Fn>,
        "subgraph callback must accept SubgraphBuilder& or no arguments");
    }
    return finish_subgraph(scope, kind);
  }

  SamplePortRef lift_to_sample_port(SamplePortRef const&);
  SamplePortRef lift_to_sample_port(SamplePortRef&&);
  template<class T>
    requires (!std::same_as<std::remove_cvref_t<T>, SamplePortRef> &&
      requires(std::remove_cvref_t<T> const& ref) { ref.node_ref(); } &&
      std::convertible_to<T, SamplePortRef>)
  SamplePortRef lift_to_sample_port(T&& value) {
    return lift_to_sample_port(static_cast<SamplePortRef>(std::forward<T>(value)));
  }
  template<class ChannelType>
  SamplePortRef lift_to_sample_port(TypedSamplePortRef<ChannelType> const& value) {
    return lift_to_sample_port(static_cast<SamplePortRef>(value));
  }
  template<class ChannelType, class Member>
  SamplePortRef lift_to_sample_port(TypedSamplePortChannelRef<ChannelType, Member> const& value) {
    return lift_to_sample_port(value.erased());
  }
  template<class ChannelType>
  SamplePortRef lift_to_sample_port(TypedSamplePortTileRef<ChannelType> const& value) {
    return lift_to_sample_port(value.erased());
  }
  template<class T>
    requires (std::is_arithmetic_v<std::remove_cvref_t<T>> ||
      std::same_as<std::remove_cvref_t<T>, Sample>)
  SamplePortRef lift_to_sample_port(T value) {
    return static_cast<SamplePortRef>(node<Constant>(static_cast<Sample>(value)));
  }
  SamplePortRef lift_to_sample_port(NamedRef const&);

  PublicSampleInputRef input_named(std::string_view, Sample,
      std::optional<Sample>, std::optional<Sample>);
  PublicSampleInputRef input_named(std::string_view, ChannelLayout, Sample,
      std::optional<Sample>, std::optional<Sample>);
  PublicEventInputRef event_input_named(std::string_view, EventTypeId);
  void connect_sample_input(NodeBundlePortId, SamplePortRef);
  void connect_sample_input(NodeBundlePortId, std::span<SamplePortRef const>);
  void connect_event_input(NodeBundlePortId, EventPortRef);
  bool sample_input_is_connected(NodeBundlePortId) const;
  bool event_input_is_connected(NodeBundlePortId) const;
  void connect_sample_output(NodeBundlePortId, NodeRef const&);
  EventPortRef event_output(NodeBundlePortId) const;
  size_t sample_port_index(NodeBundleHandle, bool inputs, std::string_view) const;
  size_t event_port_index(NodeBundleHandle, bool inputs, std::string_view) const;
  size_t sample_input_count(NodeBundleHandle) const;
  size_t sample_output_count(NodeBundleHandle) const;
  size_t event_input_count(NodeBundleHandle) const;
  size_t event_output_count(NodeBundleHandle) const;
  NodeBundleHandle tiled_member(NodeBundleHandle, size_t) const;
  NodePorts const& typed_ports(NodeBundleHandle) const;
  SamplePortRef sample_port_from_output(NodeBundlePortId);
  EventPortRef event_port_from_output(NodeBundlePortId) const;
  SamplePortRef make_sample_port(ChannelTypeId, std::span<SampleOutputChannelId const>);
  std::span<SampleOutputChannelId const> sample_port_channels(SamplePortRef const&) const;
  EventPortRef make_event_port(EventTypeId, std::span<EventOutputPortId const>);
  std::span<EventOutputPortId const> event_port_sources(EventPortRef const&) const;
  SamplePortRef detach_sample_port(SamplePortRef const&, size_t);
  void apply_ttl(NodeBundleHandle, size_t);
  void annotate_node(NodeBundleHandle, std::string_view, std::string_view,
      uint32_t, uint32_t);
  void annotate_public_sample_input_source_info(PublicSampleInputRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_public_event_input_source_info(PublicEventInputRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_sample_port_source_info(SamplePortRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_event_port_source_info(EventPortRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_public_sample_output_source_info(std::span<SourceInfo const>);
  void annotate_public_event_output_source_info(std::span<SourceInfo const>);
  void annotate_public_sample_output_source_info(size_t, SourceInfo);
  void annotate_public_event_output_source_info(size_t, SourceInfo);
  AuthoredGraph finish() const &;
  AuthoredGraph finish() &&;

private:
  NodeRef embed_child(GraphBuilder&, std::string_view);
  details::SubgraphBuildScope* begin_subgraph();
  NodeRef finish_subgraph(details::SubgraphBuildScope*, std::string_view);
  void abandon_subgraph(details::SubgraphBuildScope*) noexcept;
  PublicSampleInputRef subgraph_input(
      details::SubgraphBuildScope*, std::string_view, ChannelLayout,
      Sample, std::optional<Sample>, std::optional<Sample>);
  PublicEventInputRef subgraph_event_input(
      details::SubgraphBuildScope*, std::string_view, EventTypeId);
  void subgraph_outputs(
      details::SubgraphBuildScope*, std::span<SampleOutputRequest const>);
  void subgraph_outputs(
      details::SubgraphBuildScope*, std::span<NamedRef const>);
  void subgraph_event_outputs(
      details::SubgraphBuildScope*, std::span<EventOutputRequest const>);
};

namespace details {
template<class... Args>
auto make_node_call_requests(GraphBuilder& builder, Args&&... args)
{
  using Requests = NodeCallRequests<
      sample_input_arg_count_v<Args...>, event_input_arg_count_v<Args...>>;
  Requests requests;
  size_t sample_index = 0;
  size_t event_index = 0;

  auto append = [&]<class Arg>(Arg&& arg) {
    using Value = std::remove_cvref_t<Arg>;
    if constexpr (is_named_arg_v<Value>) {
      if constexpr (Value::kind == NamedPortKind::sample) {
        requests.sample_inputs[sample_index++] = {
            .source = builder.lift_to_sample_port(
                std::forward<Arg>(arg).value),
            .name = Value::name.view(),
            .input_ordinal = 0,
            .target = NodeCallInputTarget::named,
        };
      } else {
        requests.event_inputs[event_index++] = {
            .source = lift_node_call_event_operand(
                std::forward<Arg>(arg).value),
            .name = Value::name.view(),
            .input_ordinal = 0,
            .target = NodeCallInputTarget::named,
        };
      }
    } else if constexpr (graph_builder_event_port_like<Arg>) {
      requests.event_inputs[event_index++] = {
          .source = static_cast<EventPortRef>(std::forward<Arg>(arg)),
          .name = {},
          .input_ordinal = 0,
          .target = NodeCallInputTarget::positional,
      };
    } else {
      requests.sample_inputs[sample_index++] = {
          .source = builder.lift_to_sample_port(std::forward<Arg>(arg)),
          .name = {},
          .input_ordinal = 0,
          .target = NodeCallInputTarget::positional,
      };
    }
  };
  (append(std::forward<Args>(args)), ...);
  return requests;
}

template<class Node, class... Args>
auto make_tiled_node_call_requests(GraphBuilder& builder, Args&&... args)
    -> NodeCallRequests<
        tiled_sample_input_arg_count_v<Args...>,
        tiled_event_input_arg_count_v<Args...>>
{
  using Requests = NodeCallRequests<
      tiled_sample_input_arg_count_v<Args...>,
      tiled_event_input_arg_count_v<Args...>>;
  Requests requests;
  size_t positional_sample = 0;
  size_t sample_index = 0;
  size_t event_index = 0;

  auto append = [&]<class Arg>(Arg&& arg) {
    using Value = std::remove_cvref_t<Arg>;
    if constexpr (is_named_arg_v<Value>) {
      if constexpr (Value::kind == NamedPortKind::sample) {
        constexpr auto input_ordinal =
            static_input_port_index<Node, Value::name>();
        requests.sample_inputs[sample_index++] = {
            .source = builder.lift_to_sample_port(
                std::forward<Arg>(arg).value),
            .name = {},
            .input_ordinal = input_ordinal,
            .target = NodeCallInputTarget::explicit_ordinal,
        };
      } else {
        requests.event_inputs[event_index++] = {
            .source = lift_node_call_event_operand(
                std::forward<Arg>(arg).value),
            .name = Value::name.view(),
            .input_ordinal = 0,
            .target = NodeCallInputTarget::named,
        };
      }
    } else {
      requests.sample_inputs[sample_index++] = {
          .source = builder.lift_to_sample_port(std::forward<Arg>(arg)),
          .name = {},
          .input_ordinal = positional_sample++,
          .target = NodeCallInputTarget::explicit_ordinal,
      };
    }
  };
  (append(std::forward<Args>(args)), ...);
  return requests;
}

template<class Sink, class... Refs>
inline void author_sample_output_requests(
    GraphBuilder& builder, Sink&& sink, Refs&&... refs) {
  constexpr size_t count = sizeof...(Refs);
  std::array<SampleOutputRequest, count> requests{};
  size_t index = 0;
  auto append = [&]<class Ref>(Ref&& ref) {
    using Value = std::remove_cvref_t<Ref>;
    auto& request = requests[index++];
    if constexpr (is_channel_named_arg_v<Value>) {
      using Channel = typename Value::channel_type;
      request.ref = builder.lift_to_sample_port(ref.value);
      request.name = Value::name.view();
      request.channel_layout = {.channel_type = ChannelTypeTraits<Channel>::id,
        .sample_layout = SampleStreamLayout::planar};
      request.family_name = Value::name.view();
      request.family_channel_type = ChannelTypeTraits<Channel>::id;
      request.target_channel_ordinal = Value::channel_ordinal;
    } else if constexpr (is_default_channel_named_arg_v<Value>) {
      using Channel = typename Value::channel_type;
      request.ref = builder.lift_to_sample_port(ref.value);
      request.name = "main";
      request.channel_layout = {.channel_type = ChannelTypeTraits<Channel>::id,
        .sample_layout = SampleStreamLayout::planar};
      request.family_name = "main";
      request.family_channel_type = ChannelTypeTraits<Channel>::id;
      request.target_channel_ordinal = Value::channel_ordinal;
    } else if constexpr (is_named_arg_v<Value>) {
      if constexpr (Value::name.view().starts_with("__"))
        error("generated channel assignments are not public outputs");
      request.ref = builder.lift_to_sample_port(ref.value);
      request.name = Value::name.view();
      request.channel_layout = {.channel_type = request.ref.channel_type,
        .sample_layout = SampleStreamLayout::planar};
      request.family_name = Value::name.view();
      request.family_channel_type = request.ref.channel_type;
    } else {
      static_assert(count == 1,
        "outputs(...) requires names when exposing more than one sample output");
      request.ref = builder.lift_to_sample_port(std::forward<Ref>(ref));
      request.name = "main";
      request.channel_layout = {.channel_type = request.ref.channel_type,
        .sample_layout = SampleStreamLayout::planar};
      request.family_name = "main";
      request.family_channel_type = request.ref.channel_type;
    }
  };
  (append(std::forward<Refs>(refs)), ...);
  std::invoke(std::forward<Sink>(sink),
      std::span<SampleOutputRequest const>(requests));
}

template<class Sink, class... Refs>
inline void author_event_output_requests(
    Sink&& sink, Refs&&... refs) {
  constexpr size_t count = sizeof...(Refs);
  std::array<EventOutputRequest, count> requests{};
  size_t index = 0;
  auto append = [&]<class Ref>(Ref&& ref) {
    using Value = std::remove_cvref_t<Ref>;
    auto& request = requests[index++];
    if constexpr (is_named_arg_v<Value>) {
      request.ref = static_cast<EventPortRef>(ref.value);
      request.name = Value::name.view();
    } else {
      static_assert(count == 1,
        "event_outputs(...) requires names when exposing more than one event output");
      request.ref = static_cast<EventPortRef>(std::forward<Ref>(ref));
    }
  };
  (append(std::forward<Refs>(refs)), ...);
  std::invoke(std::forward<Sink>(sink),
      std::span<EventOutputRequest const>(requests));
}
} // namespace details

template<class... Refs>
inline void GraphBuilder::outputs(Refs&&... refs) {
  details::author_sample_output_requests(
      *this,
      [this](std::span<SampleOutputRequest const> requests) {
        outputs(requests);
      },
      std::forward<Refs>(refs)...);
}

template<class... Refs>
inline void GraphBuilder::event_outputs(Refs&&... refs) {
  details::author_event_output_requests(
      [this](std::span<EventOutputRequest const> requests) {
        event_outputs(requests);
      },
      std::forward<Refs>(refs)...);
}

inline PublicSampleInputRef SubgraphBuilder::input() {
  return _builder.subgraph_input(
      _scope, {}, {.channel_type = ChannelTypeId::mono,
                   .sample_layout = SampleStreamLayout::planar},
      Sample{0.0f}, std::nullopt, std::nullopt);
}
template<fixed_string Name, class ChannelType>
inline TypedPublicSampleInputRef<ChannelType> SubgraphBuilder::input(
    Sample value, std::optional<Sample> min, std::optional<Sample> max) {
  return TypedPublicSampleInputRef<ChannelType>{_builder.subgraph_input(
      _scope, Name.view(), {.channel_type = ChannelTypeTraits<ChannelType>::id,
                            .sample_layout = SampleStreamLayout::planar},
      value, min, max)};
}
inline PublicSampleInputRef SubgraphBuilder::input(
    Sample value, std::optional<Sample> min, std::optional<Sample> max) {
  return _builder.subgraph_input(
      _scope, {}, {.channel_type = ChannelTypeId::mono,
                   .sample_layout = SampleStreamLayout::planar},
      value, min, max);
}
template<fixed_string Name>
inline PublicEventInputRef SubgraphBuilder::event_input(EventTypeId type) {
  return _builder.subgraph_event_input(_scope, Name.view(), type);
}
inline PublicEventInputRef SubgraphBuilder::event_input(EventTypeId type) {
  return _builder.subgraph_event_input(_scope, {}, type);
}
template<class... Refs>
inline void SubgraphBuilder::outputs(Refs&&... refs) {
  details::author_sample_output_requests(
      _builder,
      [this](std::span<SampleOutputRequest const> requests) {
        _builder.subgraph_outputs(_scope, requests);
      },
      std::forward<Refs>(refs)...);
}
inline void SubgraphBuilder::outputs(std::initializer_list<NamedRef> refs) {
  outputs(std::span<NamedRef const>(refs.begin(), refs.size()));
}
inline void SubgraphBuilder::outputs(std::span<NamedRef const> refs) {
  _builder.subgraph_outputs(_scope, refs);
}
inline void SubgraphBuilder::outputs(std::span<SampleOutputRequest const> refs) {
  _builder.subgraph_outputs(_scope, refs);
}
template<class... Refs>
inline void SubgraphBuilder::event_outputs(Refs&&... refs) {
  details::author_event_output_requests(
      [this](std::span<EventOutputRequest const> requests) {
        _builder.subgraph_event_outputs(_scope, requests);
      },
      std::forward<Refs>(refs)...);
}
inline void SubgraphBuilder::event_outputs(std::span<EventOutputRequest const> refs) {
  _builder.subgraph_event_outputs(_scope, refs);
}

template<class Node, class PortProjection>
inline NodePorts const& TypedNodeRef<Node, PortProjection>::ports() const {
  if (!_graph_builder) details::error("attempted to use a null NodeRef");
  return _graph_builder->typed_ports(_index);
}
template<class T>
inline NodeRef NodeRef::connect_input(size_t i, T&& value) const {
  if (!_graph_builder) details::error("attempted to use a null NodeRef");
  auto source = _graph_builder->lift_to_sample_port(std::forward<T>(value));
  if (source.graph_builder != _graph_builder)
    details::error("sample source belongs to another builder");
  _graph_builder->connect_sample_input({_index, PortKind::sample, i}, std::move(source));
  return _clone_handle();
}
template<class T>
inline NodeRef NodeRef::connect_input(std::string_view name, T&& value) const {
  if (!_graph_builder) details::error("attempted to use a null NodeRef");
  return connect_input(_graph_builder->sample_port_index(_index, true, name),
                       std::forward<T>(value));
}
template<class... Args>
inline NodeRef NodeRef::operator()(Args&&... args) const {
  if (!_graph_builder) details::error("attempted to use a null NodeRef");
  auto requests = details::make_node_call_requests(
      *_graph_builder, std::forward<Args>(args)...);
  apply_node_call(
      {.data = requests.sample_inputs.data(), .size = requests.sample_inputs.size()},
      {.data = requests.event_inputs.data(), .size = requests.event_inputs.size()});
  return _clone_handle();
}
template<class Node, class Projection>
inline SamplePortRef TypedNodeRef<Node, Projection>::operator[](size_t i) const {
  if (i >= get_num_outputs(ports()))
    details::error("sample output port is out of bounds on " + this->to_string());
  return NodeRef::operator[](i);
}
template<class Node, class Projection>
inline SamplePortRef TypedNodeRef<Node, Projection>::operator[](std::string_view name) const {
  if (!_graph_builder) details::error("attempted to use a null NodeRef");
  auto outputs = get_outputs(ports());
  std::optional<size_t> match;
  for (size_t i = 0; i < outputs.size(); ++i) {
    if (outputs[i].name != name) continue;
    if (match) details::error("output name is ambiguous");
    match = i;
  }
  if (!match) details::error("output port does not exist on " + this->to_string());
  return NodeRef::operator[](*match);
}
template<class Node, class Projection>
inline EventPortRef TypedNodeRef<Node, Projection>::event_port(size_t i) const {
  if (i >= ports().event_outputs().size())
    details::error("event output port is out of bounds");
  return NodeRef::event_port(i);
}
template<class Node, class Projection>
inline EventPortRef TypedNodeRef<Node, Projection>::event_port(
    std::string_view name) const {
  auto const& outputs = ports().event_outputs();
  std::optional<size_t> match;
  for (size_t i = 0; i < outputs.size(); ++i) {
    if (outputs[i].name != name) continue;
    if (match) details::error("event output name is ambiguous");
    match = i;
  }
  if (!match) details::error("event output port does not exist on " + this->to_string());
  return NodeRef::event_port(*match);
}
template<class Node, class Projection>
inline EventPortRef TypedNodeRef<Node, Projection>::event_port() const {
  if (ports().event_outputs().size() != 1)
    details::error(this->to_string() + " does not have exactly 1 event output port");
  return event_port(0);
}
template<class Node, class Projection>
inline TypedNodeRef<Node, Projection>::operator SamplePortRef() const {
  if (get_num_outputs(ports()) != 1)
    details::error(this->to_string() + " does not have exactly 1 output port");
  return SamplePortRef(*_graph_builder, {_index, PortKind::sample, 0});
}
template<class Node, class Projection>
template<class... Args>
    requires(details::node_call_enabled<std::remove_cvref_t<Node>, Args...>)
inline TypedNodeRef<Node, Projection>
TypedNodeRef<Node, Projection>::operator()(Args&&... args) const {
  if (!_graph_builder) details::error("attempted to use a null NodeRef");
  auto requests = details::make_node_call_requests(
      *_graph_builder, std::forward<Args>(args)...);
  this->apply_node_call(
      {.data = requests.sample_inputs.data(), .size = requests.sample_inputs.size()},
      {.data = requests.event_inputs.data(), .size = requests.event_inputs.size()});
  return this->_clone_handle();
}
template<class Node, class Projection>
template<class T>
inline TypedNodeRef<Node, Projection>
TypedNodeRef<Node, Projection>::connect_input(size_t i, T&& value) const {
  auto inputs = get_inputs(ports());
  if (i >= inputs.size()) details::error("sample input out of bounds");
  auto ref = _graph_builder->lift_to_sample_port(std::forward<T>(value));
  if (ref.graph_builder != _graph_builder)
    details::error("sample source belongs to another builder");
  _graph_builder->connect_sample_input({_index, PortKind::sample, i}, ref);
  return this->_clone_handle();
}
template<class Node, class Projection>
template<class T>
inline TypedNodeRef<Node, Projection>
TypedNodeRef<Node, Projection>::connect_input(
    std::string_view name, T&& value) const {
  auto inputs = get_inputs(ports());
  std::optional<size_t> match;
  for (size_t i = 0; i < inputs.size(); ++i) {
    if (inputs[i].name != name) continue;
    if (match) details::error("input name is ambiguous");
    match = i;
  }
  if (!match) details::error("input port does not exist");
  return connect_input(*match, std::forward<T>(value));
}
template<class Node, class Projection>
inline TypedNodeRef<Node, Projection>
TypedNodeRef<Node, Projection>::connect_event_input(
    size_t i, EventPortRef value) const {
  if (i >= ports().event_inputs().size())
    details::error("event input out of bounds");
  if (value.graph_builder != _graph_builder)
    details::error("event source belongs to another builder");
  _graph_builder->connect_event_input({_index, PortKind::event, i}, value);
  return this->_clone_handle();
}
template<class Node, class Projection>
inline TypedNodeRef<Node, Projection>
TypedNodeRef<Node, Projection>::connect_event_input(
    std::string_view name, EventPortRef value) const {
  auto const& inputs = ports().event_inputs();
  std::optional<size_t> match;
  for (size_t i = 0; i < inputs.size(); ++i) {
    if (inputs[i].name != name) continue;
    if (match) details::error("event input name is ambiguous");
    match = i;
  }
  if (!match) details::error("event input port does not exist");
  return connect_event_input(*match, std::move(value));
}
template<class Node, class Projection>
inline SamplePortRef TypedNodeRef<Node, Projection>::detach(size_t latency) const {
  return static_cast<SamplePortRef>(*this).detach(latency);
}
} // namespace iv
