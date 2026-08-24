#pragma once

#include <intravenous/graph/builder/public_ports.hpp>

#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace iv {
class GraphBuilder;

// Callback-facing facade for authoring one functional subgraph boundary.
// Nodes remain authored on the owning GraphBuilder; this type owns only the
// semantic interface state for the boundary created by GraphBuilder::subgraph.
class SubgraphBuilder {
  friend class GraphBuilder;

  GraphBuilder& _builder;
  GraphBuilderPublicPorts _ports;

  constexpr explicit SubgraphBuilder(GraphBuilder&, NodeBundleHandle);

  constexpr PublicSampleInputRef input_named(
      std::string_view name, Sample default_value,
      std::optional<Sample> min, std::optional<Sample> max);
  constexpr PublicEventInputRef event_input_named(
      std::string_view name, EventTypeId type);

public:
  SubgraphBuilder(SubgraphBuilder const&) = delete;
  SubgraphBuilder& operator=(SubgraphBuilder const&) = delete;
  SubgraphBuilder(SubgraphBuilder&&) = delete;
  SubgraphBuilder& operator=(SubgraphBuilder&&) = delete;

  constexpr PublicSampleInputRef input();
  template<fixed_string Name>
  constexpr PublicSampleInputRef input(
      Sample default_value = 0.0,
      std::optional<Sample> min = std::nullopt,
      std::optional<Sample> max = std::nullopt) {
    return input_named(Name.view(), default_value, min, max);
  }
  constexpr PublicSampleInputRef input(
      Sample default_value,
      std::optional<Sample> min = std::nullopt,
      std::optional<Sample> max = std::nullopt);

  template<fixed_string Name>
  constexpr PublicEventInputRef event_input(EventTypeId type) {
    return event_input_named(Name.view(), type);
  }
  constexpr PublicEventInputRef event_input(EventTypeId type);

  template<class... Refs>
  void event_outputs(Refs&&... refs);
  constexpr void event_outputs(std::span<EventOutputRefConfig const> refs);

  template<class... Refs>
  void outputs(Refs&&... refs);
  constexpr void outputs(std::initializer_list<NamedRef> refs);
  constexpr void outputs(std::span<OutputRefConfig const> refs);
  constexpr void outputs(std::span<NamedRef const> refs);
};

} // namespace iv

// Definitions require GraphBuilder to be complete. graph/builder.h includes this
// header before appending the implementation section below.
#ifdef IV_GRAPH_BUILDER_COMPLETE
namespace iv {
constexpr SubgraphBuilder::SubgraphBuilder(
    GraphBuilder& builder, NodeBundleHandle boundary)
    : _builder(builder), _ports(boundary) {}

constexpr PublicSampleInputRef SubgraphBuilder::input() {
  return input(Sample{0.0f});
}

constexpr PublicSampleInputRef SubgraphBuilder::input_named(
    std::string_view name, Sample value, std::optional<Sample> min,
    std::optional<Sample> max) {
  return PublicSampleInputRef(
      _ports.add_sample_input(_builder, _builder._node_bundles,
                              name, value, min, max));
}

constexpr PublicSampleInputRef SubgraphBuilder::input(
    Sample value, std::optional<Sample> min, std::optional<Sample> max) {
  return PublicSampleInputRef(
      _ports.add_sample_input(_builder, _builder._node_bundles,
                              {}, value, min, max));
}

constexpr PublicEventInputRef SubgraphBuilder::event_input_named(
    std::string_view name, EventTypeId type) {
  return PublicEventInputRef(
      _ports.add_event_input(_builder, _builder._node_bundles, name, type));
}

constexpr PublicEventInputRef SubgraphBuilder::event_input(EventTypeId type) {
  return PublicEventInputRef(
      _ports.add_event_input(_builder, _builder._node_bundles, {}, type));
}

constexpr void SubgraphBuilder::event_outputs(
    std::span<EventOutputRefConfig const> refs) {
  _ports.define_event_outputs(
      _builder, _builder._node_bundles, _builder._identity, refs);
}

constexpr void SubgraphBuilder::outputs(std::initializer_list<NamedRef> refs) {
  outputs(std::span<NamedRef const>(refs.begin(), refs.size()));
}

constexpr void SubgraphBuilder::outputs(std::span<OutputRefConfig const> refs) {
  _ports.define_sample_outputs(
      _builder, _builder._node_bundles, _builder._identity, refs);
}

constexpr void SubgraphBuilder::outputs(std::span<NamedRef const> refs) {
  _ports.define_sample_outputs_from_named_refs(
      _builder, _builder._node_bundles, _builder._identity,
      [&](auto&& value) {
        return _builder.lift_to_sample_port(
            std::forward<decltype(value)>(value));
      },
      refs);
}
} // namespace iv
#endif
