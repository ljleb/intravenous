#include <intravenous/graph/builder/subgraphs.h>
#include <intravenous/graph/builder.h>

#include <algorithm>
#include <limits>

namespace iv {
namespace {
void append_scope_source_info(ScopedSubgraph& scope, SourceInfo info) {
  if (!std::ranges::contains(scope.source_infos, info)) scope.source_infos.push_back(std::move(info));
}
}
bool SubgraphScopeManager::active() const { return !_stack.empty(); }
ScopedSubgraph& SubgraphScopeManager::current() { IV_ASSERT(active(), "current() requires an active subgraph scope"); return _stack.back(); }
void SubgraphScopeManager::begin(std::string_view kind, GraphBuilderNodeBundles& bundles) {
  auto boundary = bundles.append_scope_boundary();
  _stack.push_back({.start_bundle_index = bundles.size(), .kind = std::string(kind), .boundary = boundary});
}
ScopedSubgraph SubgraphScopeManager::finish() { auto result=std::move(_stack.back()); _stack.pop_back(); return result; }
void SubgraphScopeManager::abandon_top() { _stack.pop_back(); }

SamplePortRef SubgraphScopeManager::add_scope_sample_input(
    GraphBuilder& builder, GraphBuilderNodeBundles& bundles, std::string_view name,
    Sample value, std::optional<Sample> min, std::optional<Sample> max, bool has_name) {
  auto& scope=current();
  auto ordinal=bundles.bundle(scope.boundary).append_boundary_sample_input({
      .name=has_name?std::string(name):std::string{}, .default_value=value,
      .min=min.value_or(-std::numeric_limits<Sample::storage>::infinity()),
      .max=max.value_or(std::numeric_limits<Sample::storage>::infinity())});
  return SamplePortRef(builder,{scope.boundary,PortKind::sample,ordinal});
}
EventPortRef SubgraphScopeManager::add_scope_event_input(
    GraphBuilder& builder, GraphBuilderNodeBundles& bundles, std::string_view name,
    EventTypeId type, bool has_name) {
  auto& scope=current();
  auto ordinal=bundles.bundle(scope.boundary).append_boundary_event_input(
      {.name=has_name?std::string(name):std::string{},.type=type});
  return EventPortRef(builder,{scope.boundary,PortKind::event,ordinal});
}
void SubgraphScopeManager::annotate_scope_input_source_info(
    NodeBundlePortId boundary, GraphBuilderNodeBundles const& bundles, SourceInfo info) {
  auto& scope=current();
  if (boundary.node_bundle_handle != scope.boundary)
    details::error("attempted to annotate a scope input from another scope");
  auto const& b=bundles.bundle(scope.boundary);
  auto count=boundary.port_kind==PortKind::sample?b.boundary_sample_inputs().size():b.boundary_event_inputs().size();
  if(boundary.port_ordinal>=count)details::error("scope input ordinal is out of bounds");
  append_scope_source_info(scope,std::move(info));
}
void SubgraphScopeManager::define_sample_outputs(
    std::span<OutputRefConfig const> refs, GraphBuilder& builder,
    GraphBuilderNodeBundles& bundles, GraphBuilderIdentity const& identity) {
  auto& scope=current(); auto& boundary=bundles.bundle(scope.boundary);
  for(size_t i=0;i<refs.size();++i){
    if(refs[i].ref.graph_builder!=&builder)details::error("builder "+identity.value+": subgraph output belongs to another builder");
    if(refs.size()>1&&refs[i].config.name.empty())details::error("builder "+identity.value+": subgraph outputs(...) requires names when exposing more than one sample output");
    auto ordinal=boundary.append_boundary_sample_output(refs[i].config);
    builder.record_authored_sample_connection({scope.boundary,PortKind::sample,ordinal},refs[i].ref);
  }
  scope.outputs_defined=true;
}
void SubgraphScopeManager::define_event_outputs(
    std::span<EventOutputRefConfig const> refs, GraphBuilder& builder,
    GraphBuilderNodeBundles& bundles, GraphBuilderIdentity const& identity) {
  auto& scope=current(); if(scope.event_outputs_defined)details::error("subgraph event_outputs(...) was already called on builder "+identity.value);
  auto& boundary=bundles.bundle(scope.boundary); boundary.clear_boundary_event_outputs();
  for(size_t i=0;i<refs.size();++i){
    auto const& ref=refs[i].ref; if(ref.graph_builder!=&builder)details::error("builder "+identity.value+": subgraph event output belongs to another builder");
    if(refs.size()>1&&refs[i].config.name.empty())details::error("builder "+identity.value+": subgraph event_outputs(...) requires names when exposing more than one event output");
    auto config=refs[i].config; config.type=ref.type; auto ordinal=boundary.append_boundary_event_output(std::move(config));
    builder.record_authored_event_connection({scope.boundary,PortKind::event,ordinal},ref);
  }
  scope.event_outputs_defined=true;
}
NodeRef SubgraphScopeManager::finalize_scope(
    GraphBuilder& builder, GraphBuilderNodeBundles& bundles, ScopedSubgraph scope) {
  auto child_count=bundles.size()-scope.start_bundle_index;
  NodeRef result(builder,bundles.append_subgraph(scope.boundary,scope.start_bundle_index,child_count,scope.kind));
  for(auto const& info:scope.source_infos)result._annotate_source_info(info.declaration_identity,info.span.file_path,info.span.begin,info.span.end);
  return result;
}
size_t SubgraphScopeManager::current_start_bundle_index() const { return _stack.back().start_bundle_index; }
} // namespace iv
