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

  explicit SubgraphBuilder(GraphBuilder&, NodeBundleHandle);

  PublicSampleInputRef input_named(std::string_view name, Sample default_value,
                                   std::optional<Sample> min,
                                   std::optional<Sample> max);
  PublicEventInputRef event_input_named(std::string_view name, EventTypeId type);

public:
  SubgraphBuilder(SubgraphBuilder const&) = delete;
  SubgraphBuilder& operator=(SubgraphBuilder const&) = delete;
  SubgraphBuilder(SubgraphBuilder&&) = delete;
  SubgraphBuilder& operator=(SubgraphBuilder&&) = delete;

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
  PublicEventInputRef event_input(EventTypeId type) {
    return event_input_named(Name.view(), type);
  }
  PublicEventInputRef event_input(EventTypeId type);

  template<class... Refs>
  void event_outputs(Refs&&... refs);
  void event_outputs(std::span<EventOutputRefConfig const> refs);

  template<class... Refs>
  void outputs(Refs&&... refs);
  constexpr void outputs(std::initializer_list<NamedRef> refs);
  void outputs(std::span<OutputRefConfig const> refs);
  constexpr void outputs(std::span<NamedRef const> refs);
};

} // namespace iv
