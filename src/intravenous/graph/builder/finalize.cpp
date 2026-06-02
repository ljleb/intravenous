#include "finalize.h"

#include "connections.h"
#include "detach.h"
#include "metadata.h"
#include "public_ports.h"
#include "topology.h"

#include <limits>

namespace iv {
namespace {
struct PreparedBuilderGraph {
    GraphBuilderIdentity const& identity;
    GraphBuilderTopology const& topology;
    GraphBuilderConnections const* connections;
    GraphBuilderDetach const* detach;
    details::PreparedGraph graph{
        .nodes = {},
        .explicit_ttl_samples = {},
        .node_ids = {},
        .node_logical_ids = {},
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
    std::unordered_map<PortId, PortId> source_of;
    std::unordered_map<PortId, GraphEventEdge> event_source_of;

    PreparedBuilderGraph(
        GraphBuilderIdentity const& identity_,
        GraphBuilderTopology const& topology_,
        GraphBuilderConnections const* connections_ = nullptr,
        GraphBuilderDetach const* detach_ = nullptr
    ) :
        identity(identity_),
        topology(topology_),
        connections(connections_),
        detach(detach_),
        runtime_node_indices(topology_.node_count(), GRAPH_ID)
    {
        topology.for_each_sample_edge([&](GraphEdge const& edge) {
            source_of[edge.target] = edge.source;
        });
        topology.for_each_event_edge([&](GraphEventEdge const& edge) {
            event_source_of[edge.target] = edge;
        });
    }

    void append_metadata_nodes()
    {
        for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
            auto const& node = topology.node(node_i);
            if (node.materialization.is_placeholder()) {
                continue;
            }
            runtime_node_indices[node_i] = graph.nodes.size();
            graph.nodes.push_back(TypeErasedNode(details::MetadataGraphNode{
                .input_configs = node.ports.sample_inputs,
                .output_configs = node.ports.sample_outputs,
                .event_input_configs = node.ports.event_inputs,
                .event_output_configs = node.ports.event_outputs,
            }));
            append_node_metadata(node_i, node, node.type_identity.value);
        }
    }

    void append_materialized_nodes(size_t detach_id_offset)
    {
        for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
            auto const& node = topology.node(node_i);
            if (node.materialization.is_placeholder()) {
                continue;
            }
            runtime_node_indices[node_i] = graph.nodes.size();
            graph.nodes.push_back(node.materialization.make(detach_id_offset));
            append_node_metadata(node_i, node, graph.nodes.back().type_name());
        }
    }

    void append_node_metadata(size_t node_i, BuilderNode const& node, std::string kind)
    {
        graph.explicit_ttl_samples.push_back(node.lifetime.ttl_samples);
        graph.node_ids.push_back(identity.child_id(node_i));
        graph.node_logical_ids.push_back(node.logical_binding.ids);
        graph.node_source_infos.push_back(node.source_annotations.infos);
        graph.node_construction_order.push_back(node_i);
        graph.node_kinds.push_back(std::move(kind));
        graph.node_type_identities.push_back(node.type_identity.value);
    }

    PortId resolve_sample_source(PortId source) const
    {
        if (source.node == GRAPH_ID) {
            return source;
        }
        auto const& node = topology.node(source.node);
        if (!node.materialization.is_placeholder()) {
            return PortId{ runtime_node_indices[source.node], source.port };
        }
        PortId const passthrough_source = node.lowered_subgraph.sample_output_sources[source.port];
        if (passthrough_source.node == source.node) {
            return resolve_sample_source(source_of.at(passthrough_source));
        }
        return resolve_sample_source(passthrough_source);
    }

    PortId resolve_event_source(PortId source) const
    {
        if (source.node == GRAPH_ID) {
            return source;
        }
        auto const& node = topology.node(source.node);
        if (!node.materialization.is_placeholder()) {
            return PortId{ runtime_node_indices[source.node], source.port };
        }
        PortId const passthrough_source = node.lowered_subgraph.event_output_sources[source.port];
        if (passthrough_source.node == source.node) {
            return resolve_event_source(event_source_of.at(passthrough_source).source);
        }
        return resolve_event_source(passthrough_source);
    }

