#pragma once

#include <intravenous/graph/builder/output_refs.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>

namespace iv {
class GraphBuilder;
namespace details {
struct BuilderSession;

// A BuilderSession owns one LIFO stack of construction scopes. The root scope
// is the whole graph; a definition scope is one module body (or one nested
// live GraphBuilder) with its own virtual-node namespace; a subgraph scope is
// one boundary created by GraphBuilder::subgraph inside its enclosing
// definition's namespace.
enum class BuilderScopeKind : std::uint8_t {
  root,
  definition,
  subgraph,
};

// Scope identities are never reused within a session, so a view of a closed
// scope is detected as stale rather than aliasing a newer scope.
using BuilderScopeId = std::uint64_t;
inline constexpr BuilderScopeId root_builder_scope_id = 0;
} // namespace details

// Callback-facing view of one functional subgraph boundary. It is the session
// plus the scope whose boundary it edits; nodes are created through any
// GraphBuilder view of the same session and land in the session's current
// scope.
class SubgraphBuilder {
  friend class GraphBuilder;

  details::BuilderSession* _session;
  details::BuilderScopeId _scope;

  explicit SubgraphBuilder(
      details::BuilderSession* session, details::BuilderScopeId scope) noexcept
      : _session(session), _scope(scope) {}

public:
  SubgraphBuilder(SubgraphBuilder const&) = delete;
  SubgraphBuilder& operator=(SubgraphBuilder const&) = delete;
  SubgraphBuilder(SubgraphBuilder&&) = delete;
  SubgraphBuilder& operator=(SubgraphBuilder&&) = delete;

  PublicSampleInputRef input();
  template<fixed_string Name, class ChannelType = mono>
    requires requires { ChannelTypeTraits<ChannelType>::id; }
  TypedPublicSampleInputRef<ChannelType> input();
  template<fixed_string Name, class ChannelType = mono>
    requires requires { ChannelTypeTraits<ChannelType>::id; }
  TypedPublicSampleInputRef<ChannelType> input(Sample default_value,
      std::optional<Sample> min = std::nullopt,
      std::optional<Sample> max = std::nullopt);
  template<fixed_string Name, RegisteredContinuousValue... Types>
    requires(sizeof...(Types) != 0)
  PublicSampleInputRef input();
  template<fixed_string Name, RegisteredContinuousValue... Types>
    requires(sizeof...(Types) != 0)
  PublicSampleInputRef input(
      typename ContinuousValueType<Types>::BoundaryInputOptions...);
  template<RegisteredContinuousValue... Types>
    requires(sizeof...(Types) != 0)
  PublicSampleInputRef input();
  template<RegisteredContinuousValue... Types>
    requires(sizeof...(Types) != 0)
  PublicSampleInputRef input(
      typename ContinuousValueType<Types>::BoundaryInputOptions...);
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
