#include <intravenous/runtime/iv_module_source_introspection.h>

#include <intravenous/compat.h>
#include <intravenous/filesystem_paths.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <ranges>
#include <sstream>
#include <span>
#include <stdexcept>
#include <unordered_set>

namespace iv {
SourceTextLineMap SourceTextLineMap::from_file(std::filesystem::path const &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open source file: " + path.string());
    }

    SourceTextLineMap map;
    map.text.assign(
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>());
    map.line_offsets.push_back(0);
    for (size_t i = 0; i < map.text.size(); ++i) {
        if (map.text[i] == '\n') {
            map.line_offsets.push_back(i + 1);
        }
    }
    return map;
}

size_t SourceTextLineMap::offset_for(SourcePosition position) const
{
    if (line_offsets.empty()) {
        return 0;
    }

    size_t const line_index =
        position.line <= 1
            ? 0
            : std::min<size_t>(position.line - 1, line_offsets.size() - 1);
    size_t const line_start = line_offsets[line_index];
    size_t const next_line_start =
        line_index + 1 < line_offsets.size() ? line_offsets[line_index + 1] : text.size();
    size_t const requested_column = position.column <= 1 ? 0 : position.column - 1;
    return std::min(line_start + requested_column, next_line_start);
}

SourcePosition SourceTextLineMap::position_for(size_t offset) const
{
    offset = std::min(offset, text.size());
    auto const it = std::upper_bound(line_offsets.begin(), line_offsets.end(), offset);
    size_t const line_index =
        it == line_offsets.begin()
            ? 0
            : static_cast<size_t>(std::distance(line_offsets.begin(), it - 1));
    return SourcePosition{
        .line = static_cast<uint32_t>(line_index + 1),
        .column = static_cast<uint32_t>(offset - line_offsets[line_index] + 1),
    };
}

