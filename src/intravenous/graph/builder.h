#pragma once

// Module-facing builder facade. It owns no graph containers: all mutable
// graph state belongs to the opaque BuilderSession in libiv_builder.

#include <intravenous/basic_nodes/constant.h>
#include <intravenous/channel_ports.h>
#include <intravenous/graph/builder/node_refs.h>
#include <intravenous/graph/builder/embedding.h>
#include <intravenous/graph/builder/output_refs.h>
#include <intravenous/graph/builder/session_ops.h>
#include <intravenous/graph/builder/snapshot_error.h>
#include <intravenous/graph/builder/subgraphs.hpp>
#include <intravenous/graph/source_info.h>
#include <intravenous/node/build_request.h>
#include <intravenous/node/message.h>
#include <intravenous/node/immutable_array.h>
#include <intravenous/module/configuration_argument.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <exception>
#include <string_view>
#include <type_traits>
#include <tuple>
#include <utility>
#include <variant>

namespace iv {
struct ConfiguredGraph;
class GraphBuilder;
class GraphBuilderState;
class SubgraphClosure;

namespace details {
struct BuilderSession;
void register_builder_module_class_message_handlers(
    GraphBuilder&, std::shared_ptr<void const> receiver);
void attach_builder_node_message_path(
    BuilderSession*, NodeRef&, NodeBundleHandle);
NodeRef configure_package_definition(
    GraphBuilder&, std::string_view, std::span<ConfigurationArgument>);
NodeRef configure_tiled_package_definition(
    GraphBuilder&, std::string_view, ChannelLayout,
    std::span<ConfigurationArgument>);
NodeRef configure_package_definition_impl(
    GraphBuilder&, std::string_view, std::optional<ChannelLayout>,
    std::span<ConfigurationArgument>);
NodeRef configure_package_definition_provider(
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
void iv_builder_prepare_node_source_identity(
    GraphBuilder*,
    std::string_view source_identity,
    std::string_view authored_name);
NodeBundleHandle iv_builder_append_node(
    GraphBuilder&, NodeBuildRequest const&);
NodeBundleHandle iv_builder_append_tiled_node(
    GraphBuilder&, NodeBuildRequest const&, ChannelLayout);
NodeBundleHandle iv_builder_append_tiled_node_bundles(
    GraphBuilder&, std::span<NodeBundleHandle const>, ChannelLayout);
void register_builder_embedded_message_handlers(
    GraphBuilder&,
    std::span<ProjectNodePathSelector const>,
    std::span<ConfiguredNodeMessageHandler const>);
// Scope operations. A view's interface operations edit the boundary of the
// scope it names; node creation edits the session's current scope.
[[nodiscard]] BuilderScopeId iv_builder_current_scope(BuilderSession*) noexcept;
void const* iv_builder_copy_immutable_data(
    BuilderSession*, std::span<std::byte const>, std::size_t alignment);

PublicSampleInputRef iv_builder_scope_input(
    BuilderSession*, BuilderScopeId, ContinuousInputConfig);
PublicEventInputRef iv_builder_scope_event_input(
    BuilderSession*, BuilderScopeId, std::string_view, EventTypeId);
void iv_builder_scope_outputs(
    BuilderSession*, BuilderScopeId, std::span<SampleOutputRequest const>);
void iv_builder_scope_outputs(
    BuilderSession*, BuilderScopeId, std::span<OutputRequest const>);
void iv_builder_scope_named_outputs(
    BuilderSession*, BuilderScopeId, std::span<NamedRef const>);
void iv_builder_scope_event_outputs(
    BuilderSession*, BuilderScopeId, std::span<EventOutputRequest const>);
[[nodiscard]] BuilderScopeId iv_builder_begin_subgraph_scope(BuilderSession*);
[[nodiscard]] NodeRef iv_builder_finish_subgraph_scope(
    BuilderSession*, BuilderScopeId, std::string_view kind);
// Records an exception escaping authored construction code as the session's
// construction failure (see module/builder_session.h); the session then
// refuses further construction and its configuration pass fails.
void iv_builder_record_failure(
    BuilderSession*, std::exception_ptr failure, std::string_view context) noexcept;
void iv_builder_throw_if_failed(BuilderSession*);
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

template<RegisteredContinuousValue... Types>
[[nodiscard]] ContinuousInputConfig make_boundary_input_config(
    std::string_view name,
    typename ContinuousValueType<Types>::BoundaryInputOptions... options)
  requires(sizeof...(Types) != 0)
{
  auto authored = iv::input<Types...>(
      std::string(name), InputAccess::sequential,
      ContinuousValueType<Types>::boundary_input_sequential_options(
          options)...);
  return materialize_continuous_config(erase_port_config(authored));
}

template<RegisteredContinuousValue... Types>
[[nodiscard]] ContinuousInputConfig make_boundary_input_config(
    std::string_view name)
  requires(sizeof...(Types) != 0)
{
  return make_boundary_input_config<Types...>(
      name, typename ContinuousValueType<Types>::BoundaryInputOptions{}...);
}

[[nodiscard]] inline ContinuousInputConfig make_default_boundary_input_config(
    std::string_view name, ChannelLayout channel_layout =
        continuous_value_details::default_audio_channel_layout)
{
  using Options =
      typename ContinuousValueType<DefaultContinuousValueType>::BoundaryInputOptions;
  return make_boundary_input_config<DefaultContinuousValueType>(name, Options{
      .channel_layout = channel_layout,
  });
}

[[nodiscard]] inline ContinuousInputConfig make_sample_boundary_input_config(
    std::string_view name, ChannelLayout channel_layout, Sample default_value,
    std::optional<Sample> min, std::optional<Sample> max)
{
  return make_boundary_input_config<Sample>(name, {
      .channel_layout = channel_layout,
      .default_value = default_value,
      .min = min.value_or(
          -std::numeric_limits<Sample::storage>::infinity()),
      .max = max.value_or(
          std::numeric_limits<Sample::storage>::infinity()),
  });
}

}

class GraphBuilder {
  friend struct details::BuilderSession;
  friend GraphBuilderState& details::builder_graph_state(GraphBuilder&);
  friend class NodeRef;
  friend void details::iv_builder_prepare_node_source_identity(
      GraphBuilder*, std::string_view, std::string_view);
  friend void details::register_builder_module_class_message_handlers(
      GraphBuilder&, std::shared_ptr<void const>);
  friend NodeBundleHandle details::iv_builder_append_node(
      GraphBuilder&, details::NodeBuildRequest const&);
  friend NodeBundleHandle details::iv_builder_append_tiled_node(
      GraphBuilder&, details::NodeBuildRequest const&, ChannelLayout);
  friend NodeBundleHandle details::iv_builder_append_tiled_node_bundles(
      GraphBuilder&, std::span<NodeBundleHandle const>, ChannelLayout);
  friend void details::register_builder_embedded_message_handlers(
      GraphBuilder&,
      std::span<ProjectNodePathSelector const>,
      std::span<ConfiguredNodeMessageHandler const>);
  friend NodeRef details::configure_package_definition(
      GraphBuilder&, std::string_view,
      std::span<details::ConfigurationArgument>);
  friend NodeRef details::configure_tiled_package_definition(
      GraphBuilder&, std::string_view, ChannelLayout,
      std::span<details::ConfigurationArgument>);
  friend NodeRef details::configure_package_definition_impl(
      GraphBuilder&, std::string_view, std::optional<ChannelLayout>,
      std::span<details::ConfigurationArgument>);
  friend NodeRef details::configure_package_definition_provider(
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
  // The scope whose boundary input()/outputs() edit. Node creation always
  // edits the session's current scope instead.
  details::BuilderScopeId _scope = details::root_builder_scope_id;
  bool _owns_session = false;
  // A view returned by nested() owns its open scope until subgraph(...)
  // finalizes it; destroying it first rolls that scope back.
  bool _owns_scope = false;

  GraphBuilder(details::BuilderSession*, details::BuilderScopeId,
      bool owns_session, bool owns_scope) noexcept;

public:
  // A root view that owns a new BuilderSession.
  GraphBuilder();
  // A non-owning view of the session's current scope.
  explicit GraphBuilder(details::BuilderSession*) noexcept;
  // A non-owning view of one open scope of `session`.
  GraphBuilder(details::BuilderSession*, details::BuilderScopeId) noexcept;
  ~GraphBuilder();
  GraphBuilder(GraphBuilder const&) = delete;
  GraphBuilder& operator=(GraphBuilder const&) = delete;
  GraphBuilder(GraphBuilder&&) noexcept;
  GraphBuilder& operator=(GraphBuilder&&) noexcept;

  bool valid() const noexcept { return _session != nullptr; }
  // Internal: the session this view belongs to, for host configuration code.
  [[nodiscard]] details::BuilderSession* _builder_session() const noexcept
  {
      return _session;
  }

  // Copy construction data once. No array allocation or copying occurs in Tick.
  template<class T>
  [[nodiscard]] ImmutableArray<T> immutable_array(std::span<T const> values) {
    if (values.empty()) return {};
    auto* data = details::iv_builder_copy_immutable_data(
        _session, std::as_bytes(values), alignof(T));
    return ImmutableArray<T>{static_cast<T const*>(data), values.size()};
  }

  PublicSampleInputRef input();
  template<fixed_string Name, class ChannelType = mono>
    requires requires { ChannelTypeTraits<ChannelType>::id; }
  TypedPublicSampleInputRef<ChannelType> input() {
    return TypedPublicSampleInputRef<ChannelType>{input_named(
        Name.view(), details::make_default_boundary_input_config(
            Name.view(), {
                .channel_type = ChannelTypeTraits<ChannelType>::id,
                .sample_layout = SampleStreamLayout::planar,
            }))};
  }
  template<fixed_string Name, class ChannelType = mono>
    requires requires { ChannelTypeTraits<ChannelType>::id; }
  TypedPublicSampleInputRef<ChannelType> input(Sample default_value,
      std::optional<Sample> min = std::nullopt,
      std::optional<Sample> max = std::nullopt) {
    return TypedPublicSampleInputRef<ChannelType>{input_named(
        Name.view(), details::make_sample_boundary_input_config(
            Name.view(), {
                .channel_type = ChannelTypeTraits<ChannelType>::id,
                .sample_layout = SampleStreamLayout::planar,
            }, default_value, min, max))};
  }
  template<fixed_string Name, RegisteredContinuousValue... Types>
    requires(sizeof...(Types) != 0)
  PublicSampleInputRef input() {
    return input_named(
        Name.view(), details::make_boundary_input_config<Types...>(Name.view()));
  }
  template<fixed_string Name, RegisteredContinuousValue... Types>
    requires(sizeof...(Types) != 0)
  PublicSampleInputRef input(
      typename ContinuousValueType<Types>::BoundaryInputOptions... options) {
    return input_named(Name.view(),
        details::make_boundary_input_config<Types...>(
            Name.view(), std::move(options)...));
  }
  template<RegisteredContinuousValue... Types>
    requires(sizeof...(Types) != 0)
  PublicSampleInputRef input() {
    return input_named({}, details::make_boundary_input_config<Types...>({}));
  }
  template<RegisteredContinuousValue... Types>
    requires(sizeof...(Types) != 0)
  PublicSampleInputRef input(
      typename ContinuousValueType<Types>::BoundaryInputOptions... options) {
    return input_named({}, details::make_boundary_input_config<Types...>(
        {}, std::move(options)...));
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
  void outputs(std::span<OutputRequest const>);

  template<class... Refs>
  void event_outputs(Refs&&... refs);
  void event_outputs(std::span<EventOutputRequest const>);

  // One source-level subgraph concept. A callable constructs a fresh
  // boundary scope in place; a SubgraphClosure is the reusable, transportable
  // value form of a callable or of a frozen ConfiguredGraph (imported
  // directly). A nested live GraphBuilder is not a closure: it is the open
  // current scope, finalized in place exactly once by subgraph(GraphBuilder&&).
  template<class Fn>
    requires(std::invocable<Fn, SubgraphBuilder&> || std::invocable<Fn>)
  NodeRef subgraph(Fn&& fn, std::string_view kind = "Subgraph") {
    auto const scope = details::iv_builder_begin_subgraph_scope(_session);
    // A failure escaping the callable, or a refused finalization (a scope the
    // callable opened is still open), fails the session; nothing is undone.
    try {
      if constexpr (std::invocable<Fn, SubgraphBuilder&>) {
        SubgraphBuilder subgraph(_session, scope);
        std::invoke(std::forward<Fn>(fn), subgraph);
      } else {
        std::invoke(std::forward<Fn>(fn));
      }
      details::iv_builder_throw_if_failed(_session);
      return details::iv_builder_finish_subgraph_scope(_session, scope, kind);
    } catch (...) {
      details::iv_builder_record_failure(
          _session, std::current_exception(), "while invoking a subgraph closure");
      throw;
    }
  }
  NodeRef subgraph(SubgraphClosure& closure, std::string_view kind = "Subgraph");
  NodeRef subgraph(SubgraphClosure&& closure, std::string_view kind = "Subgraph");
  // Finalizes a view returned by nested() in place; its bundles stay where
  // they were constructed. The view no longer owns a scope afterwards.
  NodeRef subgraph(GraphBuilder&& nested, std::string_view kind = "Subgraph");

  // Opens a nested definition scope in this session and returns the view
  // that owns it. Nodes created anywhere in the session while it is open
  // belong to it, so it is finalized where it was opened: attach it with
  // subgraph(std::move(view)) or snapshot it with configured_graph().
  [[nodiscard]] GraphBuilder nested();

  // Non-destructive copy of the scope this view represents. Fails when the
  // scope refers to graph state outside itself, leaving the live scope valid.
  [[nodiscard]] std::expected<ConfiguredGraph, ConfiguredGraphSnapshotError>
  configured_graph() const;

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

  PublicSampleInputRef input_named(
      std::string_view, ContinuousInputConfig);
  PublicEventInputRef event_input_named(std::string_view, EventTypeId);
  void connect_sample_input(NodeBundlePortId, SamplePortRef);
  void connect_sample_input(NodeBundlePortId, std::span<SamplePortRef const>);
  void connect_event_input(NodeBundlePortId, EventPortRef);
  bool sample_input_is_connected(NodeBundlePortId) const;
  bool event_input_is_connected(NodeBundlePortId) const;
  EventPortRef event_output(NodeBundlePortId) const;
  size_t sample_port_index(NodeBundleHandle, bool inputs, std::string_view) const;
  size_t event_port_index(NodeBundleHandle, bool inputs, std::string_view) const;
  size_t sample_input_count(NodeBundleHandle) const;
  size_t sample_output_count(NodeBundleHandle) const;
  size_t event_input_count(NodeBundleHandle) const;
  size_t event_output_count(NodeBundleHandle) const;
  NodeBundlePortId input_port_at(NodeBundleHandle, size_t) const;
  NodeBundleHandle tiled_member(NodeBundleHandle, size_t) const;
  NodePorts const& typed_ports(NodeBundleHandle) const;
  SamplePortRef sample_port_from_output(NodeBundlePortId);
  EventPortRef event_port_from_output(NodeBundlePortId) const;
  SamplePortRef make_sample_port(ChannelTypeId, std::span<SampleOutputChannelId const>);
  SamplePortRef make_tiled_sample_port(
      ChannelTypeId, std::span<SamplePortRef const>);
  SamplePortRef select_sample_port_channel(SamplePortRef const&, size_t);
  std::span<SampleOutputChannelId const> sample_port_channels(SamplePortRef const&) const;
  EventPortRef make_event_port(EventTypeId, std::span<EventOutputPortId const>);
  EventPortRef make_event_port(std::span<EventPortRef const>);
  std::span<EventOutputPortId const> event_port_sources(EventPortRef const&) const;
  SamplePortRef detach_sample_port(
      SamplePortRef const&, size_t, std::optional<Sample>);
  EventPortRef detach_event_port(EventPortRef const&, size_t);
  void apply_ttl(NodeBundleHandle, size_t);
  void annotate_node(NodeBundleHandle, std::string_view, std::string_view,
      uint32_t, uint32_t);
  void annotate_node_input_source_info(NodeBundleHandle, PortKind,
      std::string_view, std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_public_sample_output_source_info(std::span<SourceInfo const>);
  void annotate_public_event_output_source_info(std::span<SourceInfo const>);
  void annotate_public_sample_output_source_info(size_t, SourceInfo);
  void annotate_public_event_output_source_info(size_t, SourceInfo);
  // Imports a frozen configured graph directly as one new subgraph scope. The
  // returned translation is stable for this session and leaves all local
  // handles in the frozen graph untouched.
  ConfiguredGraphEmbedding embed(
      ConfiguredGraph const&, std::string_view kind = "Configured graph");

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
};

// The transportable subgraph value: a frozen graph or a construction
// callable, each placing a fresh scope on every invocation through an lvalue.
// It is move-only so it can carry move-only captures. A closure capturing live
// refs is a valid construction argument, but its invocation cannot be
// independently cached.
using ErasedSubgraphFunction = std::move_only_function<void(SubgraphBuilder&)>;

class SubgraphClosure {
  friend class GraphBuilder;

  std::variant<
      std::reference_wrapper<ConfiguredGraph const>,
      std::shared_ptr<ConfiguredGraph const>,
      ErasedSubgraphFunction> _value;

public:
  // Borrowed frozen graph; the caller keeps it alive while the closure is used.
  SubgraphClosure(ConfiguredGraph const& graph) noexcept
      : _value(std::cref(graph)) {}
  explicit SubgraphClosure(std::shared_ptr<ConfiguredGraph const> graph);
  template<class Fn>
    requires(!std::same_as<std::remove_cvref_t<Fn>, SubgraphClosure>
        && !std::same_as<std::remove_cvref_t<Fn>, GraphBuilder>
        && !std::same_as<std::remove_cvref_t<Fn>, ConfiguredGraph>
        && std::invocable<std::remove_cvref_t<Fn>&, SubgraphBuilder&>)
  SubgraphClosure(Fn&& fn)
      : _value(std::in_place_type<ErasedSubgraphFunction>, std::forward<Fn>(fn)) {}

  SubgraphClosure(SubgraphClosure&&) noexcept = default;
  SubgraphClosure& operator=(SubgraphClosure&&) noexcept = default;
  SubgraphClosure(SubgraphClosure const&) = delete;
  SubgraphClosure& operator=(SubgraphClosure const&) = delete;
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
      TypedNodeRef<StoredNode> result(builder, handle);
      attach_builder_node_message_path(builder._session, result, handle);
      return result;
    } else {
      NodeRef result(builder, handle);
      attach_builder_node_message_path(builder._session, result, handle);
      return result;
    }
  } catch (...) {
    iv_builder_discard_node_config(builder._session, value);
    // The node constructor is authored code; its failure fails the session.
    iv_builder_record_failure(builder._session, std::current_exception(), {});
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
    TiledNodeRef<StoredNode, ChannelType> result(builder, handle);
    attach_builder_node_message_path(builder._session, result, handle);
    return result;
  } catch (...) {
    iv_builder_discard_node_config(builder._session, value);
    // The node constructor is authored code; its failure fails the session.
    iv_builder_record_failure(builder._session, std::current_exception(), {});
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
    NodeRef result(builder, handle);
    attach_builder_node_message_path(builder._session, result, handle);
    return result;
  } catch (...) {
    iv_builder_discard_node_config(builder._session, value);
    // The node constructor is authored code; its failure fails the session.
    iv_builder_record_failure(builder._session, std::current_exception(), {});
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
    // The node constructor is authored code; its failure fails the session.
    iv_builder_record_failure(builder._session, std::current_exception(), {});
    throw;
  }
}
} // namespace details

namespace details {
template<class... Args>
auto make_node_call_requests(BuilderSession* session, Args&&... args)
{
  // Scalar arguments are lifted into constant nodes of the current scope.
  GraphBuilder builder(session);
  using Requests = NodeCallRequests<
      sample_input_arg_count_v<Args...> + dynamic_input_arg_count_v<Args...>,
      event_input_arg_count_v<Args...> + dynamic_input_arg_count_v<Args...>>;
  Requests requests;
  size_t positional_index = 0;
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
            .input_index = 0,
            .target = NodeCallInputTarget::named,
        };
      } else {
        requests.event_inputs[event_index++] = {
            .source = lift_node_call_event_operand(
                std::forward<Arg>(arg).value),
            .name = Value::name.view(),
            .input_index = 0,
            .target = NodeCallInputTarget::named,
        };
      }
    } else if constexpr (graph_builder_node_output_like<Arg>) {
      auto source = arg.node_ref();
      auto const port = [&] {
        if constexpr (static_default_output_kind<Arg>())
          return NodeBundlePortId{source.node_bundle_handle(), *static_default_output_kind<Arg>(), 0};
        else return source.output_port_at(0);
      }();
      if (port.port_kind == PortKind::event) {
        requests.event_inputs[event_index++] = {
            .source = source.event_port(port.port_index), .name = {},
            .input_index = positional_index++, .target = NodeCallInputTarget::positional};
      } else {
        requests.sample_inputs[sample_index++] = {
            .source = source[port.port_index], .name = {},
            .input_index = positional_index++, .target = NodeCallInputTarget::positional};
      }
    } else if constexpr (graph_builder_event_port_like<Arg>) {
      requests.event_inputs[event_index++] = {
        .source = static_cast<EventPortRef>(std::forward<Arg>(arg)),
        .name = {},
        .input_index = positional_index++,
        .target = NodeCallInputTarget::positional,
      };
    } else {
      requests.sample_inputs[sample_index++] = {
        .source = builder.lift_to_sample_port(std::forward<Arg>(arg)),
        .name = {},
        .input_index = positional_index++,
        .target = NodeCallInputTarget::positional,
      };
    }
  };
  (append(std::forward<Args>(args)), ...);
  requests.sample_input_count = sample_index;
  requests.event_input_count = event_index;
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
      request.target_channel_index = Value::channel_index;
    } else if constexpr (is_default_channel_named_arg_v<Value>) {
      using Channel = typename Value::channel_type;
      request.ref = builder.lift_to_sample_port(ref.value);
      request.name = "main";
      request.channel_layout = {.channel_type = ChannelTypeTraits<Channel>::id,
        .sample_layout = SampleStreamLayout::planar};
      request.family_name = "main";
      request.family_channel_type = ChannelTypeTraits<Channel>::id;
      request.target_channel_index = Value::channel_index;
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
      request.ref = lift_node_call_event_operand(ref.value);
      request.name = Value::name.view();
    } else {
      static_assert(count == 1,
        "event_outputs(...) requires names when exposing more than one event output");
      request.ref = lift_node_call_event_operand(std::forward<Ref>(ref));
    }
  };
  (append(std::forward<Refs>(refs)), ...);
  return requests;
}
template<class... Refs>
inline auto make_output_requests(GraphBuilder& builder, Refs&&... refs)
{
  constexpr auto count = sizeof...(Refs);
  std::array<OutputRequest, count> requests{};
  std::size_t index = 0;
  auto append = [&]<class Ref>(Ref&& ref) {
    using Value = std::remove_cvref_t<Ref>;
    if constexpr (is_named_arg_v<Value>) {
      if constexpr (Value::kind == NamedPortKind::event)
        requests[index++] = make_event_output_requests(std::forward<Ref>(ref))[0];
      else requests[index++] = make_sample_output_requests(builder, std::forward<Ref>(ref))[0];
    } else if constexpr (is_channel_named_arg_v<Value> || is_default_channel_named_arg_v<Value>) {
      requests[index++] = make_sample_output_requests(builder, std::forward<Ref>(ref))[0];
    } else {
      static_assert(count == 1, "outputs(...) requires names when exposing more than one output");
      if constexpr (graph_builder_node_output_like<Ref>) {
        auto source = ref.node_ref();
        auto const port = source.output_port_at(0);
        if (port.port_kind == PortKind::event)
          requests[index++] = make_event_output_requests(source.event_port(port.port_index))[0];
        else requests[index++] = make_sample_output_requests(builder, source[port.port_index])[0];
      } else if constexpr (graph_builder_event_port_like<Ref>) {
        requests[index++] = make_event_output_requests(std::forward<Ref>(ref))[0];
      } else {
        requests[index++] = make_sample_output_requests(builder, std::forward<Ref>(ref))[0];
      }
    }
  };
  (append(std::forward<Refs>(refs)), ...);
  return requests;
}
} // namespace details

