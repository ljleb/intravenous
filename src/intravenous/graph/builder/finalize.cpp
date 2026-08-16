#include <intravenous/graph/builder/finalize.h>

#include <intravenous/graph/builder/connections.h>
#include <intravenous/graph/builder/detach.h>
#include <intravenous/graph/builder/metadata.h>
#include <intravenous/graph/builder/node_bundles.h>
#include <intravenous/graph/builder/public_ports.h>
#include <intravenous/graph/builder/topology.h>
#include <intravenous/graph/builder/virtual_nodes.h>

#include <algorithm>
#include <limits>

namespace iv {
namespace {
struct PreparedBuilderGraph {
  GraphBuilderIdentity const &identity;
  GraphBuilderTopology const &topology;
  GraphBuilderNodeBundles const &node_bundles;
  GraphBuilderVirtualNodes const &virtual_nodes;
  GraphBuilderConnections const *connections;
  GraphBuilderDetach const *detach;
  details::PreparedGraph graph{
      .nodes = {},
      .explicit_ttl_samples = {},
      .node_ids = {},
      .node_virtual_ids = {},
      .node_source_infos = {},
      .node_construction_order = {},
      .node_kinds = {},
      .node_type_identities = {},
      .edges = {},
      .event_edges = {},
      .timeline_filled_input_ports = {},
      .timeline_filled_event_input_ports = {},
      .detached_info_by_source = {},
      .detached_reader_outputs = {},
  };
  std::vector<size_t> runtime_node_indices;
  std::unordered_map<ConcretePortId, ConcretePortId> source_of;
  std::unordered_map<ConcretePortId, GraphEventEdge> event_source_of;

  PreparedBuilderGraph(GraphBuilderIdentity const &identity_,
                       GraphBuilderTopology const &topology_,
                       GraphBuilderNodeBundles const &node_bundles_,
                       GraphBuilderVirtualNodes const &virtual_nodes_,
                       GraphBuilderConnections const *connections_ = nullptr,
                       GraphBuilderDetach const *detach_ = nullptr)
      : identity(identity_), topology(topology_), node_bundles(node_bundles_), virtual_nodes(virtual_nodes_),
        connections(connections_), detach(detach_),
        runtime_node_indices(topology_.node_count(), GRAPH_ID) {
    topology.for_each_sample_edge(
        [&](GraphEdge const &edge) { source_of[edge.target] = edge.source; });
    topology.for_each_event_edge([&](GraphEventEdge const &edge) {
      event_source_of[edge.target] = edge;
    });
  }

  void append_metadata_nodes() {
    for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
      if (topology.is_subgraph_node(node_i)) {
        continue;
      }
      auto const &node = topology.concrete_node(node_i);
      runtime_node_indices[node_i] = graph.nodes.size();
      graph.nodes.push_back(TypeErasedNode(details::MetadataGraphNode{
          .input_configs = node.ports.sample_inputs,
          .output_configs = node.ports.sample_outputs,
          .event_input_configs = node.ports.event_input_configs,
          .event_output_configs = node.ports.event_output_configs,
      }));
      append_node_metadata(node_i, node, node.type_identity.value);
    }
  }

  void append_materialized_nodes(size_t detach_id_offset) {
    for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
      if (topology.is_subgraph_node(node_i)) {
        continue;
      }
      auto const &node = topology.concrete_node(node_i);
      runtime_node_indices[node_i] = graph.nodes.size();
      graph.nodes.push_back(node.materialization.make(detach_id_offset));
      append_node_metadata(node_i, node, graph.nodes.back().type_name());
    }
  }

  void append_node_metadata(size_t node_i, ConcreteNode const &node,
                            std::string kind) {
    graph.explicit_ttl_samples.push_back(node.lifetime.ttl_samples);
    graph.node_ids.push_back(identity.child_id(node_i));
    auto const &bundle = node_bundles.bundle(
        node_bundles.bundle_for_concrete_node(node_i));
    graph.node_virtual_ids.push_back(virtual_nodes.ids_for_bundle(bundle));
    auto source_infos = bundle.source_annotations.infos;
    for (auto const handle : bundle.virtual_node_handles) {
      for (auto const &info : virtual_nodes.record(handle).source_infos) {
        if (std::find(source_infos.begin(), source_infos.end(), info) ==
            source_infos.end()) {
          source_infos.push_back(info);
        }
      }
    }
    graph.node_source_infos.push_back(std::move(source_infos));
    graph.node_construction_order.push_back(node_i);
    graph.node_kinds.push_back(std::move(kind));
    graph.node_type_identities.push_back(node.type_identity.value);
  }