namespace {
constexpr std::string_view runtime_node_id_separator = "\x1flogical:";

struct LivePortStateMaps {
    std::unordered_map<std::string, std::string> sample_inputs;
    std::unordered_map<std::string, std::string> event_inputs;
    std::unordered_map<std::string, std::string> sample_outputs;
    std::unordered_map<std::string, std::string> event_outputs;
};

std::string port_state_key(
    std::string_view logical_node_id,
    std::optional<size_t> member_ordinal,
    size_t port_ordinal)
{
    std::string key(logical_node_id);
    key += "#port:";
    key += std::to_string(port_ordinal);
    if (member_ordinal.has_value()) {
        key += "#member:";
        key += std::to_string(*member_ordinal);
    }
    return key;
}

std::string sample_input_state_value(ProjectSampleInputState state)
{
    switch (state) {
    case ProjectSampleInputState::default_:
        return "default";
    case ProjectSampleInputState::overridden:
        return "overridden";
    case ProjectSampleInputState::logical_follow:
        return "logicalFollow";
    case ProjectSampleInputState::timeline_lane:
        return "timelineLane";
    case ProjectSampleInputState::disconnected:
        return "disconnected";
    }
    return "default";
}

std::string event_input_state_value(ProjectEventInputState state)
{
    switch (state) {
    case ProjectEventInputState::default_:
        return "default";
    case ProjectEventInputState::logical_follow:
        return "logicalFollow";
    case ProjectEventInputState::timeline_lane:
        return "timelineLane";
    case ProjectEventInputState::disconnected:
        return "disconnected";
    }
    return "default";
}

std::string sample_output_state_value(ProjectSampleOutputState state)
{
    switch (state) {
    case ProjectSampleOutputState::disconnected:
        return "disconnected";
    case ProjectSampleOutputState::logical:
        return "logical";
    case ProjectSampleOutputState::timeline_lane:
        return "timelineLane";
    }
    return "disconnected";
}

std::string event_output_state_value(ProjectEventOutputState state)
{
    switch (state) {
    case ProjectEventOutputState::disconnected:
        return "disconnected";
    case ProjectEventOutputState::logical:
        return "logical";
    case ProjectEventOutputState::timeline_lane:
        return "timelineLane";
    }
    return "disconnected";
}

LivePortStateMaps build_live_port_state_maps(
    IvModuleSourceIntrospectionAuthoredStateSnapshot const &snapshot)
{
    LivePortStateMaps maps;

    for (auto const &request : snapshot.sample_input_values) {
        maps.sample_inputs[port_state_key(
            request.node_id,
            request.member_ordinal,
            request.input_ordinal)] = "overridden";
    }
    for (auto const &request : snapshot.sample_input_states) {
        maps.sample_inputs[port_state_key(
            request.node_id,
            request.member_ordinal,
            request.input_ordinal)] = sample_input_state_value(request.state);
    }
    for (auto const &request : snapshot.event_input_states) {
        maps.event_inputs[port_state_key(
            request.node_id,
            request.member_ordinal,
            request.input_ordinal)] = event_input_state_value(request.state);
    }
    for (auto const &request : snapshot.sample_output_states) {
        maps.sample_outputs[port_state_key(
            request.node_id,
            request.member_ordinal,
            request.output_ordinal)] = sample_output_state_value(request.state);
    }
    for (auto const &request : snapshot.event_output_states) {
        maps.event_outputs[port_state_key(
            request.node_id,
            request.member_ordinal,
            request.output_ordinal)] = event_output_state_value(request.state);
    }

    return maps;
}

LogicalPortInfo to_live_port(IntrospectionPortInfo const &port)
{
    return LogicalPortInfo{
        .name = port.name,
        .type = port.type,
        .connectivity = port.connectivity,
        .ordinal = port.ordinal,
        .default_value = port.default_value,
        .min = port.min,
        .max = port.max,
        .current_value = port.default_value,
        .sample_channel_type = port.sample_channel_type,
    };
}

std::vector<LogicalPortInfo> to_live_ports(std::span<IntrospectionPortInfo const> ports)
{
    std::vector<LogicalPortInfo> live_ports;
    live_ports.reserve(ports.size());
    for (auto const &port : ports) {
        live_ports.push_back(to_live_port(port));
    }
    return live_ports;
}

void sort_and_deduplicate_spans(std::vector<SourceSpan> &spans)
{
    std::sort(spans.begin(), spans.end(), [](auto const &a, auto const &b) {
        return std::tie(a.file_path, a.begin, a.end) <
               std::tie(b.file_path, b.begin, b.end);
    });
    spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
}

LoadedGraphIntrospectionIndex build_graph_introspection_index(
    std::string const &definition_id,
    GraphIntrospectionMetadata const &introspection,
    std::filesystem::path const &module_root,
    std::string const &module_id,
    std::span<ModuleDependency const> dependencies)
{
    LoadedGraphIntrospectionIndex graph_index;
    graph_index.definition_id = definition_id;
    graph_index.module_root = normalize_path(module_root);
    graph_index.module_id = module_id;
    graph_index.logical_nodes = introspection.logical_nodes;
    graph_index.dependency_file_paths.insert(
        normalized_path_string(graph_index.module_root / "module.cpp"));
    for (auto const &dependency : dependencies) {
        if (!dependency.entry_file.empty()) {
            graph_index.dependency_file_paths.insert(
                normalized_path_string(dependency.entry_file));
        }
    }
    for (auto &logical_node : graph_index.logical_nodes) {
        for (auto &span : logical_node.source_spans) {
            if (!span.file_path.empty()) {
                span.file_path = normalized_path_string(span.file_path);
                graph_index.dependency_file_paths.insert(span.file_path);
            }
        }
        sort_and_deduplicate_spans(logical_node.source_spans);
    }
    for (size_t i = 0; i < graph_index.logical_nodes.size(); ++i) {
        graph_index.logical_node_index_by_id.emplace(graph_index.logical_nodes[i].id, i);
    }

    return graph_index;
}

std::string runtime_node_id(
    std::string_view instance_id,
    std::string_view logical_node_id)
{
    std::string value(instance_id);
    value += runtime_node_id_separator;
    value += logical_node_id;
    return value;
}

struct ResolvedRuntimeNodeId {
    std::string instance_id;
    std::string logical_node_id;
};

std::optional<ResolvedRuntimeNodeId> parse_runtime_node_id(std::string_view runtime_id)
{
    auto const separator = runtime_id.find(runtime_node_id_separator);
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    return ResolvedRuntimeNodeId{
        .instance_id = std::string(runtime_id.substr(0, separator)),
        .logical_node_id = std::string(runtime_id.substr(separator + runtime_node_id_separator.size())),
    };
}

std::string live_input_snapshot_key(
    std::string_view logical_node_id,
    std::optional<size_t> member_ordinal,
    size_t input_ordinal)
{
    std::string key(logical_node_id);
    key += "#input:";
    key += std::to_string(input_ordinal);
    if (member_ordinal.has_value()) {
        key += "#member:";
        key += std::to_string(*member_ordinal);
    }
    return key;
}

GraphInputPortDescriptor sample_graph_input_port_for(
    IntrospectionLogicalNode const &node,
    std::optional<size_t> concrete_member_ordinal,
    size_t input_ordinal)
{
    auto const &ports = concrete_member_ordinal.has_value()
        ? node.members[*concrete_member_ordinal].sample_inputs
        : node.sample_inputs;
    auto const port_it = std::ranges::find_if(ports, [&](IntrospectionPortInfo const &port) {
        return port.ordinal == input_ordinal;
    });
    if (port_it == ports.end()) {
        throw std::runtime_error("unknown sample input ordinal " + std::to_string(input_ordinal));
    }
    return GraphInputPortDescriptor{
        .logical_node_id = node.id,
        .concrete_member_ordinal = concrete_member_ordinal,
        .port_kind = PortKind::sample,
        .port_ordinal = port_it->ordinal,
        .port_name = port_it->name,
        .port_type = port_it->type,
    };
}
} // namespace

