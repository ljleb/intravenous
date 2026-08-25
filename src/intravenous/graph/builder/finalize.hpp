#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/graph/node.h>
#include <intravenous/graph/builder/connections.hpp>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/builder/metadata.h>
#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/builder/public_ports.hpp>
#include <intravenous/graph/builder/virtual_nodes.hpp>
#include <intravenous/graph/generated_node_spec.hpp>
#include <intravenous/graph/reflected_node.hpp>

#include <algorithm>
#include <flat_map>
#include <limits>
#include <optional>
#include <vector>

namespace iv {
struct GraphBuilderIdentity;
class GraphBuilderNodeBundles;
class GraphBuilderConnections;
class GraphBuilderPublicPorts;
class GraphBuilderVirtualNodes;

struct GraphBuilderRootNodeBuildResult {
  Graph graph;
  GraphBuildMetadata metadata;
};

class GraphBuilderFinalizer {
public:
  static constexpr GraphIntrospectionMetadata build_metadata(
      GraphBuilderIdentity const&, LoweredBuilderGraph const&,
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&,
      GraphBuilderConnections const&, size_t detach_id_offset);
  static consteval GraphBuilderRootNodeBuildResult build_root_node(
      GraphBuilderIdentity const&, LoweredBuilderGraph const&,
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&,
      GraphBuilderPublicPorts const&, size_t detach_id_offset,
      bool execution_root = false);
};

namespace {
struct PreparedBuilderGraph {
  GraphBuilderIdentity const& identity;
  LoweredBuilderGraph const& lowered;
  LoweredTopology const& topology;
  GraphBuilderNodeBundles const& node_bundles;
  GraphBuilderVirtualNodes const& virtual_nodes;
  details::PreparedGraph graph{
      .nodes = {}, .explicit_ttl_samples = {}, .node_ids = {},
      .node_virtual_ids = {}, .node_source_infos = {},
      .node_construction_order = {}, .node_kinds = {},
      .node_type_identities = {}, .edges = {}, .event_edges = {},
      .detached_info_by_source = {}, .detached_reader_outputs = {},
  };
  std::vector<size_t> runtime_node_indices;
  std::flat_map<TopologyPortId, TopologyPortId> source_of;
  std::flat_map<TopologyPortId, TopologyEventEdge> event_source_of;

  constexpr PreparedBuilderGraph(
      GraphBuilderIdentity const& identity_,
      LoweredBuilderGraph const& lowered_,
      GraphBuilderNodeBundles const& bundles,
      GraphBuilderVirtualNodes const& virtuals)
      : identity(identity_), lowered(lowered_), topology(lowered_.topology),
        node_bundles(bundles), virtual_nodes(virtuals),
        runtime_node_indices(topology.node_count(), GRAPH_ID) {
    topology.for_each_sample_edge(
        [&](TopologyEdge const& edge) { source_of[edge.target] = edge.source; });
    topology.for_each_event_edge([&](TopologyEventEdge const& edge) {
      event_source_of[edge.target] = edge;
    });
  }

  constexpr std::optional<NodeBundleHandle>
  bundle_for_lowered_node(size_t node) const {
    if (node >= lowered.bundle_by_lowered_node.size()) return std::nullopt;
    return lowered.bundle_by_lowered_node[node];
  }

  constexpr NodeBundleHandle subgraph_bundle_handle(size_t topology_node) const {
    auto const handle = bundle_for_lowered_node(topology_node);
    if (!handle || !node_bundles.bundle(*handle).is_subgraph())
      details::error("lowered subgraph node has no semantic subgraph bundle");
    return *handle;
  }

  constexpr void append_metadata_nodes() {
    for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
      if (topology.is_subgraph_node(node_i)) continue;
      auto const& node = topology.concrete_node(node_i);
      runtime_node_indices[node_i] = graph.nodes.size();
      graph.nodes.push_back(ReflectedNodeDescription{
          .ports = node.ports,
          .type_name = node.type_identity.value,
          .internal_latency_samples = node.internal_latency_samples,
          .maximum_block_size = node.maximum_block_size,
          .default_ttl_samples = node.default_ttl_samples,
          .block_skippable = node.block_skippable,
      });
      append_node_metadata(node_i, node, node.type_identity.value);
    }
  }