    void add_sample_target_edges(PortId source, PortId target)
    {
        if (target.node == GRAPH_ID) {
            graph.edges.emplace(GraphEdge{ source, target });
            return;
        }

        auto const& target_node = topology.node(target.node);
        if (!target_node.materialization.is_placeholder()) {
            graph.edges.emplace(GraphEdge{ source, { runtime_node_indices[target.node], target.port } });
            return;
        }

        for (PortId const child_target : target_node.lowered_subgraph.sample_input_targets[target.port]) {
            add_sample_target_edges(source, child_target);
        }
    }

    void add_event_target_edges(GraphEventEdge edge, PortId target)
    {
        if (target.node == GRAPH_ID) {
            graph.event_edges.emplace(GraphEventEdge{ edge.source, target, std::move(edge.conversion) });
            return;
        }

        auto const& target_node = topology.node(target.node);
        if (!target_node.materialization.is_placeholder()) {
            graph.event_edges.emplace(GraphEventEdge{
                edge.source,
                { runtime_node_indices[target.node], target.port },
                std::move(edge.conversion)
            });
            return;
        }

        for (PortId const child_target : target_node.lowered_subgraph.event_input_targets[target.port]) {
            add_event_target_edges(GraphEventEdge{ edge.source, child_target, edge.conversion }, child_target);
        }
    }

    void lower_edges()
    {
        topology.for_each_sample_edge([&](GraphEdge const& edge) {
            add_sample_target_edges(resolve_sample_source(edge.source), edge.target);
        });
        topology.for_each_event_edge([&](GraphEventEdge const& edge) {
            GraphEventEdge resolved = edge;
            resolved.source = resolve_event_source(edge.source);
            add_event_target_edges(std::move(resolved), edge.target);
        });
    }

    void copy_runtime_filled_inputs()
    {
        IV_ASSERT(connections != nullptr, "copy_runtime_filled_inputs requires GraphBuilderConnections");
        connections->for_each_runtime_filled_sample_input([&](PortId port) {
            if (port.node == GRAPH_ID || topology.node(port.node).materialization.is_placeholder()) {
                return;
            }
            graph.timeline_filled_input_ports.insert(PortId{ runtime_node_indices[port.node], port.port });
        });
        connections->for_each_runtime_filled_event_input([&](PortId port) {
            if (port.node == GRAPH_ID || topology.node(port.node).materialization.is_placeholder()) {
                return;
            }
            graph.timeline_filled_event_input_ports.insert(PortId{ runtime_node_indices[port.node], port.port });
        });
    }

    PortId materialize_placeholder_default(size_t placeholder_node, size_t input_port)
    {
        runtime_node_indices.push_back(graph.nodes.size());
        graph.nodes.push_back(TypeErasedNode(
            Constant(topology.node(placeholder_node).inputs()[input_port].default_value)
        ));
        graph.explicit_ttl_samples.push_back(std::nullopt);
        graph.node_ids.push_back(identity.child_id(placeholder_node) + ".default." + std::to_string(input_port));
        graph.node_logical_ids.emplace_back();
        graph.node_source_infos.emplace_back();
        graph.node_construction_order.push_back(placeholder_node);
        return PortId{ graph.nodes.size() - 1, 0 };
    }

    void add_placeholder_default_edges()
    {
        for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
            auto const& node = topology.node(node_i);
            if (!node.materialization.is_placeholder()) {
                continue;
            }
            for (size_t input_port = 0; input_port < node.inputs().size(); ++input_port) {
                PortId const placeholder_input{ node_i, input_port };
                if (source_of.contains(placeholder_input)) {
                    continue;
                }
                PortId const default_source = materialize_placeholder_default(node_i, input_port);
                for (PortId const child_target : node.lowered_subgraph.sample_input_targets[input_port]) {
                    add_sample_target_edges(default_source, child_target);
                }
            }
        }
    }