SourceTextLineMap const &
IvModuleSourceIntrospection::source_text_for(std::string const &normalized_path) const
{
    auto it = source_text_cache.find(normalized_path);
    if (it == source_text_cache.end()) {
        it = source_text_cache.emplace(
            normalized_path, SourceTextLineMap::from_file(normalized_path)).first;
    }
    return it->second;
}

void IvModuleSourceIntrospection::invalidate_source_text(std::string const &normalized_path)
{
    source_text_cache.erase(normalized_path);
}

void IvModuleSourceIntrospection::invalidate_source_texts(
    std::span<ModuleDependency const> dependencies)
{
    for (auto const &dependency : dependencies) {
        if (dependency.entry_file.empty()) {
            continue;
        }
        invalidate_source_text(normalized_path_string(dependency.entry_file));
    }
}

std::pair<uint32_t, uint32_t> IvModuleSourceIntrospection::byte_range_for(
    std::string const &normalized_path,
    SourceRange const &range) const
{
    SourceTextLineMap const &index = source_text_for(normalized_path);
    return {
        static_cast<uint32_t>(index.offset_for(range.start)),
        static_cast<uint32_t>(index.offset_for(range.end)),
    };
}

LiveSourceSpan IvModuleSourceIntrospection::to_live_span(SourceSpan const &span) const
{
    SourceTextLineMap const &index = source_text_for(span.file_path);
    return LiveSourceSpan{
        .file_path = span.file_path,
        .range = SourceRange{
            .start = index.position_for(span.begin),
            .end = index.position_for(span.end),
        },
    };
}