  constexpr void append_reflected_nodes(size_t detach_offset) {
    for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
      if (topology.is_subgraph_node(node_i)) continue;
      auto const& node = topology.concrete_node(node_i);
      runtime_node_indices[node_i] = graph.nodes.size();
      ReflectedNodeDescription description;
      if (node.operations.valid()) {
        description = {
            .ports = node.ports,
            .operations = node.operations.apply_detach_id_offset(detach_offset),
            .type_name = node.type_identity.value,
            .internal_latency_samples = node.internal_latency_samples,
            .maximum_block_size = node.maximum_block_size,
            .default_ttl_samples = node.default_ttl_samples,
            .block_skippable = node.block_skippable,
        };
      } else if consteval {
        description = details::reflect_generated_node(node.generated_node);
        description.operations =
            description.operations.apply_detach_id_offset(detach_offset);
      } else {
        details::runtime_graph_builder_node_call_is_forbidden();
      }
      graph.nodes.push_back(description);
      append_node_metadata(node_i, node, std::string(graph.nodes.back().type_name));
    }
  }

  constexpr void append_node_metadata(
      size_t node_i, ConcreteNode const& node, std::string kind) {
    graph.explicit_ttl_samples.push_back(node.lifetime.ttl_samples);
    graph.node_ids.push_back(identity.child_id(node_i));
    std::vector<std::string> virtual_ids;
    std::vector<SourceInfo> source_infos;
    if (auto handle = bundle_for_lowered_node(node_i)) {
      auto const& bundle = node_bundles.bundle(*handle);
      virtual_ids = virtual_nodes.ids_for_bundle(bundle);
      source_infos = bundle.source_annotations().infos;
      for (auto const virtual_handle : bundle.virtual_node_handles()) {
        for (auto const& info : virtual_nodes.record(virtual_handle).source_infos)
          if (std::find(source_infos.begin(), source_infos.end(), info) == source_infos.end())
            source_infos.push_back(info);
      }
    }
    graph.node_virtual_ids.push_back(std::move(virtual_ids));
    graph.node_source_infos.push_back(std::move(source_infos));
    graph.node_construction_order.push_back(node_i);
    graph.node_kinds.push_back(std::move(kind));
    graph.node_type_identities.push_back(node.type_identity.value);
  }

  constexpr ConcretePortId resolve_sample_source(TopologyPortId source) const {
    if (auto boundary = lowered.subgraph_input_of_boundary_source.find(source);
        boundary != lowered.subgraph_input_of_boundary_source.end()) {
      auto incoming = source_of.find(boundary->second);
      if (incoming == source_of.end())
        details::error("subgraph boundary sample source has no incoming connection");
      return resolve_sample_source(incoming->second);
    }
    if (source.node == GRAPH_ID) return {GRAPH_ID, source.port};
    if (source.node >= topology.node_count())
      details::error("unresolved sample boundary topology source");
    if (!topology.is_subgraph_node(source.node))
      return {runtime_node_indices[source.node], source.port};
    auto const& node = topology.subgraph_node(source.node);
    auto passthrough = node.lowered_subgraph.sample_output_sources.at(source.port);
    if (passthrough.node == source.node) {
      auto it = source_of.find(passthrough);
      if (it == source_of.end()) details::error("subgraph sample passthrough has no source");
      return resolve_sample_source(it->second);
    }
    return resolve_sample_source(passthrough);
  }

