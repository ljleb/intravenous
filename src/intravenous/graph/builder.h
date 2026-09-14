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
#include <intravenous/module/configuration_argument.h>

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
#include <tuple>
#include <utility>

namespace iv {
struct ConfiguredGraph;
class GraphBuilder;
class GraphBuilderState;

namespace details {
struct BuilderSession;
NodeRef configure_package_definition(
    GraphBuilder&, std::string_view, std::span<ConfigurationArgument>);
NodeRef configure_tiled_package_definition(
    GraphBuilder&, std::string_view, ChannelLayout,
    std::span<ConfigurationArgument>);
NodeRef configure_package_definition_impl(
    GraphBuilder&, std::string_view, std::optional<ChannelLayout>,
    std::span<ConfigurationArgument>);
template<class Node, class... Args>
node_ref_for_t<Node> configure_concrete_node(GraphBuilder&, Args&&...);
template<class Node, class ChannelType, class... Args>
TiledNodeRef<Node, ChannelType> configure_concrete_tiled_node(
    GraphBuilder&, Args&&...);
template<class Node, class... Args>
NodeRef configure_concrete_node_tiled(
    GraphBuilder&, ChannelLayout, Args&&...);
template<class Node>
NodeRef configure_runtime_binary_op(
    GraphBuilder&, SamplePortRef, SamplePortRef, std::string_view);
GraphBuilderState& builder_graph_state(GraphBuilder&);
bool builder_session_has_packages(BuilderSession const*) noexcept;
NodeBundleHandle iv_builder_append_node(
    GraphBuilder&, NodeBuildRequest const&);
NodeBundleHandle iv_builder_append_tiled_node(
    GraphBuilder&, NodeBuildRequest const&, ChannelLayout);
NodeBundleHandle iv_builder_append_tiled_node_bundles(
    GraphBuilder&, std::span<NodeBundleHandle const>, ChannelLayout);
// A tiled registered IV_MODULE first configures its independent child graphs,
// then validates their public interfaces before any child is embedded in the
// caller. This prevents a rejected tile request from leaving partial subgraphs
// in a graph when provider code catches the diagnostic.
void iv_builder_validate_tiled_module_interfaces(
    std::span<GraphBuilder* const>);
void* iv_builder_allocate_node_config(
    BuilderSession*, std::size_t size, std::size_t alignment);
void iv_builder_discard_node_config(BuilderSession*, void* storage) noexcept;
// Unary DSL connection validation is shared configuration behavior. These helpers
// return only the runtime result the DSL needs to preserve its static return
// type.
bool iv_builder_connect_unary_sample(
    NodeRef const&, SamplePortRef, std::string_view operation);
EventPortRef iv_builder_connect_unary_event(
    NodeRef const&, EventPortRef, std::string_view operation);

}

class GraphBuilder {
  friend struct details::BuilderSession;
  friend GraphBuilderState& details::builder_graph_state(GraphBuilder&);
  friend NodeBundleHandle details::iv_builder_append_node(
      GraphBuilder&, details::NodeBuildRequest const&);
  friend NodeBundleHandle details::iv_builder_append_tiled_node(
      GraphBuilder&, details::NodeBuildRequest const&, ChannelLayout);
  friend NodeBundleHandle details::iv_builder_append_tiled_node_bundles(
      GraphBuilder&, std::span<NodeBundleHandle const>, ChannelLayout);
  friend NodeRef details::configure_package_definition(
      GraphBuilder&, std::string_view,
      std::span<details::ConfigurationArgument>);
  friend NodeRef details::configure_tiled_package_definition(
      GraphBuilder&, std::string_view, ChannelLayout,
      std::span<details::ConfigurationArgument>);
  friend NodeRef details::configure_package_definition_impl(
      GraphBuilder&, std::string_view, std::optional<ChannelLayout>,
      std::span<details::ConfigurationArgument>);
  template<class Node, class... Args>
  friend details::node_ref_for_t<Node> details::configure_concrete_node(
      GraphBuilder&, Args&&...);
  template<class Node, class ChannelType, class... Args>
  friend TiledNodeRef<Node, ChannelType> details::configure_concrete_tiled_node(
      GraphBuilder&, Args&&...);
  template<class Node, class... Args>
  friend NodeRef details::configure_concrete_node_tiled(
      GraphBuilder&, ChannelLayout, Args&&...);
  template<class Node>
  friend NodeRef details::configure_runtime_binary_op(
      GraphBuilder&, SamplePortRef, SamplePortRef, std::string_view);
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