LogicalNodeInfo IvModuleSourceIntrospection::to_logical_node(
    IntrospectionLogicalNode const &node,
    std::string const &instance_id) const
{
    auto const runtime_id = runtime_node_id(instance_id, node.id);
    IvModuleSourceIntrospectionAuthoredStateSnapshotBuilder authored_state_builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_source_introspection_authored_state_snapshot_requested_event,
        authored_state_builder);
    auto const live_port_states = build_live_port_state_maps(authored_state_builder.build());
    std::vector<IvModuleSourceIntrospectionLiveInputSnapshotRequest> snapshot_requests;
    snapshot_requests.reserve(
        node.sample_inputs.size() +
        std::accumulate(
            node.members.begin(),
            node.members.end(),
            size_t{0},
            [](size_t sum, auto const &member) {
                return sum + member.sample_inputs.size();
            }));
    for (auto const &port : node.sample_inputs) {
        snapshot_requests.push_back(IvModuleSourceIntrospectionLiveInputSnapshotRequest{
            .logical_node_id = runtime_id,
            .member_ordinal = std::nullopt,
            .input_ordinal = port.ordinal,
            .fallback = port.default_value,
        });
    }
    for (auto const &member : node.members) {
        for (auto const &port : member.sample_inputs) {
            snapshot_requests.push_back(IvModuleSourceIntrospectionLiveInputSnapshotRequest{
                .logical_node_id = runtime_id,
                .member_ordinal = member.ordinal,
                .input_ordinal = port.ordinal,
                .fallback = port.default_value,
            });
        }
    }

    IvModuleSourceIntrospectionLiveInputSnapshotsBuilder snapshot_builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_source_introspection_live_input_snapshots_requested_event,
        snapshot_requests,
        snapshot_builder);
    auto const snapshots = snapshot_builder.build();

    std::unordered_map<std::string, IvModuleSourceIntrospectionLiveInputSnapshot> snapshots_by_key;
    snapshots_by_key.reserve(snapshots.size());
    for (auto const &snapshot : snapshots) {
        snapshots_by_key.emplace(
            live_input_snapshot_key(
                snapshot.logical_node_id,
                snapshot.member_ordinal,
                snapshot.input_ordinal),
            snapshot);
    }

    LogicalNodeInfo live;
    live.id = runtime_id;
    live.instance_id = instance_id;
    live.kind = node.kind;
    live.source_identity = node.source_identity;
    live.type_identity = node.type_identity;
    live.sample_inputs = to_live_ports(node.sample_inputs);
    for (auto &port : live.sample_inputs) {
        auto const snapshot_it = snapshots_by_key.find(
            live_input_snapshot_key(runtime_id, std::nullopt, port.ordinal));
        if (snapshot_it == snapshots_by_key.end()) {
            throw std::runtime_error("missing live input snapshot for logical input");
        }
        port.current_value = snapshot_it->second.current_value;
        auto const state_it = live_port_states.sample_inputs.find(
            port_state_key(runtime_id, std::nullopt, port.ordinal));
        port.state_value =
            state_it != live_port_states.sample_inputs.end()
                ? state_it->second
                : "overridden";
    }
    live.sample_outputs = to_live_ports(node.sample_outputs);
    for (auto &port : live.sample_outputs) {
        auto const state_it = live_port_states.sample_outputs.find(
            port_state_key(runtime_id, std::nullopt, port.ordinal));
        port.state_value =
            state_it != live_port_states.sample_outputs.end()
                ? state_it->second
                : "disconnected";
    }
    live.event_inputs = to_live_ports(node.event_inputs);
    for (auto &port : live.event_inputs) {
        auto const state_it = live_port_states.event_inputs.find(
            port_state_key(runtime_id, std::nullopt, port.ordinal));
        port.state_value =
            state_it != live_port_states.event_inputs.end()
                ? state_it->second
                : "default";
    }
    live.event_outputs = to_live_ports(node.event_outputs);
    for (auto &port : live.event_outputs) {
        auto const state_it = live_port_states.event_outputs.find(
            port_state_key(runtime_id, std::nullopt, port.ordinal));
        port.state_value =
            state_it != live_port_states.event_outputs.end()
                ? state_it->second
                : "disconnected";
    }
    live.member_count = node.backing_node_ids.size();
    live.members.reserve(node.members.size());
    for (auto const &member : node.members) {
        LogicalNodeMemberInfo live_member;
        live_member.ordinal = member.ordinal;
        live_member.backing_node_id = member.backing_node_id;
        live_member.kind = member.kind;
        live_member.type_identity = member.type_identity;
        live_member.sample_inputs = to_live_ports(member.sample_inputs);
        for (auto &port : live_member.sample_inputs) {
            auto const snapshot_it = snapshots_by_key.find(
                live_input_snapshot_key(runtime_id, member.ordinal, port.ordinal));
            if (snapshot_it == snapshots_by_key.end()) {
                throw std::runtime_error("missing live input snapshot for concrete input");
            }
            port.current_value = snapshot_it->second.current_value;
            port.has_concrete_override = snapshot_it->second.has_concrete_override;
            auto const state_it = live_port_states.sample_inputs.find(
                port_state_key(runtime_id, member.ordinal, port.ordinal));
            if (state_it != live_port_states.sample_inputs.end()) {
                port.state_value = state_it->second;
            } else {
                port.state_value =
                    port.connectivity == LogicalPortConnectivity::connected
                        ? "disconnected"
                        : "logicalFollow";
            }
        }
        live_member.sample_outputs = to_live_ports(member.sample_outputs);
        for (auto &port : live_member.sample_outputs) {
            auto const state_it = live_port_states.sample_outputs.find(
                port_state_key(runtime_id, member.ordinal, port.ordinal));
            if (state_it != live_port_states.sample_outputs.end()) {
                port.state_value = state_it->second;
            } else {
                auto const logical_state_it = live_port_states.sample_outputs.find(
                    port_state_key(runtime_id, std::nullopt, port.ordinal));
                port.state_value =
                    logical_state_it != live_port_states.sample_outputs.end()
                    && logical_state_it->second == "timelineLane"
                        ? "logical"
                        : "disconnected";
            }
        }
        live_member.event_inputs = to_live_ports(member.event_inputs);
        for (auto &port : live_member.event_inputs) {
            auto const state_it = live_port_states.event_inputs.find(
                port_state_key(runtime_id, member.ordinal, port.ordinal));
            if (state_it != live_port_states.event_inputs.end()) {
                port.state_value = state_it->second;
            } else {
                port.state_value =
                    port.connectivity == LogicalPortConnectivity::connected
                        ? "disconnected"
                        : "logicalFollow";
            }
        }
        live_member.event_outputs = to_live_ports(member.event_outputs);
        for (auto &port : live_member.event_outputs) {
            auto const state_it = live_port_states.event_outputs.find(
                port_state_key(runtime_id, member.ordinal, port.ordinal));
            if (state_it != live_port_states.event_outputs.end()) {
                port.state_value = state_it->second;
            } else {
                auto const logical_state_it = live_port_states.event_outputs.find(
                    port_state_key(runtime_id, std::nullopt, port.ordinal));
                port.state_value =
                    logical_state_it != live_port_states.event_outputs.end()
                    && logical_state_it->second == "timelineLane"
                        ? "logical"
                        : "disconnected";
            }
        }
        live.members.push_back(std::move(live_member));
    }
    live.source_spans.reserve(node.source_spans.size());
    for (auto const &span : node.source_spans) {
        live.source_spans.push_back(to_live_span(span));
    }
    return live;
}