  constexpr ConcretePortId resolve_event_source(TopologyPortId source) const {
    if (auto boundary = lowered.subgraph_event_input_of_boundary_source.find(source);
        boundary != lowered.subgraph_event_input_of_boundary_source.end()) {
      auto incoming = event_source_of.find(boundary->second);
      if (incoming == event_source_of.end())
        details::error("subgraph boundary event source has no incoming connection");
      return resolve_event_source(incoming->second.source);
    }
    if (source.node == GRAPH_ID) return {GRAPH_ID, source.port};
    if (source.node >= topology.node_count())
      details::error("unresolved event boundary topology source");
    if (!topology.is_subgraph_node(source.node))
      return {runtime_node_indices[source.node], source.port};
    auto const& node = topology.subgraph_node(source.node);
    auto passthrough = node.lowered_subgraph.event_output_sources.at(source.port);
    if (passthrough.node == source.node) {
      auto it = event_source_of.find(passthrough);
      if (it == event_source_of.end()) details::error("subgraph event passthrough has no source");
      return resolve_event_source(it->second.source);
    }
    return resolve_event_source(passthrough);
  }

  constexpr TopologyPortId resolve_sample_source_topology(TopologyPortId source) const {
    if (auto boundary = lowered.subgraph_input_of_boundary_source.find(source);
        boundary != lowered.subgraph_input_of_boundary_source.end()) {
      auto incoming = source_of.find(boundary->second);
      if (incoming == source_of.end())
        details::error("subgraph boundary sample source has no incoming connection");
      return resolve_sample_source_topology(incoming->second);
    }
    if (source.node == GRAPH_ID) return source;
    if (source.node >= topology.node_count())
      details::error("unresolved sample boundary topology source");
    if (!topology.is_subgraph_node(source.node)) return source;
    auto const& node = topology.subgraph_node(source.node);
    auto passthrough = node.lowered_subgraph.sample_output_sources.at(source.port);
    if (passthrough.node == source.node) {
      auto it = source_of.find(passthrough);
      if (it == source_of.end()) details::error("subgraph sample passthrough has no source");
      return resolve_sample_source_topology(it->second);
    }
    return resolve_sample_source_topology(passthrough);
  }

  constexpr TopologyPortId resolve_event_source_topology(TopologyPortId source) const {
    if (auto boundary = lowered.subgraph_event_input_of_boundary_source.find(source);
        boundary != lowered.subgraph_event_input_of_boundary_source.end()) {
      auto incoming = event_source_of.find(boundary->second);
      if (incoming == event_source_of.end())
        details::error("subgraph boundary event source has no incoming connection");
      return resolve_event_source_topology(incoming->second.source);
    }
    if (source.node == GRAPH_ID) return source;
    if (source.node >= topology.node_count())
      details::error("unresolved event boundary topology source");
    if (!topology.is_subgraph_node(source.node)) return source;
    auto const& node = topology.subgraph_node(source.node);
    auto passthrough = node.lowered_subgraph.event_output_sources.at(source.port);
    if (passthrough.node == source.node) {
      auto it = event_source_of.find(passthrough);
      if (it == event_source_of.end()) details::error("subgraph event passthrough has no source");
      return resolve_event_source_topology(it->second.source);
    }
    return resolve_event_source_topology(passthrough);
  }

  constexpr void add_sample_target_edges(ConcretePortId source, TopologyPortId target) {
    if (target.node == GRAPH_ID) {
      graph.edges.emplace(GraphEdge{source, {GRAPH_ID, target.port}});
      return;
    }
    if (!topology.is_subgraph_node(target.node)) {
      graph.edges.emplace(GraphEdge{source, {runtime_node_indices[target.node], target.port}});
      return;
    }
    auto const& node = topology.subgraph_node(target.node);
    for (auto child : node.lowered_subgraph.sample_input_targets.at(target.port))
      add_sample_target_edges(source, child);
  }

