#pragma once

#include <intravenous/graph/builder/output_refs.h>

#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>

namespace iv {
class GraphBuilder;
namespace details {
struct SubgraphBuildScope;
}

// Callback-facing facade for one functional subgraph boundary. Nodes remain
// built on the enclosing GraphBuilder; the opaque scope selects the boundary
// interface held by libiv_builder.
class SubgraphBuilder {
  friend class GraphBuilder;

  GraphBuilder& _builder;
  details::SubgraphBuildScope* _scope;

  explicit SubgraphBuilder(
      GraphBuilder& builder, details::SubgraphBuildScope* scope)
      : _builder(builder), _scope(scope) {}

public:
  SubgraphBuilder(SubgraphBuilder const&) = delete;
  SubgraphBuilder& operator=(SubgraphBuilder const&) = delete;
  SubgraphBuilder(SubgraphBuilder&&) = delete;
  SubgraphBuilder& operator=(SubgraphBuilder&&) = delete;

  PublicSampleInputRef input();
  template<fixed_string Name, class ChannelType = mono>
  TypedPublicSampleInputRef<ChannelType> input(Sample default_value = 0.0,
      std::optional<Sample> min = std::nullopt,
      std::optional<Sample> max = std::nullopt);
  PublicSampleInputRef input(Sample default_value,
      std::optional<Sample> min = std::nullopt,
      std::optional<Sample> max = std::nullopt);
  template<fixed_string Name>
  PublicEventInputRef event_input(EventTypeId type);
  PublicEventInputRef event_input(EventTypeId type);

  template<class... Refs> void outputs(Refs&&... refs);
  void outputs(std::initializer_list<NamedRef>);
  void outputs(std::span<NamedRef const>);
  void outputs(std::span<SampleOutputRequest const>);
  template<class... Refs> void event_outputs(Refs&&... refs);
  void event_outputs(std::span<EventOutputRequest const>);
};
} // namespace iv