  ConcretePortId resolve_sample_source(ConcretePortId source) const {
    if (source.node == GRAPH_ID) {
      return source;
    }
    if (!topology.is_subgraph_node(source.node)) {
      return ConcretePortId{runtime_node_indices[source.node], source.port};
    }
    auto const &node = topology.subgraph_node(source.node);
    ConcretePortId const passthrough_source =
        node.lowered_subgraph.sample_output_sources[source.port];
    if (passthrough_source.node == source.node) {
      return resolve_sample_source(source_of.at(passthrough_source));
    }
    return resolve_sample_source(passthrough_source);
  }

  ConcretePortId resolve_event_source(ConcretePortId source) const {
    if (source.node == GRAPH_ID) {
      return source;
    }
    if (!topology.is_subgraph_node(source.node)) {
      return ConcretePortId{runtime_node_indices[source.node], source.port};
    }
    auto const &node = topology.subgraph_node(source.node);
    ConcretePortId const passthrough_source =
        node.lowered_subgraph.event_output_sources[source.port];
    if (passthrough_source.node == source.node) {
      return resolve_event_source(
          event_source_of.at(passthrough_source).source);
    }
    return resolve_event_source(passthrough_source);
  }

  void add_sample_target_edges(ConcretePortId source, ConcretePortId target) {
    if (target.node == GRAPH_ID) {
      graph.edges.emplace(GraphEdge{source, target});
      return;
    }

    if (!topology.is_subgraph_node(target.node)) {
      graph.edges.emplace(
          GraphEdge{source, {runtime_node_indices[target.node], target.port}});
      return;
    }
    auto const &target_node = topology.subgraph_node(target.node);

    for (ConcretePortId const child_target :
         target_node.lowered_subgraph.sample_input_targets[target.port]) {
      add_sample_target_edges(source, child_target);
    }
  }

  void add_event_target_edges(GraphEventEdge edge, ConcretePortId target) {
    if (target.node == GRAPH_ID) {
      graph.event_edges.emplace(
          GraphEventEdge{edge.source, target, std::move(edge.conversion)});
      return;
    }

    if (!topology.is_subgraph_node(target.node)) {
      graph.event_edges.emplace(
          GraphEventEdge{edge.source,
                         {runtime_node_indices[target.node], target.port},
                         std::move(edge.conversion)});
      return;
    }
    auto const &target_node = topology.subgraph_node(target.node);

    for (ConcretePortId const child_target :
         target_node.lowered_subgraph.event_input_targets[target.port]) {
      add_event_target_edges(
          GraphEventEdge{edge.source, child_target, edge.conversion},
          child_target);
    }
  }

  void lower_edges() {
    topology.for_each_sample_edge([&](GraphEdge const &edge) {
      add_sample_target_edges(resolve_sample_source(edge.source), edge.target);
    });
    topology.for_each_event_edge([&](GraphEventEdge const &edge) {
      GraphEventEdge resolved = edge;
      resolved.source = resolve_event_source(edge.source);
      add_event_target_edges(std::move(resolved), edge.target);
    });
  }

  void copy_runtime_filled_inputs() {
    IV_ASSERT(connections != nullptr,
              "copy_runtime_filled_inputs requires GraphBuilderConnections");
    connections->for_each_runtime_filled_sample_input([&](ConcretePortId port) {
      if (port.node == GRAPH_ID ||
          topology.is_subgraph_node(port.node)) {
        return;
      }
      graph.timeline_filled_input_ports.insert(
          ConcretePortId{runtime_node_indices[port.node], port.port});
    });
    connections->for_each_runtime_filled_event_input([&](ConcretePortId port) {
      if (port.node == GRAPH_ID ||
          topology.is_subgraph_node(port.node)) {
        return;
      }
      graph.timeline_filled_event_input_ports.insert(
          ConcretePortId{runtime_node_indices[port.node], port.port});
    });
  }

  ConcretePortId materialize_subgraph_default(size_t subgraph_node,
                                         size_t input_port) {
    runtime_node_indices.push_back(graph.nodes.size());
    graph.nodes.push_back(TypeErasedNode(Constant(
        topology.subgraph_node(subgraph_node).inputs()[input_port].default_value)));
    graph.explicit_ttl_samples.push_back(std::nullopt);
    graph.node_ids.push_back(identity.child_id(subgraph_node) + ".default." +
                             std::to_string(input_port));
    graph.node_virtual_ids.emplace_back();
    graph.node_source_infos.emplace_back();
    graph.node_construction_order.push_back(subgraph_node);
    return ConcretePortId{graph.nodes.size() - 1, 0};
  }

