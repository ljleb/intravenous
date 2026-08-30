#pragma once

#include <intravenous/basic_nodes/type_erased.h>
#include <intravenous/graph/authored_graph.hpp>
#include <intravenous/graph/executable_graph_ir.hpp>
#include <intravenous/graph/node.h>
#include <intravenous/graph/builder/connections.hpp>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/builder/metadata.hpp>
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
struct CompiledGraph {
  Graph graph;
  GraphBuildMetadata metadata;
  GraphIntrospectionMetadata introspection;
};

class GraphCompiler {
public:
  static consteval CompiledGraph compile(ExecutableGraphIR);
};

// namespace

consteval ExecutableGraphIR GraphLowerer::lower(
    AuthoredGraph const& authored, GraphLoweringOptions options) {
  details::LoweringWorkspace lowered;
  GraphLowerer lowerer(
      authored.identity, authored.node_bundles, authored.connections,
      authored.public_ports, authored.virtual_nodes, authored.detach, lowered,
      options.execution_root);
  lowerer.run();
  lowerer.begin_materialization();
  lowerer.append_reflected_nodes(options.detach_id_offset);
  lowerer.lower_edges();
  lowerer.add_subgraph_default_edges();
  lowerer.copy_detach_info();

  auto sample_inputs = options.execution_root
      ? std::vector<InputConfig>{}
      : std::vector<InputConfig>(
            authored.public_ports.sample_inputs(authored.node_bundles).begin(),
            authored.public_ports.sample_inputs(authored.node_bundles).end());
  auto sample_outputs = options.execution_root
      ? std::vector<OutputConfig>{}
      : std::vector<OutputConfig>(
            authored.public_ports.sample_outputs(authored.node_bundles).begin(),
            authored.public_ports.sample_outputs(authored.node_bundles).end());
  auto event_inputs = options.execution_root
      ? std::vector<EventInputConfig>{}
      : std::vector<EventInputConfig>(
            authored.public_ports.event_inputs(authored.node_bundles).begin(),
            authored.public_ports.event_inputs(authored.node_bundles).end());
  auto event_outputs = options.execution_root
      ? std::vector<EventOutputConfig>{}
      : std::vector<EventOutputConfig>(
            authored.public_ports.event_outputs(authored.node_bundles).begin(),
            authored.public_ports.event_outputs(authored.node_bundles).end());

  // These are semantic completion steps, so they belong before the executable
  // boundary rather than in GraphCompiler.
  details::expand_hyperedge_ports(lowerer.graph, authored.identity.value);
  details::stub_dangling_ports(
      lowerer.graph, sample_inputs.size(), authored.identity.value);
  details::validate_graph(lowerer.graph, sample_inputs.size(),
                          sample_outputs.size());
  details::validate_detached_edges(lowerer.graph, authored.identity.value);

  auto scopes = lowerer.build_lowered_scopes();
  auto [virtual_nodes, _] = details::build_virtual_metadata(lowerer.graph, scopes);
  GraphIntrospectionMetadata introspection;
  introspection.virtual_nodes = std::move(virtual_nodes);
  details::apply_virtual_port_metadata(
      introspection, authored.node_bundles, authored.virtual_nodes,
      authored.connections);

  auto sample_families =
      authored.public_ports.sample_input_families(authored.node_bundles);
  for (auto& family : sample_families.families) {
    family.authored_connected = std::ranges::any_of(
        family.channels, [&](auto const& channel) {
          return std::ranges::any_of(channel.port_ordinals, [&](auto ordinal) {
            auto channels = authored.node_bundles.sample_output_channels(
                {authored.public_ports.boundary_handle(), PortKind::sample, ordinal});
            return std::ranges::any_of(channels, [&](auto c) {
              return authored.connections.sample_output_is_connected(c);
            });
          });
        });
  }
  introspection.public_sample_inputs = std::move(sample_families.families);
  introspection.public_event_inputs =
      authored.public_ports.collected_event_inputs(authored.node_bundles);
  for (auto& input : introspection.public_event_inputs) {
    auto ports = authored.node_bundles.event_output_ports(
        {authored.public_ports.boundary_handle(), PortKind::event,
         input.port_ordinal});
    input.graph_connected = std::ranges::any_of(ports, [&](auto p) {
      return authored.connections.event_output_is_connected(p);
    });
  }
  introspection.public_sample_outputs =
      authored.public_ports.sample_output_families(authored.node_bundles).families;
  introspection.public_event_outputs =
      authored.public_ports.collected_event_outputs(authored.node_bundles);

  return {
      .graph_id = authored.identity.value,
      .graph = std::move(lowerer.graph),
      .scopes = std::move(scopes),
      .public_inputs = std::move(sample_inputs),
      .public_outputs = std::move(sample_outputs),
      .public_event_inputs = std::move(event_inputs),
      .public_event_outputs = std::move(event_outputs),
      .introspection = std::move(introspection),
  };
}

consteval CompiledGraph GraphCompiler::compile(ExecutableGraphIR executable) {
  details::sort_nodes_or_error(executable.graph, executable.graph_id);
  details::validate_graph(executable.graph, executable.public_inputs.size(),
                          executable.public_outputs.size());
  auto lowered_subgraphs = details::compile_lowered_subgraphs(
      executable.graph, executable.scopes);
  auto virtual_by_backing = details::build_virtual_node_ids_by_backing_node_id(
      executable.graph, executable.scopes);
  std::vector<DetachedInfo> detached;
  detached.reserve(executable.graph.detached_info_by_source.size());
  for (auto const& [_, info] : executable.graph.detached_info_by_source)
    detached.push_back(info);
  auto execution_plan = details::build_execution_plan(
      executable.graph.nodes, executable.graph.edges,
      executable.graph.event_edges, detached);
  auto dormancy_groups = details::compile_dormancy_groups(
      executable.graph, lowered_subgraphs, executable.graph_id, execution_plan);
  auto node_source_infos = std::move(executable.graph.node_source_infos);
  auto node_type_identities = std::move(executable.graph.node_type_identities);
  return {
      .graph = Graph(details::build_graph_artifact(
          executable.graph_id, std::move(executable.graph.nodes),
          std::move(executable.graph.explicit_ttl_samples),
          std::move(executable.graph.node_ids), std::move(executable.graph.edges),
          std::move(executable.graph.event_edges), std::move(detached),
          std::move(execution_plan), std::move(executable.public_inputs),
          std::move(executable.public_outputs),
          std::move(executable.public_event_inputs),
          std::move(executable.public_event_outputs), std::move(dormancy_groups))),
      .metadata = {
          .lowered_subgraphs = std::move(lowered_subgraphs),
          .concrete_node_type_identities = std::move(node_type_identities),
          .node_source_infos = std::move(node_source_infos),
          .virtual_node_ids_by_backing_node_id = std::move(virtual_by_backing),
      },
      .introspection = std::move(executable.introspection),
  };
}
} // namespace iv