void IvModuleSourceIntrospection::handle_iv_module_definitions_changed(
    IvModuleDefinitionsChanged const &diff)
{
    std::scoped_lock lock(mutex);
    for (auto const &definition_id : diff.deleted_definition_ids) {
        graph_indexes_by_definition_id.erase(definition_id);
    }
    for (auto const &definition : diff.created) {
        invalidate_source_texts(definition.dependencies);
        graph_indexes_by_definition_id[definition.definition_id] =
            build_graph_introspection_index(
                definition.definition_id,
                definition.introspection,
                definition.module_root,
                definition.module_id,
                definition.dependencies);
    }
    for (auto const &definition : diff.updated) {
        invalidate_source_texts(definition.dependencies);
        graph_indexes_by_definition_id[definition.definition_id] =
            build_graph_introspection_index(
                definition.definition_id,
                definition.introspection,
                definition.module_root,
                definition.module_id,
                definition.dependencies);
    }
}

void IvModuleSourceIntrospection::handle_iv_module_instances_list_changed(
    std::vector<IvModuleInstanceInfo> const &instances)
{
    std::scoped_lock lock(mutex);
    realized_instances_by_id.clear();
    for (auto const &instance : instances) {
        if (!instance.realized || instance.instance_id.empty()) {
            continue;
        }
        realized_instances_by_id.emplace(instance.instance_id, instance);
    }
}

