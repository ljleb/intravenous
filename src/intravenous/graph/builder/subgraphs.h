#pragma once

#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_refs.h>
#include <intravenous/graph/builder/node_bundles.h>

#include <concepts>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iv {
class GraphBuilder;
struct ScopedSubgraph {
  size_t start_bundle_index = 0;
  std::string kind{};
  NodeBundleHandle boundary = 0;
  std::vector<SourceInfo> source_infos{};
  bool outputs_defined = false;
  bool event_outputs_defined = false;
};

class SubgraphScopeManager {
public:
  bool active() const;
  ScopedSubgraph& current();
  void begin(std::string_view kind, GraphBuilderNodeBundles&);
  ScopedSubgraph finish();
  void abandon_top();
  void annotate_scope_input_source_info(NodeBundlePortId,
      GraphBuilderNodeBundles const&, SourceInfo);
  SamplePortRef add_scope_sample_input(GraphBuilder&, GraphBuilderNodeBundles&,
      std::string_view, Sample, std::optional<Sample>, std::optional<Sample>, bool);
  EventPortRef add_scope_event_input(GraphBuilder&, GraphBuilderNodeBundles&,
      std::string_view, EventTypeId, bool);
  void define_sample_outputs(std::span<OutputRefConfig const>, GraphBuilder&,
      GraphBuilderNodeBundles&, GraphBuilderIdentity const&);
  template<class LiftSample>
  void define_sample_outputs_from_named_refs(GraphBuilder&, GraphBuilderNodeBundles&,
      GraphBuilderIdentity const&, LiftSample&&, std::span<NamedRef const>);
  void define_event_outputs(std::span<EventOutputRefConfig const>, GraphBuilder&,
      GraphBuilderNodeBundles&, GraphBuilderIdentity const&);
  template<class Fn>
  NodeRef run(GraphBuilder&, GraphBuilderNodeBundles&, Fn&&, std::string_view kind);
  size_t current_start_bundle_index() const;

private:
  NodeRef finalize_scope(GraphBuilder&, GraphBuilderNodeBundles&, ScopedSubgraph);
  std::vector<ScopedSubgraph> _stack{};
};

template<class Fn>
NodeRef SubgraphScopeManager::run(GraphBuilder& builder,
                                  GraphBuilderNodeBundles& bundles,
                                  Fn&& fn, std::string_view kind) {
  static_assert(std::invocable<Fn&> && !std::invocable<Fn&, GraphBuilder&>,
      "iv::GraphBuilder::subgraph(Fn) requires a zero-argument callback; use embed_subgraph(GraphBuilder) for isolated nested builders");
  begin(kind, bundles);
  struct Guard { SubgraphScopeManager* self; bool active=true; ~Guard(){if(active)self->abandon_top();} } guard{this};
  std::forward<Fn>(fn)();
  auto scope = finish(); guard.active = false;
  return finalize_scope(builder, bundles, std::move(scope));
}

template<class LiftSample>
void SubgraphScopeManager::define_sample_outputs_from_named_refs(
    GraphBuilder& builder, GraphBuilderNodeBundles& bundles,
    GraphBuilderIdentity const& identity, LiftSample&& lift_sample,
    std::span<NamedRef const> refs) {
  std::vector<OutputRefConfig> outputs; outputs.reserve(refs.size());
  for (auto const& ref : refs) {
    if (refs.size() > 1 && ref.name.empty())
      details::error("builder " + identity.value + ": outputs(...) requires names when exposing more than one sample output");
    outputs.push_back({.ref = lift_sample(ref), .config = {.name = std::string(ref.name)}});
  }
  define_sample_outputs(outputs, builder, bundles, identity);
}
} // namespace iv