    void copy_detach_info()
    {
        IV_ASSERT(detach != nullptr, "copy_detach_info requires GraphBuilderDetach");
        detach->for_each_info([&](PortId source, DetachedSamplePortInfo const& info) {
            PortId const remapped_source = resolve_sample_source(source);
            graph.detached_info_by_source.emplace(remapped_source, DetachedSamplePortInfo{
                .detach_id = info.detach_id,
                .original_source = remapped_source,
                .writer_node = runtime_node_indices[info.writer_node],
                .reader_output = resolve_sample_source(info.reader_output),
                .loop_extra_latency = info.loop_extra_latency,
            });
        });
        detach->for_each_reader_output([&](PortId reader_output) {
            graph.detached_reader_outputs.insert(resolve_sample_source(reader_output));
        });
    }

    iv::LoweredSubgraphSpec::PortRef make_scope_port_ref(PortId port) const
    {
        return iv::LoweredSubgraphSpec::PortRef{
            .node_id = port.node == GRAPH_ID ? std::string{} : identity.child_id(port.node),
            .port = port.port,
            .is_graph_port = port.node == GRAPH_ID,
        };
    }

    void collect_scope_targets(PortId target, std::vector<iv::LoweredSubgraphSpec::PortRef>& out) const
    {
        if (target.node == GRAPH_ID || !topology.node(target.node).materialization.is_placeholder()) {
            out.push_back(make_scope_port_ref(target));
            return;
        }
        for (PortId const child_target : topology.node(target.node).lowered_subgraph.sample_input_targets[target.port]) {
            collect_scope_targets(child_target, out);
        }
    }

    void collect_scope_event_targets(PortId target, std::vector<iv::LoweredSubgraphSpec::PortRef>& out) const
    {
        if (target.node == GRAPH_ID || !topology.node(target.node).materialization.is_placeholder()) {
            out.push_back(make_scope_port_ref(target));
            return;
        }
        for (PortId const child_target : topology.node(target.node).lowered_subgraph.event_input_targets[target.port]) {
            collect_scope_event_targets(child_target, out);
        }
    }

    std::vector<iv::LoweredSubgraphSpec> build_lowered_scopes() const
    {
        std::vector<size_t> placeholder_indices;
        for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
            if (topology.node(node_i).materialization.is_placeholder()) {
                placeholder_indices.push_back(node_i);
            }
        }

        auto encloses = [&](size_t parent, size_t child) {
            auto const& parent_node = topology.node(parent);
            auto const& child_node = topology.node(child);
            size_t const parent_begin = parent_node.lowered_subgraph.begin;
            size_t const parent_end = parent_begin + parent_node.lowered_subgraph.count;
            size_t const child_begin = child_node.lowered_subgraph.begin;
            size_t const child_end = child_begin + child_node.lowered_subgraph.count;
            return parent_begin <= child_begin && child_end <= parent_end;
        };

        std::unordered_map<size_t, size_t> scope_index_by_placeholder;
        std::vector<iv::LoweredSubgraphSpec> scopes;