ProjectQueryResult IvModuleSourceIntrospection::query_by_spans(
    std::filesystem::path const &file_path,
    std::vector<SourceRange> const &ranges,
    SourceRangeMatchMode match_mode,
    std::optional<std::string> instance_id) const
{
    std::scoped_lock lock(mutex);
    if (graph_indexes_by_definition_id.empty() || realized_instances_by_id.empty()) {
        return {};
    }

    std::string const normalized_file_path = normalized_path_string(file_path);
    std::vector<std::pair<uint32_t, uint32_t>> requested_ranges;
    requested_ranges.reserve(ranges.size());
    for (auto const &range : ranges) {
        requested_ranges.push_back(byte_range_for(normalized_file_path, range));
    }

    ProjectQueryResult result;

    auto span_touches_range =
        [](SourceSpan const &span,
           std::pair<uint32_t, uint32_t> const &requested_range) {
            auto const [begin, end] = requested_range;
            if (begin == end) {
                return span.begin <= begin && begin <= span.end;
            }
            return span.begin <= end && begin <= span.end;
        };

    auto span_distance_to_range =
        [](SourceSpan const &span,
           std::pair<uint32_t, uint32_t> const &requested_range) {
            auto const [begin, end] = requested_range;
            if (span.begin <= end && begin <= span.end) {
                return 0u;
            }
            if (span.end < begin) {
                return begin - span.end;
            }
            return span.begin - end;
        };

    struct RankedRuntimeLogicalNode {
        std::string definition_id;
        size_t logical_index = 0;
        uint32_t best_span_size = std::numeric_limits<uint32_t>::max();
        uint32_t best_distance = std::numeric_limits<uint32_t>::max();
        uint32_t best_begin = std::numeric_limits<uint32_t>::max();
        uint32_t best_end = std::numeric_limits<uint32_t>::max();
    };

    std::vector<RankedRuntimeLogicalNode> ranked_nodes;
    for (auto const &[definition_id, graph_index] : graph_indexes_by_definition_id) {
        for (size_t logical_index = 0; logical_index < graph_index.logical_nodes.size();
             ++logical_index) {
            auto const &node = graph_index.logical_nodes[logical_index];
            bool matches = requested_ranges.empty();
            RankedRuntimeLogicalNode ranked{
                .definition_id = definition_id,
                .logical_index = logical_index,
            };
            if (!requested_ranges.empty()) {
                auto const node_matches_range =
                    [&](std::pair<uint32_t, uint32_t> const &requested_range) {
                        bool any = false;
                        for (auto const &span : node.source_spans) {
                            if (span.file_path != normalized_file_path ||
                                !span_touches_range(span, requested_range)) {
                                continue;
                            }
                            any = true;
                            auto const span_size =
                                span.end >= span.begin ? span.end - span.begin : 0u;
                            auto const distance =
                                span_distance_to_range(span, requested_range);
                            ranked.best_span_size = std::min(ranked.best_span_size, span_size);
                            ranked.best_distance = std::min(ranked.best_distance, distance);
                            ranked.best_begin = std::min(ranked.best_begin, span.begin);
                            ranked.best_end = std::min(ranked.best_end, span.end);
                        }
                        return any;
                    };
                if (match_mode == SourceRangeMatchMode::union_) {
                    matches = std::ranges::any_of(requested_ranges, node_matches_range);
                } else {
                    matches = std::ranges::all_of(requested_ranges, node_matches_range);
                }
            } else if (!node.source_spans.empty()) {
                ranked.best_span_size =
                    node.source_spans.front().end >= node.source_spans.front().begin
                        ? node.source_spans.front().end - node.source_spans.front().begin
                        : 0u;
                ranked.best_distance = 0u;
                ranked.best_begin = node.source_spans.front().begin;
                ranked.best_end = node.source_spans.front().end;
            }
            if (!matches) {
                continue;
            }
            ranked_nodes.push_back(ranked);
        }
    }

    std::sort(ranked_nodes.begin(), ranked_nodes.end(), [&](auto const &a, auto const &b) {
        if (a.best_span_size != b.best_span_size) {
            return a.best_span_size < b.best_span_size;
        }
        if (a.best_distance != b.best_distance) {
            return a.best_distance < b.best_distance;
        }
        if (a.best_begin != b.best_begin) {
            return a.best_begin < b.best_begin;
        }
        if (a.best_end != b.best_end) {
            return a.best_end < b.best_end;
        }
        auto const &a_index = graph_indexes_by_definition_id.at(a.definition_id);
        auto const &b_index = graph_indexes_by_definition_id.at(b.definition_id);
        auto const &a_node = a_index.logical_nodes[a.logical_index];
        auto const &b_node = b_index.logical_nodes[b.logical_index];
        if (a_node.kind != b_node.kind) {
            return a_node.kind < b_node.kind;
        }
        if (a_node.id != b_node.id) {
            return a_node.id < b_node.id;
        }
        return a.definition_id < b.definition_id;
    });

    std::unordered_set<std::string> emitted;
    for (auto const &ranked : ranked_nodes) {
        auto const index_it = graph_indexes_by_definition_id.find(ranked.definition_id);
        if (index_it == graph_indexes_by_definition_id.end()) {
            continue;
        }
        auto const &graph_index = index_it->second;
        auto const &node = graph_index.logical_nodes[ranked.logical_index];
        std::vector<std::string> matching_instance_ids;
        for (auto const &[candidate_instance_id, instance] : realized_instances_by_id) {
            if (instance.definition_id != ranked.definition_id) {
                continue;
            }
            if (instance_id.has_value() && candidate_instance_id != *instance_id) {
                continue;
            }
            matching_instance_ids.push_back(candidate_instance_id);
        }
        std::sort(matching_instance_ids.begin(), matching_instance_ids.end());
        for (auto const &matching_instance_id : matching_instance_ids) {
            auto const emitted_id = runtime_node_id(matching_instance_id, node.id);
            if (!emitted.insert(emitted_id).second) {
                continue;
            }
            result.nodes.push_back(to_logical_node(node, matching_instance_id));
        }
    }

    return result;
}