  // A package definition ID is the only source-facing node-creation API.
  // Concrete node-type construction is deliberately private to lowering,
  // finalization, and the implementation of registered provider adapters.
  // The loaded IV packages resolve the provider immediately, so this returns
  // the provider's genuine realized NodeRef.
  template<fixed_string Id, class... Args>
  auto node(Args&&... args) {
    // Configuration is synchronous. Keep references to the actual caller
    // objects so the provider receives ordinary C++ value/reference semantics;
    // a registered-ID call must not silently copy an lvalue before validation.
    auto arguments = details::configuration_arguments(std::forward<Args>(args)...);
    return details::configure_package_definition(
        *this, Id.view(), std::span<details::ConfigurationArgument>{arguments});
  }

  // A registered definition can request a promoted channel layout without
  // exposing its implementation type. The result intentionally remains
  // NodeRef: an ID never promises a static port interface to its caller.
  // An IV_MODULE definition repeats a mono-interface subgraph once per
  // channel.
  template<fixed_string Id, class ChannelType, class... Args>
    requires requires { ChannelTypeTraits<ChannelType>::id; }
  NodeRef node(Args&&... args) {
    auto arguments = details::configuration_arguments(std::forward<Args>(args)...);
    return details::configure_tiled_package_definition(
        *this, Id.view(), {
          .channel_type = ChannelTypeTraits<ChannelType>::id,
          .sample_layout = SampleStreamLayout::planar,
        }, std::span<details::ConfigurationArgument>{arguments});
  }