template<class... Refs>
inline void GraphBuilder::outputs(Refs&&... refs) {
  auto requests = details::make_output_requests(*this, std::forward<Refs>(refs)...);
  outputs(std::span<OutputRequest const>(requests));
}

template<class... Refs>
inline void GraphBuilder::event_outputs(Refs&&... refs) {
  auto requests = details::make_event_output_requests(
      std::forward<Refs>(refs)...);
  event_outputs(std::span<EventOutputRequest const>(requests));
}

inline PublicSampleInputRef SubgraphBuilder::input() {
  return details::iv_builder_scope_input(_session, _scope,
      details::make_default_boundary_input_config({}, {
          .channel_type = ChannelTypeId::mono,
          .sample_layout = SampleStreamLayout::planar,
      }));
}
template<fixed_string Name, class ChannelType>
  requires requires { ChannelTypeTraits<ChannelType>::id; }
inline TypedPublicSampleInputRef<ChannelType> SubgraphBuilder::input() {
  return TypedPublicSampleInputRef<ChannelType>{details::iv_builder_scope_input(
      _session, _scope, details::make_default_boundary_input_config(Name.view(), {
          .channel_type = ChannelTypeTraits<ChannelType>::id,
          .sample_layout = SampleStreamLayout::planar,
      }))};
}
template<fixed_string Name, class ChannelType>
  requires requires { ChannelTypeTraits<ChannelType>::id; }