        for (size_t placeholder_i : placeholder_indices) {
            auto const& placeholder = topology.node(placeholder_i);
            iv::LoweredSubgraphSpec scope;
            scope.kind = placeholder.lowered_subgraph.kind;
            scope.backing_node_id = identity.child_id(placeholder_i);
            for (auto const& info : placeholder.source_annotations.infos) {
                scope.source_infos.push_back(info);
                if (info.span.file_path.empty() || info.span.begin > info.span.end) {
                    continue;
                }
                if (std::find(scope.source_spans.begin(), scope.source_spans.end(), info.span) == scope.source_spans.end()) {
                    scope.source_spans.push_back(info.span);
                }
            }
            scope.sample_inputs = placeholder.ports.sample_inputs;
            scope.sample_outputs = placeholder.ports.sample_outputs;
            scope.event_inputs = placeholder.ports.event_inputs;
            scope.event_outputs = placeholder.ports.event_outputs;
            scope.ttl_samples = placeholder.lifetime.ttl_samples;

            size_t const begin = placeholder.lowered_subgraph.begin;
            size_t const end = begin + placeholder.lowered_subgraph.count;
            for (size_t node_i = begin; node_i < end; ++node_i) {
                if (topology.node(node_i).materialization.is_placeholder()) {
                    continue;
                }
                scope.member_node_ids.push_back(identity.child_id(node_i));
            }

            scope.sample_input_targets.reserve(placeholder.lowered_subgraph.sample_input_targets.size());
            for (auto const& targets : placeholder.lowered_subgraph.sample_input_targets) {
                std::vector<iv::LoweredSubgraphSpec::PortRef> flattened_targets;
                for (PortId const target : targets) {
                    collect_scope_targets(target, flattened_targets);
                }
                scope.sample_input_targets.push_back(std::move(flattened_targets));
            }

            scope.sample_output_sources.reserve(placeholder.lowered_subgraph.sample_output_sources.size());
            for (PortId const source : placeholder.lowered_subgraph.sample_output_sources) {
                scope.sample_output_sources.push_back(make_scope_port_ref(resolve_sample_source(source)));
            }

            scope.event_input_targets.reserve(placeholder.lowered_subgraph.event_input_targets.size());
            for (auto const& targets : placeholder.lowered_subgraph.event_input_targets) {
                std::vector<iv::LoweredSubgraphSpec::PortRef> flattened_targets;
                for (PortId const target : targets) {
                    collect_scope_event_targets(target, flattened_targets);
                }
                scope.event_input_targets.push_back(std::move(flattened_targets));
            }

            scope.event_output_sources.reserve(placeholder.lowered_subgraph.event_output_sources.size());
            for (PortId const source : placeholder.lowered_subgraph.event_output_sources) {
                scope.event_output_sources.push_back(make_scope_port_ref(resolve_event_source(source)));
            }

            if (scope.member_node_ids.empty()) {
                continue;
            }

            std::sort(scope.member_node_ids.begin(), scope.member_node_ids.end());
            scope.member_node_ids.erase(
                std::unique(scope.member_node_ids.begin(), scope.member_node_ids.end()),
                scope.member_node_ids.end()
            );
            scope_index_by_placeholder.emplace(placeholder_i, scopes.size());
            scopes.push_back(std::move(scope));
        }

        for (size_t placeholder_i : placeholder_indices) {
            auto scope_it = scope_index_by_placeholder.find(placeholder_i);
            if (scope_it == scope_index_by_placeholder.end()) {
                continue;
            }
            size_t const scope_i = scope_it->second;

            size_t best_parent_placeholder = GRAPH_ID;
            size_t best_parent_span = std::numeric_limits<size_t>::max();
            for (size_t other_placeholder : placeholder_indices) {
                if (other_placeholder == placeholder_i || !encloses(other_placeholder, placeholder_i)) {
                    continue;
                }
                size_t const span = topology.node(other_placeholder).lowered_subgraph.count;
                if (span < best_parent_span && scope_index_by_placeholder.contains(other_placeholder)) {
                    best_parent_span = span;
                    best_parent_placeholder = other_placeholder;
                }
            }
            if (best_parent_placeholder != GRAPH_ID) {
                scopes[scope_i].parent_scope = scope_index_by_placeholder.at(best_parent_placeholder);
            }
        }

        return scopes;
    }
};
} // namespace