  template<class ChannelType, class... Refs>
  auto tile(Refs&&... refs) {
    static_assert(sizeof...(Refs) == ChannelType::channel_count,
        "g.tile<ChannelType>(...) requires exactly one source per channel");
    std::array<SamplePortRef, ChannelType::channel_count> members{
        lift_to_sample_port(std::forward<Refs>(refs))...};
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
    // Scalar syntax is source sugar for the shipped constant definition, not
    // a second public path that materializes Constant in every consuming IV
    // package. A host-only GraphBuilder with no package table at all retains
    // the private concrete fallback used by compiler/lowering tests; a
    // configured package missing the built-in is still an error rather than a
    // silent direct construction.
    if (details::builder_session_has_packages(_session)) {
      return static_cast<SamplePortRef>(
          node<"constant">(static_cast<Sample>(value)));
    }
    return static_cast<SamplePortRef>(
        details::configure_concrete_node<Constant>(*this, static_cast<Sample>(value)));
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
  SampleInputConfig sample_input_config(NodeBundleHandle, size_t) const;
  EventInputConfig event_input_config(NodeBundleHandle, size_t) const;
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
  void annotate_node_input_source_info(NodeBundleHandle, PortKind,
      std::string_view, std::string_view, std::string_view, uint32_t, uint32_t);
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
  ConfiguredGraph finish() const &;
  ConfiguredGraph finish() &&;

private:
  // Runtime channel negotiation and direct concrete construction are builder
  // internals. The DSL reaches this only through details::configure_runtime_binary_op.
  NodeRef configure_runtime_binary_op(
      SamplePortRef lhs,
      SamplePortRef rhs,
      std::string_view op_name,
      details::NodeBuildRequest const& request);
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
template<class Node, class... Args>
node_ref_for_t<Node> configure_concrete_node(GraphBuilder& builder, Args&&... args)
{
  using StoredNode = std::remove_cvref_t<Node>;
  static_assert(std::is_trivially_copyable_v<StoredNode>,
      "node values must be trivially copyable");
  auto* value = static_cast<StoredNode*>(
      iv_builder_allocate_node_config(
          builder._session, sizeof(StoredNode), alignof(StoredNode)));
  try {
    std::construct_at(value, std::forward<Args>(args)...);
    auto handle = iv_builder_append_node(
        builder, make_node_build_request(*value));
    if constexpr (should_preserve_node_type_v<StoredNode>) {
      return TypedNodeRef<StoredNode>(builder, handle);
    } else {
      return NodeRef(builder, handle);
    }
  } catch (...) {
    iv_builder_discard_node_config(builder._session, value);
    throw;
  }
}

template<class Node, class ChannelType, class... Args>
TiledNodeRef<Node, ChannelType> configure_concrete_tiled_node(
    GraphBuilder& builder, Args&&... args)
{
  using StoredNode = std::remove_cvref_t<Node>;
  static_assert(std::is_trivially_copyable_v<StoredNode>,
      "node values must be trivially copyable");
  auto* value = static_cast<StoredNode*>(
      iv_builder_allocate_node_config(
          builder._session, sizeof(StoredNode), alignof(StoredNode)));
  try {
    std::construct_at(value, std::forward<Args>(args)...);
    auto handle = iv_builder_append_tiled_node(
        builder, make_node_build_request(*value), {
          .channel_type = ChannelTypeTraits<ChannelType>::id,
          .sample_layout = SampleStreamLayout::planar,
        });
    return TiledNodeRef<StoredNode, ChannelType>(builder, handle);
  } catch (...) {
    iv_builder_discard_node_config(builder._session, value);
    throw;
  }
}

template<class Node, class... Args>
NodeRef configure_concrete_node_tiled(
    GraphBuilder& builder, ChannelLayout layout, Args&&... args)
{
  using StoredNode = std::remove_cvref_t<Node>;
  static_assert(std::is_trivially_copyable_v<StoredNode>,
      "node values must be trivially copyable");
  auto* value = static_cast<StoredNode*>(
      iv_builder_allocate_node_config(
          builder._session, sizeof(StoredNode), alignof(StoredNode)));
  try {
    std::construct_at(value, std::forward<Args>(args)...);
    auto handle = iv_builder_append_tiled_node(
        builder, make_node_build_request(*value), layout);
    return NodeRef(builder, handle);
  } catch (...) {
    iv_builder_discard_node_config(builder._session, value);
    throw;
  }
}

template<class Node>
NodeRef configure_runtime_binary_op(
    GraphBuilder& builder,
    SamplePortRef lhs,
    SamplePortRef rhs,
    std::string_view op_name)
{
  using StoredNode = std::remove_cvref_t<Node>;
  static_assert(std::is_trivially_copyable_v<StoredNode>,
      "node values must be trivially copyable");
  auto* value = static_cast<StoredNode*>(
      iv_builder_allocate_node_config(
          builder._session, sizeof(StoredNode), alignof(StoredNode)));
  try {
    std::construct_at(value);
    return builder.configure_runtime_binary_op(
        std::move(lhs), std::move(rhs), op_name,
        make_node_build_request(*value));
  } catch (...) {
    iv_builder_discard_node_config(builder._session, value);
    throw;
  }
}
} // namespace details

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

template<class... Refs>
inline auto make_sample_output_requests(
    GraphBuilder& builder, Refs&&... refs) {
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
  return requests;
}

template<class... Refs>
inline auto make_event_output_requests(Refs&&... refs) {
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
  return requests;
}
} // namespace details

template<class... Refs>
inline void GraphBuilder::outputs(Refs&&... refs) {
  auto requests = details::make_sample_output_requests(
      *this, std::forward<Refs>(refs)...);
  outputs(std::span<SampleOutputRequest const>(requests));
}

template<class... Refs>
inline void GraphBuilder::event_outputs(Refs&&... refs) {
  auto requests = details::make_event_output_requests(
      std::forward<Refs>(refs)...);
  event_outputs(std::span<EventOutputRequest const>(requests));
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
  auto requests = details::make_sample_output_requests(
      _builder, std::forward<Refs>(refs)...);
  _builder.subgraph_outputs(
      _scope, std::span<SampleOutputRequest const>(requests));
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
  auto requests = details::make_event_output_requests(
      std::forward<Refs>(refs)...);
  _builder.subgraph_event_outputs(
      _scope, std::span<EventOutputRequest const>(requests));
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
  return NodeRef::operator[](i);
}
template<class Node, class Projection>
inline SamplePortRef TypedNodeRef<Node, Projection>::operator[](std::string_view name) const {
  return NodeRef::operator[](name);
}
template<class Node, class Projection>
inline EventPortRef TypedNodeRef<Node, Projection>::event_port(size_t i) const {
  return NodeRef::event_port(i);
}
template<class Node, class Projection>
inline EventPortRef TypedNodeRef<Node, Projection>::event_port(
    std::string_view name) const {
  return NodeRef::event_port(name);
}
template<class Node, class Projection>
inline EventPortRef TypedNodeRef<Node, Projection>::event_port() const {
  return NodeRef::event_port();
}
template<class Node, class Projection>
inline TypedNodeRef<Node, Projection>::operator SamplePortRef() const {
  return static_cast<SamplePortRef>(static_cast<NodeRef const&>(*this));
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
  return Base::connect_input(i, std::forward<T>(value));
}
template<class Node, class Projection>
template<class T>
inline TypedNodeRef<Node, Projection>
TypedNodeRef<Node, Projection>::connect_input(
    std::string_view name, T&& value) const {
  return Base::connect_input(name, std::forward<T>(value));
}
template<class Node, class Projection>
inline TypedNodeRef<Node, Projection>
TypedNodeRef<Node, Projection>::connect_event_input(
    size_t i, EventPortRef value) const {
  return Base::connect_event_input(i, std::move(value));
}
template<class Node, class Projection>
inline TypedNodeRef<Node, Projection>
TypedNodeRef<Node, Projection>::connect_event_input(
    std::string_view name, EventPortRef value) const {
  NodeRef::connect_event_input(name, std::move(value));
  return this->_clone_handle();
}
template<class Node, class Projection>
inline SamplePortRef TypedNodeRef<Node, Projection>::detach(size_t latency) const {
  return static_cast<SamplePortRef>(*this).detach(latency);
}
} // namespace iv