  constexpr void add_event_target_edges(GraphEventEdge edge, TopologyPortId target) {
    if (target.node == GRAPH_ID) {
      graph.event_edges.emplace(GraphEventEdge{edge.source, {GRAPH_ID, target.port}, std::move(edge.conversion)});
      return;
    }
    if (!topology.is_subgraph_node(target.node)) {
      graph.event_edges.emplace(GraphEventEdge{edge.source, {runtime_node_indices[target.node], target.port}, std::move(edge.conversion)});
      return;
    }
    auto const& node = topology.subgraph_node(target.node);
    for (auto child : node.lowered_subgraph.event_input_targets.at(target.port))
      add_event_target_edges(GraphEventEdge{edge.source, {}, edge.conversion}, child);
  }

  constexpr void lower_edges() {
    topology.for_each_sample_edge([&](TopologyEdge const& edge) {
      if (lowered.subgraph_input_of_boundary_source.contains(edge.source)) return;
      add_sample_target_edges(resolve_sample_source(edge.source), edge.target);
    });
    topology.for_each_event_edge([&](TopologyEventEdge const& edge) {
      if (lowered.subgraph_event_input_of_boundary_source.contains(edge.source)) return;
      add_event_target_edges(GraphEventEdge{resolve_event_source(edge.source), {}, edge.conversion}, edge.target);
    });
  }

  constexpr ConcretePortId materialize_subgraph_default(size_t subgraph_node, size_t input_port) {
    runtime_node_indices.push_back(graph.nodes.size());
    if consteval {
      graph.nodes.push_back(details::reflect_node(Constant(
          topology.subgraph_node(subgraph_node).inputs()[input_port].default_value)));
    } else {
      details::runtime_graph_builder_node_call_is_forbidden();
    }
    graph.explicit_ttl_samples.push_back(std::nullopt);
    graph.node_ids.push_back(identity.child_id(subgraph_node) + ".default." + std::to_string(input_port));
    graph.node_virtual_ids.emplace_back();
    graph.node_source_infos.emplace_back();
    graph.node_construction_order.push_back(subgraph_node);
    return {graph.nodes.size() - 1, 0};
  }

