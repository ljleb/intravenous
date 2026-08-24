#include <intravenous/graph/builder/subgraphs.h>
#include <intravenous/graph/builder.h>

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