  void add_subgraph_default_edges() {
    for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
      if (!topology.is_subgraph_node(node_i)) {
        continue;
      }
      auto const &node = topology.subgraph_node(node_i);
      for (size_t input_port = 0; input_port < node.inputs().size();
           ++input_port) {
        ConcretePortId const subgraph_input{node_i, input_port};
        if (source_of.contains(subgraph_input)) {
          continue;
        }
        ConcretePortId const default_source =
            materialize_subgraph_default(node_i, input_port);
        for (ConcretePortId const child_target :
             node.lowered_subgraph.sample_input_targets[input_port]) {
          add_sample_target_edges(default_source, child_target);
        }
      }
    }
  }

  void copy_detach_info() {
    IV_ASSERT(detach != nullptr,
              "copy_detach_info requires GraphBuilderDetach");
    detach->for_each_info(
        [&](ConcretePortId source, DetachedSamplePortInfo const &info) {
          ConcretePortId const remapped_source = resolve_sample_source(source);
          graph.detached_info_by_source.emplace(
              remapped_source,
              DetachedSamplePortInfo{
                  .detach_id = info.detach_id,
                  .original_source = remapped_source,
                  .writer_node = runtime_node_indices[info.writer_node],
                  .reader_output = resolve_sample_source(info.reader_output),
                  .loop_extra_latency = info.loop_extra_latency,
              });
        });
    detach->for_each_reader_output([&](ConcretePortId reader_output) {
      graph.detached_reader_outputs.insert(
          resolve_sample_source(reader_output));
    });
  }

  iv::LoweredSubgraphSpec::PortRef make_scope_port_ref(ConcretePortId port) const {
    return iv::LoweredSubgraphSpec::PortRef{
        .node_id = port.node == GRAPH_ID ? std::string{}
                                         : identity.child_id(port.node),
        .port = port.port,
        .is_graph_port = port.node == GRAPH_ID,
    };
  }

  void collect_scope_targets(
      ConcretePortId target, std::vector<iv::LoweredSubgraphSpec::PortRef> &out) const {
    if (target.node == GRAPH_ID || !topology.is_subgraph_node(target.node)) {
      out.push_back(make_scope_port_ref(target));
      return;
    }
    for (ConcretePortId const child_target :
         topology.subgraph_node(target.node)
             .lowered_subgraph.sample_input_targets[target.port]) {
      collect_scope_targets(child_target, out);
    }
  }

  void collect_scope_event_targets(
      ConcretePortId target, std::vector<iv::LoweredSubgraphSpec::PortRef> &out) const {
    if (target.node == GRAPH_ID || !topology.is_subgraph_node(target.node)) {
      out.push_back(make_scope_port_ref(target));
      return;
    }
    for (ConcretePortId const child_target :
         topology.subgraph_node(target.node)
             .lowered_subgraph.event_input_targets[target.port]) {
      collect_scope_event_targets(child_target, out);
    }
  }

  std::vector<iv::LoweredSubgraphSpec> build_lowered_scopes() const {
    std::vector<size_t> subgraph_indices;
    for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
      if (topology.is_subgraph_node(node_i)) {
        subgraph_indices.push_back(node_i);
      }
    }

    auto encloses = [&](size_t parent, size_t child) {
      auto const &parent_node = topology.subgraph_node(parent);
      auto const &child_node = topology.subgraph_node(child);
      size_t const parent_begin = parent_node.lowered_subgraph.begin;
      size_t const parent_end =
          parent_begin + parent_node.lowered_subgraph.count;
      size_t const child_begin = child_node.lowered_subgraph.begin;
      size_t const child_end = child_begin + child_node.lowered_subgraph.count;
      return parent_begin <= child_begin && child_end <= parent_end;
    };

    std::unordered_map<size_t, size_t> scope_index_by_subgraph;
    std::vector<iv::LoweredSubgraphSpec> scopes;

    for (size_t subgraph_i : subgraph_indices) {
      auto const &subgraph = topology.subgraph_node(subgraph_i);
      iv::LoweredSubgraphSpec scope;
      scope.kind = subgraph.lowered_subgraph.kind;
      scope.backing_node_id = identity.child_id(subgraph_i);
      auto const &subgraph_bundle = node_bundles.bundle(
          node_bundles.bundle_for_concrete_node(subgraph_i));
      for (auto const &info : subgraph_bundle.source_annotations.infos) {
        scope.source_infos.push_back(info);
        if (info.span.file_path.empty() || info.span.begin > info.span.end) {
          continue;
        }
        if (std::find(scope.source_spans.begin(), scope.source_spans.end(),
                      info.span) == scope.source_spans.end()) {
          scope.source_spans.push_back(info.span);
        }
      }
      scope.sample_inputs = subgraph.ports.sample_inputs;
      scope.sample_outputs = subgraph.ports.sample_outputs;
      scope.event_inputs = subgraph.ports.event_input_configs;
      scope.event_outputs = subgraph.ports.event_output_configs;
      scope.ttl_samples = subgraph.lifetime.ttl_samples;

      size_t const begin = subgraph.lowered_subgraph.begin;
      size_t const end = begin + subgraph.lowered_subgraph.count;
      for (size_t node_i = begin; node_i < end; ++node_i) {
        if (topology.is_subgraph_node(node_i)) {
          continue;
        }
        scope.member_node_ids.push_back(identity.child_id(node_i));
      }

      scope.sample_input_targets.reserve(
          subgraph.lowered_subgraph.sample_input_targets.size());
      for (auto const &targets :
           subgraph.lowered_subgraph.sample_input_targets) {
        std::vector<iv::LoweredSubgraphSpec::PortRef> flattened_targets;
        for (ConcretePortId const target : targets) {
          collect_scope_targets(target, flattened_targets);
        }
        scope.sample_input_targets.push_back(std::move(flattened_targets));
      }

      scope.sample_output_sources.reserve(
          subgraph.lowered_subgraph.sample_output_sources.size());
      for (ConcretePortId const source :
           subgraph.lowered_subgraph.sample_output_sources) {
        scope.sample_output_sources.push_back(
            make_scope_port_ref(resolve_sample_source(source)));
      }

      scope.event_input_targets.reserve(
          subgraph.lowered_subgraph.event_input_targets.size());
      for (auto const &targets :
           subgraph.lowered_subgraph.event_input_targets) {
        std::vector<iv::LoweredSubgraphSpec::PortRef> flattened_targets;
        for (ConcretePortId const target : targets) {
          collect_scope_event_targets(target, flattened_targets);
        }
        scope.event_input_targets.push_back(std::move(flattened_targets));
      }

      scope.event_output_sources.reserve(
          subgraph.lowered_subgraph.event_output_sources.size());
      for (ConcretePortId const source :
           subgraph.lowered_subgraph.event_output_sources) {
        scope.event_output_sources.push_back(
            make_scope_port_ref(resolve_event_source(source)));
      }

      if (scope.member_node_ids.empty()) {
        continue;
      }

      std::sort(scope.member_node_ids.begin(), scope.member_node_ids.end());
      scope.member_node_ids.erase(std::unique(scope.member_node_ids.begin(),
                                              scope.member_node_ids.end()),
                                  scope.member_node_ids.end());
      scope_index_by_subgraph.emplace(subgraph_i, scopes.size());
      scopes.push_back(std::move(scope));
    }

    for (size_t subgraph_i : subgraph_indices) {
      auto scope_it = scope_index_by_subgraph.find(subgraph_i);
      if (scope_it == scope_index_by_subgraph.end()) {
        continue;
      }
      size_t const scope_i = scope_it->second;

      size_t best_parent_subgraph = GRAPH_ID;
      size_t best_parent_span = std::numeric_limits<size_t>::max();
      for (size_t other_subgraph : subgraph_indices) {
        if (other_subgraph == subgraph_i ||
            !encloses(other_subgraph, subgraph_i)) {
          continue;
        }
        size_t const span =
            topology.subgraph_node(other_subgraph).lowered_subgraph.count;
        if (span < best_parent_span &&
            scope_index_by_subgraph.contains(other_subgraph)) {
          best_parent_span = span;
          best_parent_subgraph = other_subgraph;
        }
      }
      if (best_parent_subgraph != GRAPH_ID) {
        scopes[scope_i].parent_scope =
            scope_index_by_subgraph.at(best_parent_subgraph);
      }
    }

    return scopes;
  }
};
} // namespace