  constexpr void add_subgraph_default_edges() {
    for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
      if (!topology.is_subgraph_node(node_i)) continue;
      auto const& node = topology.subgraph_node(node_i);
      for (size_t input = 0; input < node.inputs().size(); ++input) {
        TopologyPortId subgraph_input{node_i, input};
        if (source_of.contains(subgraph_input)) continue;
        auto source = materialize_subgraph_default(node_i, input);
        for (auto target : node.lowered_subgraph.sample_input_targets[input])
          add_sample_target_edges(source, target);
      }
    }
  }

  constexpr void copy_detach_info() {
    for (auto const& [source, info] : lowered.detached_info_by_source) {
      auto remapped = resolve_sample_source(source);
      graph.detached_info_by_source.emplace(
          remapped,
          DetachedInfo{
              .detach_id = info.detach_id,
              .original_source = remapped,
              .writer_node = runtime_node_indices[info.writer_node],
              .reader_output = resolve_sample_source(info.reader_output),
              .loop_extra_latency = info.loop_extra_latency,
          });
    }
    for (auto reader : lowered.detached_reader_outputs)
      graph.detached_reader_outputs.insert(resolve_sample_source(reader));
  }

  constexpr iv::LoweredSubgraphSpec::PortRef make_scope_port_ref(TopologyPortId port) const {
    if (port.node == GRAPH_ID)
      return {.node_id = {}, .port = port.port, .is_graph_port = true};
    if (port.node >= topology.node_count() || topology.is_subgraph_node(port.node))
      details::error("scope port did not resolve to a concrete topology node");
    return {.node_id = identity.child_id(port.node), .port = port.port, .is_graph_port = false};
  }

  constexpr void collect_scope_targets(
      TopologyPortId target,
      std::vector<iv::LoweredSubgraphSpec::PortRef>& out_refs) const {
    if (target.node == GRAPH_ID || !topology.is_subgraph_node(target.node)) {
      out_refs.push_back(make_scope_port_ref(target));
      return;
    }
    for (auto child : topology.subgraph_node(target.node).lowered_subgraph.sample_input_targets[target.port])
      collect_scope_targets(child, out_refs);
  }

  constexpr void collect_scope_event_targets(
      TopologyPortId target,
      std::vector<iv::LoweredSubgraphSpec::PortRef>& out_refs) const {
    if (target.node == GRAPH_ID || !topology.is_subgraph_node(target.node)) {
      out_refs.push_back(make_scope_port_ref(target));
      return;
    }
    for (auto child : topology.subgraph_node(target.node).lowered_subgraph.event_input_targets[target.port])
      collect_scope_event_targets(child, out_refs);
  }

  constexpr std::vector<size_t> scope_member_topology_nodes(size_t subgraph_node) const {
    auto const scope_handle = subgraph_bundle_handle(subgraph_node);
    auto const info = node_bundles.subgraph_info(scope_handle);
    auto contains_bundle = [&](NodeBundleHandle handle) {
      return handle >= info.child_begin && handle < info.child_begin + info.child_count;
    };
    auto const node_count = topology.node_count();
    std::vector<bool> anchors(node_count, false);
    std::vector<bool> members(node_count, false);
    for (size_t node = 0; node < node_count; ++node) {
      auto const handle = bundle_for_lowered_node(node);
      if (!handle || !contains_bundle(*handle)) continue;
      anchors[node] = true;
      if (!topology.is_subgraph_node(node)) members[node] = true;
    }
    auto const& binding = topology.subgraph_node(subgraph_node).lowered_subgraph;
    auto seed_generated = [&](TopologyPortId port) {
      if (port.node >= node_count || topology.is_subgraph_node(port.node)) return;
      if (bundle_for_lowered_node(port.node)) return;
      anchors[port.node] = true;
      members[port.node] = true;
    };
    for (auto const& targets : binding.sample_input_targets)
      for (auto target : targets) seed_generated(target);
    for (auto source : binding.sample_output_sources) seed_generated(source);
    for (auto const& targets : binding.event_input_targets)
      for (auto target : targets) seed_generated(target);
    for (auto source : binding.event_output_sources) seed_generated(source);
    bool changed = true;
    while (changed) {
      changed = false;
      auto inspect_edge = [&](TopologyPortId a, TopologyPortId b) {
        if (a.node >= node_count || b.node >= node_count) return;
        auto promote = [&](size_t candidate, size_t neighbor) {
          if (!anchors[neighbor] || anchors[candidate]) return;
          if (topology.is_subgraph_node(candidate)) return;
          if (bundle_for_lowered_node(candidate)) return;
          anchors[candidate] = true;
          members[candidate] = true;
          changed = true;
        };
        promote(a.node, b.node);
        promote(b.node, a.node);
      };
      topology.for_each_sample_edge([&](TopologyEdge const& edge) { inspect_edge(edge.source, edge.target); });
      topology.for_each_event_edge([&](TopologyEventEdge const& edge) { inspect_edge(edge.source, edge.target); });
    }
    std::vector<size_t> result;
    for (size_t node = 0; node < node_count; ++node)
      if (members[node]) result.push_back(node);
    return result;
  }

  constexpr bool authored_scope_contains_subgraph(size_t parent, size_t child) const {
    auto const parent_info = node_bundles.subgraph_info(subgraph_bundle_handle(parent));
    auto const child_handle = subgraph_bundle_handle(child);
    return child_handle >= parent_info.child_begin && child_handle < parent_info.child_begin + parent_info.child_count;
  }

  constexpr std::vector<iv::LoweredSubgraphSpec> build_lowered_scopes() const {
    std::vector<size_t> subgraphs;
    for (size_t i = 0; i < topology.node_count(); ++i)
      if (topology.is_subgraph_node(i)) subgraphs.push_back(i);
    std::flat_map<size_t, size_t> scope_index;
    std::vector<iv::LoweredSubgraphSpec> scopes;
    for (auto subgraph_i : subgraphs) {
      auto const& subgraph = topology.subgraph_node(subgraph_i);
      iv::LoweredSubgraphSpec scope;
      scope.kind = subgraph.lowered_subgraph.kind;
      scope.backing_node_id = identity.child_id(subgraph_i);
      auto const handle = subgraph_bundle_handle(subgraph_i);
      for (auto const& info : node_bundles.bundle(handle).source_annotations().infos) {
        scope.source_infos.push_back(info);
        if (!info.span.file_path.empty() && info.span.begin <= info.span.end &&
            std::find(scope.source_spans.begin(), scope.source_spans.end(), info.span) == scope.source_spans.end())
          scope.source_spans.push_back(info.span);
      }
      scope.sample_inputs = subgraph.ports.sample_inputs;
      scope.sample_outputs = subgraph.ports.sample_outputs;
      scope.event_inputs = subgraph.ports.event_input_configs;
      scope.event_outputs = subgraph.ports.event_output_configs;
      scope.ttl_samples = subgraph.lifetime.ttl_samples;
      for (auto node : scope_member_topology_nodes(subgraph_i))
        scope.member_node_ids.push_back(identity.child_id(node));
      for (auto const& targets : subgraph.lowered_subgraph.sample_input_targets) {
        std::vector<iv::LoweredSubgraphSpec::PortRef> flat;
        for (auto target : targets) collect_scope_targets(target, flat);
        scope.sample_input_targets.push_back(std::move(flat));
      }
      for (auto source : subgraph.lowered_subgraph.sample_output_sources)
        scope.sample_output_sources.push_back(make_scope_port_ref(resolve_sample_source_topology(source)));
      for (auto const& targets : subgraph.lowered_subgraph.event_input_targets) {
        std::vector<iv::LoweredSubgraphSpec::PortRef> flat;
        for (auto target : targets) collect_scope_event_targets(target, flat);
        scope.event_input_targets.push_back(std::move(flat));
      }
      for (auto source : subgraph.lowered_subgraph.event_output_sources)
        scope.event_output_sources.push_back(make_scope_port_ref(resolve_event_source_topology(source)));
      if (scope.member_node_ids.empty()) continue;
      std::sort(scope.member_node_ids.begin(), scope.member_node_ids.end());
      scope.member_node_ids.erase(std::unique(scope.member_node_ids.begin(), scope.member_node_ids.end()), scope.member_node_ids.end());
      scope_index.emplace(subgraph_i, scopes.size());
      scopes.push_back(std::move(scope));
    }
    for (auto child : subgraphs) {
      auto it = scope_index.find(child);
      if (it == scope_index.end()) continue;
      size_t parent = GRAPH_ID;
      size_t best_span = std::numeric_limits<size_t>::max();
      for (auto candidate : subgraphs) {
        if (candidate == child || !scope_index.contains(candidate) || !authored_scope_contains_subgraph(candidate, child)) continue;
        auto const span = node_bundles.subgraph_info(subgraph_bundle_handle(candidate)).child_count;
        if (span < best_span) {
          best_span = span;
          parent = candidate;
        }
      }
      if (parent != GRAPH_ID) scopes[it->second].parent_scope = scope_index.at(parent);
    }
    return scopes;
  }
};
} // namespace