inline TypedPublicSampleInputRef<ChannelType> SubgraphBuilder::input(
    Sample value, std::optional<Sample> min, std::optional<Sample> max) {
  return TypedPublicSampleInputRef<ChannelType>{details::iv_builder_scope_input(
      _session, _scope, details::make_sample_boundary_input_config(Name.view(), {
              .channel_type = ChannelTypeTraits<ChannelType>::id,
              .sample_layout = SampleStreamLayout::planar,
          }, value, min, max))};
}
template<fixed_string Name, RegisteredContinuousValue... Types>
  requires(sizeof...(Types) != 0)
inline PublicSampleInputRef SubgraphBuilder::input() {
  return details::iv_builder_scope_input(
      _session, _scope, details::make_boundary_input_config<Types...>(Name.view()));
}
template<fixed_string Name, RegisteredContinuousValue... Types>
  requires(sizeof...(Types) != 0)
inline PublicSampleInputRef SubgraphBuilder::input(
    typename ContinuousValueType<Types>::BoundaryInputOptions... options) {
  return details::iv_builder_scope_input(
      _session, _scope, details::make_boundary_input_config<Types...>(
          Name.view(), std::move(options)...));
}
template<RegisteredContinuousValue... Types>
  requires(sizeof...(Types) != 0)