ProjectRegionQueryResult IvModuleSourceIntrospection::query_active_regions(
    std::filesystem::path const &file_path) const
{
    std::scoped_lock lock(mutex);
    if (graph_indexes_by_definition_id.empty()) {
        return {};
    }

    std::string const normalized_file_path = normalized_path_string(file_path);
    ProjectRegionQueryResult result;

    std::unordered_set<std::string> emitted_spans;
    for (auto const &[_, graph_index] : graph_indexes_by_definition_id) {
        for (auto const &node : graph_index.logical_nodes) {
            for (auto const &span : node.source_spans) {
                if (span.file_path != normalized_file_path) {
                    continue;
                }
                auto live_span = to_live_span(span);
                auto const key = live_span.file_path + ":" +
                                 std::to_string(live_span.range.start.line) + ":" +
                                 std::to_string(live_span.range.start.column) + ":" +
                                 std::to_string(live_span.range.end.line) + ":" +
                                 std::to_string(live_span.range.end.column);
                if (emitted_spans.insert(key).second) {
                    result.source_spans.push_back(std::move(live_span));
                }
            }
        }
    }

    return result;
}

bool IvModuleSourceIntrospection::definition_uses_source_file(
    std::string const &definition_id,
    std::filesystem::path const &file_path) const
{
    std::scoped_lock lock(mutex);
    auto const definition = graph_indexes_by_definition_id.find(definition_id);
    if (definition == graph_indexes_by_definition_id.end()) {
        return false;
    }
    return definition->second.dependency_file_paths.contains(
        normalized_path_string(file_path));
}