GraphIntrospectionMetadata GraphBuilderFinalizer::build_metadata(
    GraphBuilderIdentity const &identity, GraphBuilderTopology const &topology,
    GraphBuilderNodeBundles const &node_bundles,
    GraphBuilderVirtualNodes const &virtual_nodes, size_t detach_id_offset) {
  (void)detach_id_offset;
  PreparedBuilderGraph prepared(identity, topology, node_bundles, virtual_nodes);
  prepared.append_metadata_nodes();
  prepared.lower_edges();

  auto lowered_scopes = prepared.build_lowered_scopes();
  auto [introspection_virtual_nodes, _] =
      details::build_virtual_metadata(prepared.graph, lowered_scopes);
  GraphIntrospectionMetadata metadata{
      .virtual_nodes = std::move(introspection_virtual_nodes)};
  details::apply_virtual_port_metadata(metadata, topology, node_bundles,
                                       virtual_nodes);
  return metadata;
}

GraphBuilderRootNodeBuildResult GraphBuilderFinalizer::build_root_node(
    GraphBuilderIdentity const &identity, GraphBuilderTopology const &topology,
    GraphBuilderNodeBundles const &node_bundles,
    GraphBuilderVirtualNodes const &virtual_nodes,
    GraphBuilderConnections const &connections,
    GraphBuilderPublicPorts const &public_ports,
    GraphBuilderDetach const &detach, size_t detach_id_offset) {
  if (!public_ports.sample_outputs_defined()) {
    details::error("builder " + identity.value +
                   ": g.outputs(...) must be called before build()");
  }

  PreparedBuilderGraph prepared(identity, topology, node_bundles, virtual_nodes, &connections,
                                &detach);
  prepared.append_materialized_nodes(detach_id_offset);
  prepared.copy_runtime_filled_inputs();
  prepared.lower_edges();
  prepared.add_subgraph_default_edges();
  prepared.copy_detach_info();

  details::expand_hyperedge_ports(prepared.graph, public_ports.sample_outputs(),
                                  identity.value);
  details::stub_dangling_ports(
      prepared.graph, public_ports.sample_inputs().size(), identity.value);
  details::validate_graph(prepared.graph, public_ports.sample_inputs().size(),
                          public_ports.sample_outputs().size());
  details::validate_detached_edges(prepared.graph, identity.value);
  details::sort_nodes_or_error(prepared.graph, identity.value);
  details::validate_graph(prepared.graph, public_ports.sample_inputs().size(),
                          public_ports.sample_outputs().size());

  auto lowered_scopes = prepared.build_lowered_scopes();
  auto lowered_subgraphs =
      details::compile_lowered_subgraphs(prepared.graph, lowered_scopes);
  auto [_, virtual_node_ids_by_backing_node_id] =
      details::build_virtual_metadata(prepared.graph, lowered_scopes);

  auto detached = [&] {
    std::vector<DetachedInfo> detached_info;
    detached_info.reserve(prepared.graph.detached_info_by_source.size());
    for (auto const &[_, info] : prepared.graph.detached_info_by_source) {
      detached_info.push_back(info);
    }
    return detached_info;
  }();
  auto execution_plan =
      details::build_execution_plan(prepared.graph.nodes, prepared.graph.edges,
                                    prepared.graph.event_edges, detached);
  auto dormancy_groups = details::compile_dormancy_groups(
      prepared.graph, lowered_subgraphs, identity.value, execution_plan);

  auto node_source_infos = std::move(prepared.graph.node_source_infos);
  auto node_type_identities = std::move(prepared.graph.node_type_identities);
  return GraphBuilderRootNodeBuildResult{
      .graph = Graph(details::build_graph_artifact(
          identity.value, std::move(prepared.graph.nodes),
          std::move(prepared.graph.explicit_ttl_samples),
          std::move(prepared.graph.node_ids), std::move(prepared.graph.edges),
          std::move(prepared.graph.event_edges), std::move(detached),
          std::move(execution_plan),
          std::vector<InputConfig>(public_ports.sample_inputs().begin(),
                                   public_ports.sample_inputs().end()),
          std::vector<OutputConfig>(public_ports.sample_outputs().begin(),
                                    public_ports.sample_outputs().end()),
          std::vector<EventInputConfig>(public_ports.event_inputs().begin(),
                                        public_ports.event_inputs().end()),
          std::vector<EventOutputConfig>(public_ports.event_outputs().begin(),
                                         public_ports.event_outputs().end()),
          std::move(dormancy_groups))),
      .metadata =
          GraphBuildMetadata{
              .lowered_subgraphs = std::move(lowered_subgraphs),
              .concrete_node_type_identities = std::move(node_type_identities),
              .node_source_infos = std::move(node_source_infos),
              .virtual_node_ids_by_backing_node_id =
                  std::move(virtual_node_ids_by_backing_node_id),
          },
  };
}

} // namespace iv