inline PublicSampleInputRef SubgraphBuilder::input() {
  return details::iv_builder_scope_input(
      _session, _scope, details::make_boundary_input_config<Types...>({}));
}
template<RegisteredContinuousValue... Types>
  requires(sizeof...(Types) != 0)
inline PublicSampleInputRef SubgraphBuilder::input(
    typename ContinuousValueType<Types>::BoundaryInputOptions... options) {
  return details::iv_builder_scope_input(
      _session, _scope, details::make_boundary_input_config<Types...>(
          {}, std::move(options)...));
}
inline PublicSampleInputRef SubgraphBuilder::input(
    Sample value, std::optional<Sample> min, std::optional<Sample> max) {
  return details::iv_builder_scope_input(_session, _scope,
      details::make_sample_boundary_input_config({}, {
          .channel_type = ChannelTypeId::mono,
          .sample_layout = SampleStreamLayout::planar,
      }, value, min, max));
}
template<fixed_string Name>
inline PublicEventInputRef SubgraphBuilder::event_input(EventTypeId type) {
  return details::iv_builder_scope_event_input(_session, _scope, Name.view(), type);
}
inline PublicEventInputRef SubgraphBuilder::event_input(EventTypeId type) {
  return details::iv_builder_scope_event_input(_session, _scope, {}, type);
}
template<class... Refs>
inline void SubgraphBuilder::outputs(Refs&&... refs) {
  GraphBuilder builder(_session);
  auto requests = details::make_output_requests(builder, std::forward<Refs>(refs)...);
  details::iv_builder_scope_outputs(
      _session, _scope, std::span<OutputRequest const>(requests));
}
inline void SubgraphBuilder::outputs(std::initializer_list<NamedRef> refs) {
  outputs(std::span<NamedRef const>(refs.begin(), refs.size()));
}
inline void SubgraphBuilder::outputs(std::span<NamedRef const> refs) {
  details::iv_builder_scope_named_outputs(_session, _scope, refs);
}
inline void SubgraphBuilder::outputs(std::span<SampleOutputRequest const> refs) {
  details::iv_builder_scope_outputs(_session, _scope, refs);
}
inline void SubgraphBuilder::outputs(std::span<OutputRequest const> refs) {
  details::iv_builder_scope_outputs(_session, _scope, refs);
}
template<class... Refs>
inline void SubgraphBuilder::event_outputs(Refs&&... refs) {
  auto requests = details::make_event_output_requests(
      std::forward<Refs>(refs)...);
  details::iv_builder_scope_event_outputs(
      _session, _scope, std::span<EventOutputRequest const>(requests));
}
inline void SubgraphBuilder::event_outputs(std::span<EventOutputRequest const> refs) {
  details::iv_builder_scope_event_outputs(_session, _scope, refs);
}

