#include "runtime/project_introspection.h"

#include "compat.h"
#include "filesystem_paths.h"
#include "runtime/project_introspection_events.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <ranges>
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
void sort_and_deduplicate_spans(std::vector<SourceSpan> &spans)
{
    std::sort(spans.begin(), spans.end(), [](auto const &a, auto const &b) {
        return std::tie(a.file_path, a.begin, a.end) <
               std::tie(b.file_path, b.begin, b.end);
    });
    spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
}

LoadedGraphIntrospectionIndex build_graph_introspection_index(
    GraphIntrospectionMetadata const &introspection,
    std::filesystem::path const &module_root,
    std::string const &module_id)
{
    LoadedGraphIntrospectionIndex graph_index;
    graph_index.module_root = normalize_path(module_root);
    graph_index.module_id = module_id;
    graph_index.logical_nodes = introspection.logical_nodes;
    for (auto &logical_node : graph_index.logical_nodes) {
        for (auto &span : logical_node.source_spans) {
            if (!span.file_path.empty()) {
                span.file_path = normalized_path_string(span.file_path);
            }
        }
        sort_and_deduplicate_spans(logical_node.source_spans);
    }
    for (size_t i = 0; i < graph_index.logical_nodes.size(); ++i) {
        graph_index.logical_node_index_by_id.emplace(graph_index.logical_nodes[i].id, i);
    }

    return graph_index;
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
    auto const port_it = std::ranges::find_if(ports, [&](LogicalPortInfo const &port) {
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
RuntimeProjectIntrospection::source_text_for(std::string const &normalized_path) const
{
    auto it = source_text_cache.find(normalized_path);
    if (it == source_text_cache.end()) {
        it = source_text_cache.emplace(
            normalized_path, SourceTextLineMap::from_file(normalized_path)).first;
    }
    return it->second;
}

void RuntimeProjectIntrospection::invalidate_source_text(std::string const &normalized_path)
{
    source_text_cache.erase(normalized_path);
}

void RuntimeProjectIntrospection::invalidate_source_texts(
    std::span<ModuleDependency const> dependencies)
{
    for (auto const &dependency : dependencies) {
        if (dependency.entry_file.empty()) {
            continue;
        }
        invalidate_source_text(normalized_path_string(dependency.entry_file));
    }
}

std::pair<uint32_t, uint32_t> RuntimeProjectIntrospection::byte_range_for(
    std::string const &normalized_path,
    SourceRange const &range) const
{
    SourceTextLineMap const &index = source_text_for(normalized_path);
    return {
        static_cast<uint32_t>(index.offset_for(range.start)),
        static_cast<uint32_t>(index.offset_for(range.end)),
    };
}

LiveSourceSpan RuntimeProjectIntrospection::to_live_span(SourceSpan const &span) const
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

LogicalNodeInfo RuntimeProjectIntrospection::to_logical_node(
    IntrospectionLogicalNode const &node) const
{
    std::vector<RuntimeProjectIntrospectionLiveInputSnapshotRequest> snapshot_requests;
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
        snapshot_requests.push_back(RuntimeProjectIntrospectionLiveInputSnapshotRequest{
            .logical_node_id = node.id,
            .member_ordinal = std::nullopt,
            .input_ordinal = port.ordinal,
            .fallback = port.default_value,
        });
    }
    for (auto const &member : node.members) {
        for (auto const &port : member.sample_inputs) {
            snapshot_requests.push_back(RuntimeProjectIntrospectionLiveInputSnapshotRequest{
                .logical_node_id = node.id,
                .member_ordinal = member.ordinal,
                .input_ordinal = port.ordinal,
                .fallback = port.default_value,
            });
        }
    }

    RuntimeProjectIntrospectionLiveInputSnapshotsBuilder snapshot_builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_introspection_live_input_snapshots_requested_event,
        snapshot_requests,
        snapshot_builder);
    auto const snapshots = snapshot_builder.build();

    std::unordered_map<std::string, RuntimeProjectIntrospectionLiveInputSnapshot> snapshots_by_key;
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
    live.id = node.id;
    live.kind = node.kind;
    live.source_identity = node.source_identity;
    live.type_identity = node.type_identity;
    live.sample_inputs = node.sample_inputs;
    for (auto &port : live.sample_inputs) {
        auto const snapshot_it = snapshots_by_key.find(
            live_input_snapshot_key(node.id, std::nullopt, port.ordinal));
        if (snapshot_it == snapshots_by_key.end()) {
            throw std::runtime_error("missing live input snapshot for logical input");
        }
        port.current_value = snapshot_it->second.current_value;
    }
    live.sample_outputs = node.sample_outputs;
    live.event_inputs = node.event_inputs;
    live.event_outputs = node.event_outputs;
    live.member_count = node.backing_node_ids.size();
    live.members.reserve(node.members.size());
    for (auto const &member : node.members) {
        LogicalNodeMemberInfo live_member;
        live_member.ordinal = member.ordinal;
        live_member.backing_node_id = member.backing_node_id;
        live_member.kind = member.kind;
        live_member.type_identity = member.type_identity;
        live_member.sample_inputs = member.sample_inputs;
        for (auto &port : live_member.sample_inputs) {
            auto const snapshot_it = snapshots_by_key.find(
                live_input_snapshot_key(node.id, member.ordinal, port.ordinal));
            if (snapshot_it == snapshots_by_key.end()) {
                throw std::runtime_error("missing live input snapshot for concrete input");
            }
            port.current_value = snapshot_it->second.current_value;
            port.has_concrete_override = snapshot_it->second.has_concrete_override;
        }
        live_member.sample_outputs = member.sample_outputs;
        live_member.event_inputs = member.event_inputs;
        live_member.event_outputs = member.event_outputs;
        live.members.push_back(std::move(live_member));
    }
    live.source_spans.reserve(node.source_spans.size());
    for (auto const &span : node.source_spans) {
        live.source_spans.push_back(to_live_span(span));
    }
    return live;
}

RuntimeProjectInitializeResult RuntimeProjectIntrospection::initialize() const
{
    std::scoped_lock lock(mutex);
    if (!graph_index.has_value()) {
        throw std::runtime_error(
            "runtime project introspection failed to produce an initial graph index");
    }

    return RuntimeProjectInitializeResult{
        .module_root = graph_index->module_root,
        .module_id = graph_index->module_id,
    };
}

void RuntimeProjectIntrospection::handle_iv_module_definitions_changed(
    RuntimeIvModuleDefinitionsChanged const &diff)
{
    auto const changed_count = diff.created.size() + diff.updated.size();
    if (changed_count == 0 && diff.deleted_definition_ids.empty()) {
        return;
    }
    if (changed_count > 1 || (!diff.created.empty() && !diff.updated.empty()) ||
        !diff.deleted_definition_ids.empty()) {
        throw std::runtime_error(
            "multiple iv-module definition changes are not yet supported");
    }

    RuntimeIvModuleDefinition const &definition =
        !diff.created.empty() ? diff.created.front() : diff.updated.front();
    invalidate_source_texts(definition.dependencies);

    std::scoped_lock lock(mutex);
    graph_index = build_graph_introspection_index(
        definition.introspection,
        definition.module_root,
        definition.module_id);
}

RuntimeProjectQueryResult RuntimeProjectIntrospection::query_by_spans(
    std::filesystem::path const &file_path,
    std::vector<SourceRange> const &ranges,
    SourceRangeMatchMode match_mode) const
{
    std::scoped_lock lock(mutex);
    if (!graph_index.has_value()) {
        throw std::runtime_error("runtime project introspection is not initialized");
    }

    std::string const normalized_file_path = normalized_path_string(file_path);
    std::vector<std::pair<uint32_t, uint32_t>> requested_ranges;
    requested_ranges.reserve(ranges.size());
    for (auto const &range : ranges) {
        requested_ranges.push_back(byte_range_for(normalized_file_path, range));
    }

    RuntimeProjectQueryResult result;

    auto span_touches_range =
        [](SourceSpan const &span,
           std::pair<uint32_t, uint32_t> const &requested_range) {
            auto const [begin, end] = requested_range;
            if (begin == end) {
                return span.begin <= begin && begin <= span.end;
            }
            return span.begin <= end && begin <= span.end;
        };

    struct RankedLogicalNode {
        size_t logical_index = 0;
        uint32_t best_span_size = std::numeric_limits<uint32_t>::max();
        uint32_t best_distance = std::numeric_limits<uint32_t>::max();
        uint32_t best_begin = std::numeric_limits<uint32_t>::max();
        uint32_t best_end = std::numeric_limits<uint32_t>::max();
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

    std::vector<RankedLogicalNode> ranked_nodes;
    ranked_nodes.reserve(graph_index->logical_nodes.size());
    for (size_t logical_index = 0; logical_index < graph_index->logical_nodes.size();
         ++logical_index) {
        auto const &node = graph_index->logical_nodes[logical_index];
        bool matches = requested_ranges.empty();
        RankedLogicalNode ranked{.logical_index = logical_index};
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
        auto const &a_node = graph_index->logical_nodes[a.logical_index];
        auto const &b_node = graph_index->logical_nodes[b.logical_index];
        if (a_node.kind != b_node.kind) {
            return a_node.kind < b_node.kind;
        }
        return a_node.id < b_node.id;
    });

    std::unordered_set<std::string> emitted;
    for (auto const &ranked : ranked_nodes) {
        auto const &node = graph_index->logical_nodes[ranked.logical_index];
        if (emitted.contains(node.id)) {
            continue;
        }
        emitted.insert(node.id);
        result.nodes.push_back(to_logical_node(node));
    }

    return result;
}

RuntimeProjectRegionQueryResult RuntimeProjectIntrospection::query_active_regions(
    std::filesystem::path const &file_path) const
{
    std::scoped_lock lock(mutex);
    if (!graph_index.has_value()) {
        throw std::runtime_error("runtime project introspection is not initialized");
    }

    std::string const normalized_file_path = normalized_path_string(file_path);
    RuntimeProjectRegionQueryResult result;

    std::unordered_set<std::string> emitted_spans;
    for (auto const &node : graph_index->logical_nodes) {
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

    return result;
}

LogicalNodeInfo RuntimeProjectIntrospection::get_logical_node(std::string const &node_id) const
{
    std::scoped_lock lock(mutex);
    if (!graph_index.has_value()) {
        throw std::runtime_error("runtime project introspection is not initialized");
    }

    auto const it = graph_index->logical_node_index_by_id.find(node_id);
    if (it == graph_index->logical_node_index_by_id.end()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    return to_logical_node(graph_index->logical_nodes[it->second]);
}

std::vector<LogicalNodeInfo> RuntimeProjectIntrospection::get_logical_nodes(
    std::vector<std::string> const &node_ids) const
{
    std::scoped_lock lock(mutex);
    if (!graph_index.has_value()) {
        throw std::runtime_error("runtime project introspection is not initialized");
    }

    std::vector<LogicalNodeInfo> nodes;
    nodes.reserve(node_ids.size());
    for (auto const &node_id : node_ids) {
        auto const it = graph_index->logical_node_index_by_id.find(node_id);
        if (it == graph_index->logical_node_index_by_id.end()) {
            throw std::runtime_error("unknown node id: " + node_id);
        }
        nodes.push_back(to_logical_node(graph_index->logical_nodes[it->second]));
    }
    return nodes;
}

GraphInputPortDescriptor RuntimeProjectIntrospection::sample_graph_input_port_for_node(
    std::string const &node_id,
    std::optional<size_t> concrete_member_ordinal,
    size_t input_ordinal) const
{
    std::scoped_lock lock(mutex);
    if (!graph_index.has_value()) {
        throw std::runtime_error("runtime project introspection is not initialized");
    }
    auto const it = graph_index->logical_node_index_by_id.find(node_id);
    if (it == graph_index->logical_node_index_by_id.end()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    auto const &node = graph_index->logical_nodes[it->second];
    if (concrete_member_ordinal.has_value() &&
        *concrete_member_ordinal >= node.members.size()) {
        throw std::runtime_error("unknown member ordinal " +
                                 std::to_string(*concrete_member_ordinal) +
                                 " for node id: " + node_id);
    }
    return sample_graph_input_port_for(node, concrete_member_ordinal, input_ordinal);
}
} // namespace iv
