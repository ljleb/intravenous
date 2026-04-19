#pragma once

#include "graph/builder.h"
#include "runtime/timeline.h"

namespace iv {
    class GraphPlan {
        std::string _builder_id;
        std::vector<BuilderNode> _nodes;
        std::unordered_set<GraphEdge> _edges;
        std::unordered_set<GraphEventEdge> _event_edges;
        std::vector<InputConfig> _public_inputs;
        std::vector<EventInputConfig> _public_event_inputs;
        std::vector<OutputConfig> _public_outputs;
        std::vector<EventOutputConfig> _public_event_outputs;
        std::unordered_map<PortId, DetachedSamplePortInfo> _detached_info_by_source;
        std::unordered_set<PortId> _detached_reader_outputs;
        bool _outputs_defined = false;

        std::string node_id(size_t index) const
        {
            IV_ASSERT(index < _nodes.size(), "node index out of bounds");
            std::string nested_path = _builder_id;
            if (!nested_path.empty()) {
                nested_path += ".";
            }
            nested_path += std::to_string(index);
            return nested_path;
        }

    public:
        explicit GraphPlan(GraphBuilder const& builder) :
            _builder_id(builder._builder_id.value),
            _nodes(builder._nodes),
            _edges(builder._edges),
            _event_edges(builder._event_edges),
            _public_inputs(builder._public_inputs),
            _public_event_inputs(builder._public_event_inputs),
            _public_outputs(builder._public_outputs),
            _public_event_outputs(builder._public_event_outputs),
            _detached_info_by_source(builder._detached_info_by_source),
            _detached_reader_outputs(builder._detached_reader_outputs),
            _outputs_defined(builder._outputs_defined)
        {}

        GraphPlan& fill_vacant_logical_inputs([[maybe_unused]] Timeline& timeline)
        {
            // Timeline-backed vacant logical input filling is intentionally
            // staged here rather than on GraphBuilder, but policy remains
            // unimplemented until logical identity solving is moved earlier.
            return *this;
        }