template<class Node, class PortProjection>
inline NodePorts const& TypedNodeRef<Node, PortProjection>::ports() const {
  if (!_session) details::error("attempted to use a null NodeRef");
  return details::iv_builder_typed_ports(_session, _index);
}
template<class T>
inline NodeRef NodeRef::connect_input(size_t i, T&& value) const {
  if (!_session) details::error("attempted to use a null NodeRef");
  GraphBuilder builder(_session);
  auto source = builder.lift_to_sample_port(std::forward<T>(value));
  if (source.session != _session)
    details::error("sample source belongs to another builder session");
  details::iv_builder_connect_sample_input(
      _session, {_index, PortKind::sample, i}, source);
  return _clone_handle();
}
template<class T>
inline NodeRef NodeRef::connect_input(std::string_view name, T&& value) const {
  if (!_session) details::error("attempted to use a null NodeRef");
  return connect_input(
      details::iv_builder_sample_port_index(_session, _index, true, name),
      std::forward<T>(value));
}
template<class... Args>
inline NodeRef NodeRef::operator()(Args&&... args) const {
  if (!_session) details::error("attempted to use a null NodeRef");
  auto requests = details::make_node_call_requests(
      _session, std::forward<Args>(args)...);
  apply_node_call(
      {.data = requests.sample_inputs.data(), .size = requests.sample_input_count},
      {.data = requests.event_inputs.data(), .size = requests.event_input_count});
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
  if (!_session) details::error("attempted to use a null NodeRef");
  auto requests = details::make_node_call_requests(
      _session, std::forward<Args>(args)...);
  this->apply_node_call(
      {.data = requests.sample_inputs.data(), .size = requests.sample_input_count},
      {.data = requests.event_inputs.data(), .size = requests.event_input_count});
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
inline SamplePortRef TypedNodeRef<Node, Projection>::detach(
    size_t latency, std::optional<Sample> initial_value) const {
  return static_cast<SamplePortRef>(*this).detach(latency, initial_value);
}
} // namespace iv