GraphIntrospectionMetadata GraphBuilderFinalizer::build_metadata(
    GraphBuilderIdentity const& identity,
    GraphBuilderTopology const& topology,
    size_t detach_id_offset
)
{
    (void)detach_id_offset;
    PreparedBuilderGraph prepared(identity, topology);
    prepared.append_metadata_nodes();
    prepared.lower_edges();

    auto lowered_scopes = prepared.build_lowered_scopes();
    auto [logical_nodes, _] = details::build_logical_metadata(prepared.graph, lowered_scopes);
    return GraphIntrospectionMetadata{ .logical_nodes = std::move(logical_nodes) };
}

GraphBuilderRootNodeBuildResult GraphBuilderFinalizer::build_root_node(
    GraphBuilderIdentity const& identity,
    GraphBuilderTopology const& topology,
    GraphBuilderConnections const& connections,
    GraphBuilderPublicPorts const& public_ports,
    GraphBuilderDetach const& detach,
    size_t detach_id_offset
)
{
    if (!public_ports.sample_outputs_defined()) {
        details::error("builder " + identity.value + ": g.outputs(...) must be called before build()");
    }

    PreparedBuilderGraph prepared(identity, topology, &connections, &detach);
    prepared.append_materialized_nodes(detach_id_offset);
    prepared.copy_runtime_filled_inputs();
    prepared.lower_edges();
    prepared.add_placeholder_default_edges();
    prepared.copy_detach_info();

    details::expand_hyperedge_ports(prepared.graph, identity.value);
    details::stub_dangling_ports(prepared.graph, public_ports.sample_inputs().size(), identity.value);
    details::validate_graph(
        prepared.graph,
        public_ports.sample_inputs().size(),
        public_ports.sample_outputs().size()
    );
    details::validate_detached_edges(prepared.graph, identity.value);
    details::sort_nodes_or_error(prepared.graph, identity.value);
    details::validate_graph(
        prepared.graph,
        public_ports.sample_inputs().size(),
        public_ports.sample_outputs().size()
    );

    auto lowered_scopes = prepared.build_lowered_scopes();
    auto lowered_subgraphs = details::compile_lowered_subgraphs(prepared.graph, lowered_scopes);
    auto [_, logical_node_ids_by_backing_node_id] = details::build_logical_metadata(prepared.graph, lowered_scopes);

    auto detached = [&] {
        std::vector<DetachedInfo> detached_info;
        detached_info.reserve(prepared.graph.detached_info_by_source.size());
        for (auto const& [_, info] : prepared.graph.detached_info_by_source) {
            detached_info.push_back(info);
        }
        return detached_info;
    }();
        auto execution_plan = details::build_execution_plan(
            prepared.graph.nodes,
            prepared.graph.edges,
        prepared.graph.event_edges,
        detached
    );
        auto dormancy_groups = details::compile_dormancy_groups(
            prepared.graph,
            lowered_subgraphs,
            identity.value,
            execution_plan
        );

        auto node_source_infos = std::move(prepared.graph.node_source_infos);
        return GraphBuilderRootNodeBuildResult{
            .graph = Graph(details::build_graph_artifact(
                identity.value,
                std::move(prepared.graph.nodes),
                std::move(prepared.graph.explicit_ttl_samples),
                std::move(prepared.graph.node_ids),
                std::move(prepared.graph.edges),
                std::move(prepared.graph.event_edges),
                std::move(detached),
                std::move(execution_plan),
                std::vector<InputConfig>(public_ports.sample_inputs().begin(), public_ports.sample_inputs().end()),
                std::vector<OutputConfig>(public_ports.sample_outputs().begin(), public_ports.sample_outputs().end()),
                std::vector<EventInputConfig>(public_ports.event_inputs().begin(), public_ports.event_inputs().end()),
                std::vector<EventOutputConfig>(public_ports.event_outputs().begin(), public_ports.event_outputs().end()),
                std::move(dormancy_groups)
            )),
            .metadata = GraphBuildMetadata{
                .lowered_subgraphs = std::move(lowered_subgraphs),
                .node_source_infos = std::move(node_source_infos),
                .logical_node_ids_by_backing_node_id = std::move(logical_node_ids_by_backing_node_id),
            },
        };
}

} // namespace iv
