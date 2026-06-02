#include "metadata.h"

namespace iv::details {

LogicalPortConnectivity aggregate_connectivity(std::span<LogicalConcretePortInfo const> ports)
{
    bool any_connected = false;
    bool any_disconnected = false;
    for (auto const& port : ports) {
        any_connected = any_connected || port.connected;
        any_disconnected = any_disconnected || !port.connected;
    }
    if (any_connected && any_disconnected) {
        return LogicalPortConnectivity::mixed;
    }
    return any_connected
        ? LogicalPortConnectivity::connected
        : LogicalPortConnectivity::disconnected;
}

void sort_and_deduplicate_spans(std::vector<SourceSpan>& spans)
{
    std::sort(spans.begin(), spans.end(), [](auto const& a, auto const& b) {
        return std::tie(a.file_path, a.begin, a.end) < std::tie(b.file_path, b.begin, b.end);
    });
    spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
}

std::vector<SourceSpan> source_spans_for(
    std::span<LogicalConcreteNode const* const> nodes,
    std::string_view source_identity
)
{
    std::vector<SourceSpan> spans;
    for (auto const* node : nodes) {
        for (auto const& info : node->source_infos) {
            if (info.declaration_identity != source_identity) {
                continue;
            }
            if (info.span.file_path.empty() || info.span.begin > info.span.end) {
                continue;
            }
            spans.push_back(info.span);
        }
    }
    sort_and_deduplicate_spans(spans);
    return spans;
}

LogicalMetadataBuildResult build_logical_metadata(
    PreparedGraph const& g,
    std::span<LoweredSubgraphSpec const> lowered_scopes
)
{
    auto sample_input_connected = [&](size_t node, size_t port) {
        return std::ranges::any_of(g.edges, [&](GraphEdge const& edge) {
            return edge.target.node == node
                && edge.target.port == port
                && !g.timeline_filled_input_ports.contains(edge.target);
        });
    };
    auto sample_output_connected = [&](size_t node, size_t port) {
        return std::ranges::any_of(g.edges, [&](GraphEdge const& edge) {
            return edge.source.node == node && edge.source.port == port;
        });
    };
    auto event_input_connected = [&](size_t node, size_t port) {
        return std::ranges::any_of(g.event_edges, [&](GraphEventEdge const& edge) {
            return edge.target.node == node
                && edge.target.port == port
                && !g.timeline_filled_event_input_ports.contains(edge.target);
        });
    };
    auto event_output_connected = [&](size_t node, size_t port) {
        return std::ranges::any_of(g.event_edges, [&](GraphEventEdge const& edge) {
            return edge.source.node == node && edge.source.port == port;
        });
    };

    std::vector<LogicalConcreteNode> concrete_nodes;
    std::vector<std::vector<std::string>> concrete_node_logical_ids;
    concrete_nodes.reserve(g.nodes.size());
    concrete_node_logical_ids.reserve(g.nodes.size() + lowered_scopes.size());
    for (size_t node_i = 0; node_i < g.nodes.size(); ++node_i) {
        LogicalConcreteNode concrete;
        concrete.id = g.node_ids[node_i];
        concrete.kind = node_i < g.node_kinds.size()
            ? g.node_kinds[node_i]
            : demangle_type_name(g.nodes[node_i].type_name());
        concrete.construction_order = node_i < g.node_construction_order.size()
            ? g.node_construction_order[node_i]
            : node_i;
        concrete.type_identity = node_i < g.node_type_identities.size()
            ? g.node_type_identities[node_i]
            : concrete.kind;
        if (node_i < g.node_source_infos.size()) {
            concrete.source_infos = g.node_source_infos[node_i];
        }

        auto const inputs = g.nodes[node_i].inputs();
        concrete.sample_inputs.reserve(inputs.size());
        for (size_t input_i = 0; input_i < inputs.size(); ++input_i) {
            concrete.sample_inputs.push_back(LogicalConcretePortInfo {
                .name = inputs[input_i].name,
                .type = "sample",
                .connected = sample_input_connected(node_i, input_i),
                .history = inputs[input_i].history,
                .default_value = inputs[input_i].default_value,
            });
        }

        auto const outputs = g.nodes[node_i].outputs();
        concrete.sample_outputs.reserve(outputs.size());
        for (size_t output_i = 0; output_i < outputs.size(); ++output_i) {
            concrete.sample_outputs.push_back(LogicalConcretePortInfo {
                .name = outputs[output_i].name,
                .type = "sample",
                .connected = sample_output_connected(node_i, output_i),
                .history = outputs[output_i].history,
                .latency = outputs[output_i].latency,
            });
        }

        auto const event_inputs = g.nodes[node_i].event_inputs();
        concrete.event_inputs.reserve(event_inputs.size());
        for (size_t input_i = 0; input_i < event_inputs.size(); ++input_i) {
            concrete.event_inputs.push_back(LogicalConcretePortInfo {
                .name = event_inputs[input_i].name,
                .type = event_type_name(event_inputs[input_i].type),
                .connected = event_input_connected(node_i, input_i),
            });
        }

        auto const event_outputs = g.nodes[node_i].event_outputs();
        concrete.event_outputs.reserve(event_outputs.size());
        for (size_t output_i = 0; output_i < event_outputs.size(); ++output_i) {
            concrete.event_outputs.push_back(LogicalConcretePortInfo {
                .name = event_outputs[output_i].name,
                .type = event_type_name(event_outputs[output_i].type),
                .connected = event_output_connected(node_i, output_i),
            });
        }

        concrete_nodes.push_back(std::move(concrete));
        concrete_node_logical_ids.push_back(
            node_i < g.node_logical_ids.size()
                ? g.node_logical_ids[node_i]
                : std::vector<std::string> {}
        );
    }

    for (size_t scope_i = 0; scope_i < lowered_scopes.size(); ++scope_i) {
        auto const& scope = lowered_scopes[scope_i];
        LogicalConcreteNode concrete;
        concrete.id = scope.backing_node_id;
        concrete.kind = scope.kind;
        concrete.construction_order = g.nodes.size() + scope_i;
        concrete.type_identity = "lowered-subgraph:" + scope.kind;
        concrete.source_infos = scope.source_infos;

        concrete.sample_inputs.reserve(scope.sample_inputs.size());
        for (size_t input_i = 0; input_i < scope.sample_inputs.size(); ++input_i) {
            concrete.sample_inputs.push_back(LogicalConcretePortInfo {
                .name = scope.sample_inputs[input_i].name,
                .type = "sample",
                .connected = input_i < scope.sample_input_targets.size() && !scope.sample_input_targets[input_i].empty(),
                .history = scope.sample_inputs[input_i].history,
                .default_value = scope.sample_inputs[input_i].default_value,
            });
        }

        concrete.sample_outputs.reserve(scope.sample_outputs.size());
        for (size_t output_i = 0; output_i < scope.sample_outputs.size(); ++output_i) {
            concrete.sample_outputs.push_back(LogicalConcretePortInfo {
                .name = scope.sample_outputs[output_i].name,
                .type = "sample",
                .connected = output_i < scope.sample_output_sources.size() &&
                    (scope.sample_output_sources[output_i].is_graph_port ||
                     !scope.sample_output_sources[output_i].node_id.empty()),
                .history = scope.sample_outputs[output_i].history,
                .latency = scope.sample_outputs[output_i].latency,
            });
        }

        concrete.event_inputs.reserve(scope.event_inputs.size());
        for (size_t input_i = 0; input_i < scope.event_inputs.size(); ++input_i) {
            concrete.event_inputs.push_back(LogicalConcretePortInfo {
                .name = scope.event_inputs[input_i].name,
                .type = event_type_name(scope.event_inputs[input_i].type),
                .connected = input_i < scope.event_input_targets.size() && !scope.event_input_targets[input_i].empty(),
            });
        }

        concrete.event_outputs.reserve(scope.event_outputs.size());
        for (size_t output_i = 0; output_i < scope.event_outputs.size(); ++output_i) {
            concrete.event_outputs.push_back(LogicalConcretePortInfo {
                .name = scope.event_outputs[output_i].name,
                .type = event_type_name(scope.event_outputs[output_i].type),
                .connected = output_i < scope.event_output_sources.size() &&
                    (scope.event_output_sources[output_i].is_graph_port ||
                     !scope.event_output_sources[output_i].node_id.empty()),
            });
        }

        concrete_nodes.push_back(std::move(concrete));

        std::vector<std::string> logical_node_ids;
        for (auto const& info : scope.source_infos) {
            if (info.declaration_identity.empty()) {
                continue;
            }
            if (!std::ranges::contains(logical_node_ids, info.declaration_identity)) {
                logical_node_ids.push_back(info.declaration_identity);
            }
        }
        concrete_node_logical_ids.push_back(std::move(logical_node_ids));
    }

    struct LogicalGroup {
        std::string logical_node_id;
        std::string type_identity;
        std::vector<size_t> member_indices;
    };

    std::vector<LogicalGroup> groups;
    for (size_t i = 0; i < concrete_nodes.size(); ++i) {
        if (i >= concrete_node_logical_ids.size() || concrete_node_logical_ids[i].empty()) {
            continue;
        }
        for (auto const& logical_node_id : concrete_node_logical_ids[i]) {
            auto group_it = std::find_if(groups.begin(), groups.end(), [&](auto const& group) {
                return
                    group.logical_node_id == logical_node_id &&
                    group.type_identity == concrete_nodes[i].type_identity;
            });
            if (group_it == groups.end()) {
                groups.push_back(LogicalGroup {
                    .logical_node_id = logical_node_id,
                    .type_identity = concrete_nodes[i].type_identity,
                    .member_indices = { i },
                });
            } else if (!std::ranges::contains(group_it->member_indices, i)) {
                group_it->member_indices.push_back(i);
            }
        }
    }

    std::vector<IntrospectionLogicalNode> logical_nodes;
    logical_nodes.reserve(groups.size());
    for (size_t group_i = 0; group_i < groups.size(); ++group_i) {
        auto const& group = groups[group_i];
        size_t const same_identity_group_count = std::ranges::count_if(groups, [&](auto const& candidate) {
            return candidate.logical_node_id == group.logical_node_id;
        });
        std::vector<LogicalConcreteNode const*> members;
        members.reserve(group.member_indices.size());
        auto member_indices = group.member_indices;
        std::sort(member_indices.begin(), member_indices.end(), [&](auto a, auto b) {
            auto const& left = concrete_nodes[a];
            auto const& right = concrete_nodes[b];
            if (left.construction_order != right.construction_order) {
                return left.construction_order < right.construction_order;
            }
            return left.id < right.id;
        });
        std::vector<std::string> backing_node_ids;
        backing_node_ids.reserve(group.member_indices.size());
        std::vector<IntrospectionLogicalNode::Member> logical_members;
        logical_members.reserve(member_indices.size());
        for (auto member_index : member_indices) {
            members.push_back(&concrete_nodes[member_index]);
            auto const& backing_node_id = concrete_nodes[member_index].id;
            if (!std::ranges::contains(backing_node_ids, backing_node_id)) {
                backing_node_ids.push_back(backing_node_id);
            }
            logical_members.push_back(IntrospectionLogicalNode::Member {
                .ordinal = logical_members.size(),
                .backing_node_id = backing_node_id,
                .kind = concrete_nodes[member_index].kind,
                .type_identity = concrete_nodes[member_index].type_identity,
                .sample_inputs = aggregate_ports(
                    std::span<LogicalConcreteNode const* const>(&members.back(), 1),
                    &LogicalConcreteNode::sample_inputs
                ),
                .sample_outputs = aggregate_ports(
                    std::span<LogicalConcreteNode const* const>(&members.back(), 1),
                    &LogicalConcreteNode::sample_outputs
                ),
                .event_inputs = aggregate_ports(
                    std::span<LogicalConcreteNode const* const>(&members.back(), 1),
                    &LogicalConcreteNode::event_inputs
                ),
                .event_outputs = aggregate_ports(
                    std::span<LogicalConcreteNode const* const>(&members.back(), 1),
                    &LogicalConcreteNode::event_outputs
                ),
            });
        }

        auto source_spans = source_spans_for(members, group.logical_node_id);
        std::string logical_id = group.logical_node_id;
        if (same_identity_group_count > 1) {
            logical_id += "#type:" + stable_identity_suffix(group.type_identity);
        }

        logical_nodes.push_back(IntrospectionLogicalNode {
            .id = std::move(logical_id),
            .kind = members.front()->kind,
            .source_identity = group.logical_node_id,
            .type_identity = group.type_identity,
            .source_spans = std::move(source_spans),
            .sample_inputs = aggregate_ports(members, &LogicalConcreteNode::sample_inputs),
            .sample_outputs = aggregate_ports(members, &LogicalConcreteNode::sample_outputs),
            .event_inputs = aggregate_ports(members, &LogicalConcreteNode::event_inputs),
            .event_outputs = aggregate_ports(members, &LogicalConcreteNode::event_outputs),
            .backing_node_ids = std::move(backing_node_ids),
            .members = std::move(logical_members),
        });
    }

    std::sort(logical_nodes.begin(), logical_nodes.end(), [](auto const& a, auto const& b) {
        auto const a_file = a.source_spans.empty() ? std::string{} : a.source_spans.front().file_path;
        auto const b_file = b.source_spans.empty() ? std::string{} : b.source_spans.front().file_path;
        if (a_file != b_file) {
            return a_file < b_file;
        }
        auto const a_begin = a.source_spans.empty() ? 0u : a.source_spans.front().begin;
        auto const b_begin = b.source_spans.empty() ? 0u : b.source_spans.front().begin;
        if (a_begin != b_begin) {
            return a_begin < b_begin;
        }
        auto const a_end = a.source_spans.empty() ? 0u : a.source_spans.front().end;
        auto const b_end = b.source_spans.empty() ? 0u : b.source_spans.front().end;
        if (a_end != b_end) {
            return a_end < b_end;
        }
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        if (a.id != b.id) {
            return a.id < b.id;
        }
        if (a.source_identity != b.source_identity) {
            return a.source_identity < b.source_identity;
        }
        return a.backing_node_ids < b.backing_node_ids;
    });

    std::unordered_map<std::string, std::vector<std::string>> logical_node_ids_by_backing_node_id;
    for (auto const& logical_node : logical_nodes) {
        for (auto const& backing_node_id : logical_node.backing_node_ids) {
            logical_node_ids_by_backing_node_id[backing_node_id].push_back(logical_node.id);
        }
    }
    for (auto& [_, logical_ids] : logical_node_ids_by_backing_node_id) {
        std::sort(logical_ids.begin(), logical_ids.end());
        logical_ids.erase(std::unique(logical_ids.begin(), logical_ids.end()), logical_ids.end());
    }

    return std::make_pair(
        std::move(logical_nodes),
        std::move(logical_node_ids_by_backing_node_id)
    );
}
}