        GraphBuilder::BuildResult build_with_metadata(size_t detach_id_offset = 0) const
        {
            if (!_outputs_defined) {
                details::error("builder " + _builder_id + ": g.outputs(...) must be called before build()");
            }

            details::PreparedGraph g{
                .nodes = {},
                .explicit_ttl_samples = {},
                .node_ids = {},
                .node_source_infos = {},
                .edges = {},
                .event_edges = {},
                .detached_info_by_source = {},
                .detached_reader_outputs = {},
            };
            std::vector<size_t> runtime_node_indices(_nodes.size(), GRAPH_ID);
            std::unordered_map<PortId, PortId> source_of;
            std::unordered_map<PortId, GraphEventEdge> event_source_of;
            for (GraphEdge const& edge : _edges) {
                source_of[edge.target] = edge.source;
            }
            for (GraphEventEdge const& edge : _event_edges) {
                event_source_of[edge.target] = edge;
            }

            for (size_t node_i = 0; node_i < _nodes.size(); ++node_i) {
                if (!_nodes[node_i].materialize) {
                    continue;
                }
                runtime_node_indices[node_i] = g.nodes.size();
                g.nodes.push_back(_nodes[node_i].materialize(detach_id_offset));
                g.explicit_ttl_samples.push_back(_nodes[node_i].ttl_samples);
                g.node_ids.push_back(node_id(node_i));
                g.node_source_infos.push_back(_nodes[node_i].source_infos);
            }

            auto materialize_placeholder_default = [&](size_t placeholder_node, size_t input_port) {
                runtime_node_indices.push_back(g.nodes.size());
                g.nodes.push_back(TypeErasedNode(Constant(_nodes[placeholder_node].input_configs[input_port].default_value)));
                g.explicit_ttl_samples.push_back(std::nullopt);
                g.node_ids.push_back(
                    node_id(placeholder_node) + ".default." + std::to_string(input_port)
                );
                g.node_source_infos.emplace_back();
                return PortId{ g.nodes.size() - 1, 0 };
            };

            auto resolve_source = [&](auto const& self, PortId source) -> PortId {
                if (source.node == GRAPH_ID) {
                    return source;
                }
                if (_nodes[source.node].materialize) {
                    return PortId{ runtime_node_indices[source.node], source.port };
                }
                PortId const passthrough_source = _nodes[source.node].subgraph_output_sources[source.port];
                if (passthrough_source.node == source.node) {
                    return self(self, source_of.at(passthrough_source));
                }
                return self(self, passthrough_source);
            };

            auto resolve_event_source = [&](auto const& self, PortId source) -> PortId {
                if (source.node == GRAPH_ID) {
                    return source;
                }
                if (_nodes[source.node].materialize) {
                    return PortId{ runtime_node_indices[source.node], source.port };
                }
                PortId const passthrough_source = _nodes[source.node].subgraph_event_output_sources[source.port];
                if (passthrough_source.node == source.node) {
                    return self(self, event_source_of.at(passthrough_source).source);
                }
                return self(self, passthrough_source);
            };

            auto add_target_edges = [&](auto const& self, PortId source, PortId target) -> void {
                if (target.node == GRAPH_ID) {
                    g.edges.emplace(GraphEdge{ source, target });
                    return;
                }
                if (_nodes[target.node].materialize) {
                    g.edges.emplace(GraphEdge{ source, { runtime_node_indices[target.node], target.port } });
                    return;
                }
                for (PortId const child_target : _nodes[target.node].subgraph_input_targets[target.port]) {
                    self(self, source, child_target);
                }
            };

            auto add_event_target_edges = [&](auto const& self, GraphEventEdge edge, PortId target) -> void {
                if (target.node == GRAPH_ID) {
                    g.event_edges.emplace(GraphEventEdge{ edge.source, target, std::move(edge.conversion) });
                    return;
                }
                if (_nodes[target.node].materialize) {
                    g.event_edges.emplace(GraphEventEdge{
                        edge.source,
                        { runtime_node_indices[target.node], target.port },
                        std::move(edge.conversion)
                    });
                    return;
                }
                for (PortId const child_target : _nodes[target.node].subgraph_event_input_targets[target.port]) {
                    self(self, GraphEventEdge{ edge.source, child_target, edge.conversion }, child_target);
                }
            };

            for (GraphEdge const& edge : _edges) {
                add_target_edges(add_target_edges, resolve_source(resolve_source, edge.source), edge.target);
            }
            for (GraphEventEdge const& edge : _event_edges) {
                GraphEventEdge resolved = edge;
                resolved.source = resolve_event_source(resolve_event_source, edge.source);
                add_event_target_edges(add_event_target_edges, std::move(resolved), edge.target);
            }

            for (size_t node_i = 0; node_i < _nodes.size(); ++node_i) {
                if (_nodes[node_i].materialize) {
                    continue;
                }

                for (size_t input_port = 0; input_port < _nodes[node_i].input_configs.size(); ++input_port) {
                    PortId const placeholder_input{ node_i, input_port };
                    if (source_of.contains(placeholder_input)) {
                        continue;
                    }

                    auto const default_source = materialize_placeholder_default(node_i, input_port);
                    for (PortId const child_target : _nodes[node_i].subgraph_input_targets[input_port]) {
                        add_target_edges(add_target_edges, default_source, child_target);
                    }
                }
            }

            for (auto const& [source, info] : _detached_info_by_source) {
                PortId const remapped_source = resolve_source(resolve_source, source);
                g.detached_info_by_source.emplace(remapped_source, DetachedSamplePortInfo{
                    .detach_id = info.detach_id,
                    .original_source = remapped_source,
                    .writer_node = runtime_node_indices[info.writer_node],
                    .reader_output = resolve_source(resolve_source, info.reader_output),
                    .loop_extra_latency = info.loop_extra_latency,
                });
            }

            for (PortId const reader_output : _detached_reader_outputs) {
                g.detached_reader_outputs.insert(resolve_source(resolve_source, reader_output));
            }

            details::expand_hyperedge_ports(g, _builder_id);
            details::stub_dangling_ports(g, _public_inputs.size(), _builder_id);
            details::validate_graph(g, _public_inputs.size(), _public_outputs.size());
            details::validate_detached_edges(g, _builder_id);
            details::sort_nodes_or_error(g, _builder_id);
            details::validate_graph(g, _public_inputs.size(), _public_outputs.size());

            auto lowered_scopes = [&] {
                auto make_scope_port_ref = [&](PortId port) {
                    return iv::LoweredSubgraphSpec::PortRef {
                        .node_id = port.node == GRAPH_ID ? std::string{} : node_id(port.node),
                        .port = port.port,
                        .is_graph_port = port.node == GRAPH_ID,
                    };
                };
                auto resolve_scope_source = [&](auto const& self, PortId source) -> PortId {
                    if (source.node == GRAPH_ID || _nodes[source.node].materialize) {
                        return source;
                    }
                    PortId const passthrough_source = _nodes[source.node].subgraph_output_sources[source.port];
                    if (passthrough_source.node == source.node) {
                        return self(self, source_of.at(passthrough_source));
                    }
                    return self(self, passthrough_source);
                };
                auto resolve_scope_event_source = [&](auto const& self, PortId source) -> PortId {
                    if (source.node == GRAPH_ID || _nodes[source.node].materialize) {
                        return source;
                    }
                    PortId const passthrough_source = _nodes[source.node].subgraph_event_output_sources[source.port];
                    if (passthrough_source.node == source.node) {
                        return self(self, event_source_of.at(passthrough_source).source);
                    }
                    return self(self, passthrough_source);
                };
                auto collect_scope_targets = [&](auto const& self, PortId target, std::vector<iv::LoweredSubgraphSpec::PortRef>& out) -> void {
                    if (target.node == GRAPH_ID || _nodes[target.node].materialize) {
                        out.push_back(make_scope_port_ref(target));
                        return;
                    }
                    for (PortId const child_target : _nodes[target.node].subgraph_input_targets[target.port]) {
                        self(self, child_target, out);
                    }
                };
                auto collect_scope_event_targets = [&](auto const& self, PortId target, std::vector<iv::LoweredSubgraphSpec::PortRef>& out) -> void {
                    if (target.node == GRAPH_ID || _nodes[target.node].materialize) {
                        out.push_back(make_scope_port_ref(target));
                        return;
                    }
                    for (PortId const child_target : _nodes[target.node].subgraph_event_input_targets[target.port]) {
                        self(self, child_target, out);
                    }
                };

                std::vector<size_t> placeholder_indices;
                for (size_t node_i = 0; node_i < _nodes.size(); ++node_i) {
                    if (!_nodes[node_i].materialize) {
                        placeholder_indices.push_back(node_i);
                    }
                }

                auto encloses = [&](size_t parent, size_t child) {
                    auto const& parent_node = _nodes[parent];
                    auto const& child_node = _nodes[child];
                    size_t const parent_begin = parent_node.lowered_subgraph_begin;
                    size_t const parent_end = parent_begin + parent_node.lowered_subgraph_count;
                    size_t const child_begin = child_node.lowered_subgraph_begin;
                    size_t const child_end = child_begin + child_node.lowered_subgraph_count;
                    return parent_begin <= child_begin && child_end <= parent_end;
                };

                std::unordered_map<size_t, size_t> scope_index_by_placeholder;
                std::vector<iv::LoweredSubgraphSpec> scopes;

                for (size_t placeholder_i : placeholder_indices) {
                    auto const& placeholder = _nodes[placeholder_i];
                    iv::LoweredSubgraphSpec scope;
                    scope.kind = placeholder.subgraph_kind;
                    for (auto const& info : placeholder.source_infos) {
                        if (info.span.file_path.empty() || info.span.begin > info.span.end) {
                            continue;
                        }
                        if (std::find(scope.source_spans.begin(), scope.source_spans.end(), info.span) == scope.source_spans.end()) {
                            scope.source_spans.push_back(info.span);
                        }
                    }
                    scope.sample_inputs = placeholder.input_configs;
                    scope.sample_outputs = placeholder.output_configs;
                    scope.event_inputs = placeholder.event_input_configs;
                    scope.event_outputs = placeholder.event_output_configs;
                    scope.ttl_samples = placeholder.ttl_samples;

                    size_t const begin = placeholder.lowered_subgraph_begin;
                    size_t const end = begin + placeholder.lowered_subgraph_count;
                    for (size_t node_i = begin; node_i < end; ++node_i) {
                        if (!_nodes[node_i].materialize) {
                            continue;
                        }
                        scope.member_node_ids.push_back(node_id(node_i));
                    }

                    scope.sample_input_targets.reserve(placeholder.subgraph_input_targets.size());
                    for (auto const& targets : placeholder.subgraph_input_targets) {
                        std::vector<iv::LoweredSubgraphSpec::PortRef> flattened_targets;
                        for (PortId const target : targets) {
                            collect_scope_targets(collect_scope_targets, target, flattened_targets);
                        }
                        scope.sample_input_targets.push_back(std::move(flattened_targets));
                    }
                    scope.sample_output_sources.reserve(placeholder.subgraph_output_sources.size());
                    for (PortId const source : placeholder.subgraph_output_sources) {
                        scope.sample_output_sources.push_back(make_scope_port_ref(
                            resolve_scope_source(resolve_scope_source, source)
                        ));
                    }
                    scope.event_input_targets.reserve(placeholder.subgraph_event_input_targets.size());
                    for (auto const& targets : placeholder.subgraph_event_input_targets) {
                        std::vector<iv::LoweredSubgraphSpec::PortRef> flattened_targets;
                        for (PortId const target : targets) {
                            collect_scope_event_targets(collect_scope_event_targets, target, flattened_targets);
                        }
                        scope.event_input_targets.push_back(std::move(flattened_targets));
                    }
                    scope.event_output_sources.reserve(placeholder.subgraph_event_output_sources.size());
                    for (PortId const source : placeholder.subgraph_event_output_sources) {
                        scope.event_output_sources.push_back(make_scope_port_ref(
                            resolve_scope_event_source(resolve_scope_event_source, source)
                        ));
                    }

                    if (scope.member_node_ids.empty()) {
                        continue;
                    }

                    std::sort(scope.member_node_ids.begin(), scope.member_node_ids.end());
                    scope.member_node_ids.erase(std::unique(scope.member_node_ids.begin(), scope.member_node_ids.end()), scope.member_node_ids.end());
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
                        size_t const span = _nodes[other_placeholder].lowered_subgraph_count;
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
            }();

            auto lowered_subgraphs = details::compile_lowered_subgraphs(g, lowered_scopes);

            auto detached = [&] {
                std::vector<DetachedInfo> detached_info;
                detached_info.reserve(g.detached_info_by_source.size());
                for (auto const& [_, info] : g.detached_info_by_source) {
                    detached_info.push_back(info);
                }
                return detached_info;
            }();
            auto execution_plan = details::build_execution_plan(g.nodes, g.edges, g.event_edges, detached);
            auto [logical_nodes, logical_node_ids_by_backing_node_id] = details::build_logical_metadata(g);

            GraphIntrospectionMetadata introspection {
                .lowered_subgraphs = std::move(lowered_subgraphs),
                .node_source_infos = std::move(g.node_source_infos),
                .logical_nodes = std::move(logical_nodes),
                .logical_node_ids_by_backing_node_id = std::move(logical_node_ids_by_backing_node_id),
            };
            auto dormancy_groups = details::compile_dormancy_groups(
                g,
                introspection.lowered_subgraphs,
                _builder_id,
                execution_plan
            );

            return GraphBuilder::BuildResult {
                .graph = Graph(details::build_graph_artifact(
                    _builder_id,
                    std::move(g.nodes),
                    std::move(g.explicit_ttl_samples),
                    std::move(g.node_ids),
                    std::move(g.edges),
                    std::move(g.event_edges),
                    std::move(detached),
                    std::move(execution_plan),
                    _public_inputs,
                    _public_outputs,
                    _public_event_inputs,
                    _public_event_outputs,
                    std::move(dormancy_groups)
                )),
                .introspection = std::move(introspection),
            };
        }

        Graph build(size_t detach_id_offset = 0) const
        {
            return build_with_metadata(detach_id_offset).graph;
        }
    };

    inline GraphPlan GraphBuilder::plan() const
    {
        return GraphPlan(*this);
    }
}