constexpr GraphIntrospectionMetadata GraphBuilderFinalizer::build_metadata(
    GraphBuilderIdentity const& identity, LoweredBuilderGraph const& lowered,
    GraphBuilderNodeBundles const& bundles,
    GraphBuilderVirtualNodes const& virtuals,
    GraphBuilderConnections const& connections, size_t detach_offset) {
  (void)detach_offset;
  PreparedBuilderGraph prepared(identity, lowered, bundles, virtuals);
  prepared.append_metadata_nodes();
  prepared.lower_edges();
  auto scopes = prepared.build_lowered_scopes();
  auto [virtual_nodes, _] = details::build_virtual_metadata(prepared.graph, scopes);
  GraphIntrospectionMetadata metadata;
  metadata.virtual_nodes = std::move(virtual_nodes);
  details::apply_virtual_port_metadata(metadata, bundles, virtuals, connections);
  return metadata;
}

consteval GraphBuilderRootNodeBuildResult GraphBuilderFinalizer::build_root_node(
    GraphBuilderIdentity const& identity, LoweredBuilderGraph const& lowered,
    GraphBuilderNodeBundles const& bundles,
    GraphBuilderVirtualNodes const& virtuals,
    GraphBuilderPublicPorts const& ports, size_t detach_offset,
    bool execution_root) {
  if (!ports.sample_outputs_defined())
    details::error("builder " + identity.value + ": g.outputs(...) must be called before build()");
  auto const sample_inputs = execution_root ? std::span<InputConfig const>{} : ports.sample_inputs(bundles);
  auto const sample_outputs = execution_root ? std::span<OutputConfig const>{} : ports.sample_outputs(bundles);
  auto const event_inputs = execution_root ? std::span<EventInputConfig const>{} : ports.event_inputs(bundles);
  auto const event_outputs = execution_root ? std::span<EventOutputConfig const>{} : ports.event_outputs(bundles);
  PreparedBuilderGraph prepared(identity, lowered, bundles, virtuals);
  prepared.append_reflected_nodes(detach_offset);
  prepared.lower_edges();
  prepared.add_subgraph_default_edges();
  prepared.copy_detach_info();
  details::expand_hyperedge_ports(prepared.graph, identity.value);
  details::stub_dangling_ports(prepared.graph, sample_inputs.size(), identity.value);
  details::validate_graph(prepared.graph, sample_inputs.size(), sample_outputs.size());
  details::validate_detached_edges(prepared.graph, identity.value);
  details::sort_nodes_or_error(prepared.graph, identity.value);
  details::validate_graph(prepared.graph, sample_inputs.size(), sample_outputs.size());
  auto scopes = prepared.build_lowered_scopes();
  auto lowered_subgraphs = details::compile_lowered_subgraphs(prepared.graph, scopes);
  auto [_, virtual_by_backing] = details::build_virtual_metadata(prepared.graph, scopes);
  std::vector<DetachedInfo> detached;
  detached.reserve(prepared.graph.detached_info_by_source.size());
  for (auto const& [_, info] : prepared.graph.detached_info_by_source)
    detached.push_back(info);
  auto execution_plan = details::build_execution_plan(
      prepared.graph.nodes, prepared.graph.edges, prepared.graph.event_edges, detached);
  auto dormancy_groups = details::compile_dormancy_groups(
      prepared.graph, lowered_subgraphs, identity.value, execution_plan);
  auto node_source_infos = std::move(prepared.graph.node_source_infos);
  auto node_type_identities = std::move(prepared.graph.node_type_identities);
  return {
      .graph = Graph(details::build_graph_artifact(
          identity.value, std::move(prepared.graph.nodes),
          std::move(prepared.graph.explicit_ttl_samples),
          std::move(prepared.graph.node_ids), std::move(prepared.graph.edges),
          std::move(prepared.graph.event_edges), std::move(detached),
          std::move(execution_plan),
          std::vector<InputConfig>(sample_inputs.begin(), sample_inputs.end()),
          std::vector<OutputConfig>(sample_outputs.begin(), sample_outputs.end()),
          std::vector<EventInputConfig>(event_inputs.begin(), event_inputs.end()),
          std::vector<EventOutputConfig>(event_outputs.begin(), event_outputs.end()),
          std::move(dormancy_groups))),
      .metadata = {
          .lowered_subgraphs = std::move(lowered_subgraphs),
          .concrete_node_type_identities = std::move(node_type_identities),
          .node_source_infos = std::move(node_source_infos),
          .virtual_node_ids_by_backing_node_id = std::move(virtual_by_backing),
      }};
}
} // namespace iv