LogicalNodeInfo IvModuleSourceIntrospection::get_logical_node(std::string const &node_id) const
{
    std::scoped_lock lock(mutex);
    auto const resolved = parse_runtime_node_id(node_id);
    if (!resolved.has_value()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    auto const instance_it = realized_instances_by_id.find(resolved->instance_id);
    if (instance_it == realized_instances_by_id.end()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    auto const index_it = graph_indexes_by_definition_id.find(instance_it->second.definition_id);
    if (index_it == graph_indexes_by_definition_id.end()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    auto const logical_it = index_it->second.logical_node_index_by_id.find(resolved->logical_node_id);
    if (logical_it == index_it->second.logical_node_index_by_id.end()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    return to_logical_node(
        index_it->second.logical_nodes[logical_it->second],
        resolved->instance_id);
}

std::vector<LogicalNodeInfo> IvModuleSourceIntrospection::get_logical_nodes(
    std::vector<std::string> const &node_ids) const
{
    std::scoped_lock lock(mutex);
    std::vector<LogicalNodeInfo> nodes;
    nodes.reserve(node_ids.size());
    for (auto const &node_id : node_ids) {
        auto const resolved = parse_runtime_node_id(node_id);
        if (!resolved.has_value()) {
            throw std::runtime_error("unknown node id: " + node_id);
        }
        auto const instance_it = realized_instances_by_id.find(resolved->instance_id);
        if (instance_it == realized_instances_by_id.end()) {
            throw std::runtime_error("unknown node id: " + node_id);
        }
        auto const index_it = graph_indexes_by_definition_id.find(instance_it->second.definition_id);
        if (index_it == graph_indexes_by_definition_id.end()) {
            throw std::runtime_error("unknown node id: " + node_id);
        }
        auto const logical_it = index_it->second.logical_node_index_by_id.find(resolved->logical_node_id);
        if (logical_it == index_it->second.logical_node_index_by_id.end()) {
            throw std::runtime_error("unknown node id: " + node_id);
        }
        nodes.push_back(to_logical_node(
            index_it->second.logical_nodes[logical_it->second],
            resolved->instance_id));
    }
    return nodes;
}

std::vector<LogicalNodeInfo> IvModuleSourceIntrospection::get_logical_nodes_for_instances(
    std::vector<IvModuleInstanceInfo> const &instances) const
{
    std::scoped_lock lock(mutex);
    std::vector<LogicalNodeInfo> nodes;
    for (auto const &instance : instances) {
        if (!instance.realized || instance.instance_id.empty() || instance.definition_id.empty()) {
            continue;
        }
        auto const index_it = graph_indexes_by_definition_id.find(instance.definition_id);
        if (index_it == graph_indexes_by_definition_id.end()) {
            continue;
        }
        for (auto const &logical_node : index_it->second.logical_nodes) {
            nodes.push_back(to_logical_node(logical_node, instance.instance_id));
        }
    }
    return nodes;
}

GraphInputPortDescriptor IvModuleSourceIntrospection::sample_graph_input_port_for_node(
    std::string const &node_id,
    std::optional<size_t> concrete_member_ordinal,
    size_t input_ordinal) const
{
    std::scoped_lock lock(mutex);
    auto const resolved = parse_runtime_node_id(node_id);
    if (!resolved.has_value()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    auto const instance_it = realized_instances_by_id.find(resolved->instance_id);
    if (instance_it == realized_instances_by_id.end()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    auto const index_it = graph_indexes_by_definition_id.find(instance_it->second.definition_id);
    if (index_it == graph_indexes_by_definition_id.end()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    auto const logical_it = index_it->second.logical_node_index_by_id.find(resolved->logical_node_id);
    if (logical_it == index_it->second.logical_node_index_by_id.end()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    auto const &node = index_it->second.logical_nodes[logical_it->second];
    if (concrete_member_ordinal.has_value() &&
        *concrete_member_ordinal >= node.members.size()) {
        throw std::runtime_error("unknown member ordinal " +
                                 std::to_string(*concrete_member_ordinal) +
                                 " for node id: " + node_id);
    }
    auto descriptor = sample_graph_input_port_for(node, concrete_member_ordinal, input_ordinal);
    descriptor.logical_node_id = node_id;
    return descriptor;
}
} // namespace iv
