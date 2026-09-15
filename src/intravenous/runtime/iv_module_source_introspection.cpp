#include <intravenous/runtime/iv_module_source_introspection.h>

#include <intravenous/graph/names.h>

#include <intravenous/compat.h>
#include <intravenous/filesystem_paths.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

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
constexpr std::string_view runtime_node_id_separator = "\x1fvirtual:";

VirtualPortInfo to_live_port(IntrospectionPortInfo const &port)
{
    return VirtualPortInfo{
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

std::vector<VirtualPortInfo> to_live_ports(std::span<IntrospectionPortInfo const> ports)
{
    std::vector<VirtualPortInfo> live_ports;
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
    graph_index.virtual_nodes = introspection.virtual_nodes;
    graph_index.dependency_file_paths.insert(
        normalized_path_string(graph_index.module_root / "module.cpp"));
    for (auto const &dependency : dependencies) {
        if (!dependency.entry_file.empty()) {
            graph_index.dependency_file_paths.insert(
                normalized_path_string(dependency.entry_file));
        }
    }
    for (auto &virtual_node : graph_index.virtual_nodes) {
        std::erase_if(virtual_node.source_spans, [](SourceSpan const &span) {
            return span.file_path.empty() || span.begin > span.end;
        });
        for (auto &span : virtual_node.source_spans) {
            span.file_path = normalized_path_string(span.file_path);
            graph_index.dependency_file_paths.insert(span.file_path);
        }
        sort_and_deduplicate_spans(virtual_node.source_spans);
        auto normalize_port_spans = [&](auto &ports) {
            for (auto &port : ports) {
                std::erase_if(port.source_spans, [](SourceSpan const &span) {
                    return span.file_path.empty() || span.begin > span.end;
                });
                for (auto &span : port.source_spans) {
                    span.file_path = normalized_path_string(span.file_path);
                    graph_index.dependency_file_paths.insert(span.file_path);
                }
                sort_and_deduplicate_spans(port.source_spans);
            }
        };
        normalize_port_spans(virtual_node.sample_inputs);
        normalize_port_spans(virtual_node.sample_outputs);
        normalize_port_spans(virtual_node.event_inputs);
        normalize_port_spans(virtual_node.event_outputs);
    }
    for (size_t i = 0; i < graph_index.virtual_nodes.size(); ++i) {
        graph_index.virtual_node_index_by_id.emplace(graph_index.virtual_nodes[i].id, i);
    }

    return graph_index;
}

std::string runtime_node_id(
    std::string_view instance_id,
    std::string_view virtual_node_id)
{
    std::string value(instance_id);
    value += runtime_node_id_separator;
    value += virtual_node_id;
    return value;
}

struct ResolvedRuntimeNodeId {
    std::string instance_id;
    std::string virtual_node_id;
};

std::optional<ResolvedRuntimeNodeId> parse_runtime_node_id(std::string_view runtime_id)
{
    auto const separator = runtime_id.find(runtime_node_id_separator);
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    return ResolvedRuntimeNodeId{
        .instance_id = std::string(runtime_id.substr(0, separator)),
        .virtual_node_id = std::string(runtime_id.substr(separator + runtime_node_id_separator.size())),
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

VirtualNodeInfo IvModuleSourceIntrospection::to_virtual_node(
    IntrospectionVirtualNode const &node,
    std::string const &instance_id) const
{
    VirtualNodeInfo live;
    live.id = runtime_node_id(instance_id, node.id);
    live.instance_id = instance_id;
    live.kind = node.kind;
    live.source_identity = node.source_identity;
    live.type_identity = node.type_identity;
    live.sample_inputs = to_live_ports(node.sample_inputs);
    for (auto &port : live.sample_inputs) port.state_value = "default";
    live.sample_outputs = to_live_ports(node.sample_outputs);
    for (auto &port : live.sample_outputs) port.state_value = "disconnected";
    live.event_inputs = to_live_ports(node.event_inputs);
    for (auto &port : live.event_inputs) port.state_value = "default";
    live.event_outputs = to_live_ports(node.event_outputs);
    for (auto &port : live.event_outputs) port.state_value = "disconnected";
    live.member_count = node.backing_node_ids.size();
    live.members.reserve(node.members.size());
    for (auto const &member : node.members) {
        VirtualNodeMemberInfo live_member;
        live_member.ordinal = member.ordinal;
        live_member.backing_node_id = member.backing_node_id;
        live_member.kind = member.kind;
        live_member.type_identity = member.type_identity;
        live_member.sample_inputs = to_live_ports(member.sample_inputs);
        for (auto &port : live_member.sample_inputs) {
            port.state_value = port.connectivity == VirtualPortConnectivity::connected
                ? "disconnected"
                : "virtualFollow";
        }
        live_member.sample_outputs = to_live_ports(member.sample_outputs);
        for (auto &port : live_member.sample_outputs) port.state_value = "disconnected";
        live_member.event_inputs = to_live_ports(member.event_inputs);
        for (auto &port : live_member.event_inputs) {
            port.state_value = port.connectivity == VirtualPortConnectivity::connected
                ? "disconnected"
                : "virtualFollow";
        }
        live_member.event_outputs = to_live_ports(member.event_outputs);
        for (auto &port : live_member.event_outputs) port.state_value = "disconnected";
        live.members.push_back(std::move(live_member));
    }
    live.source_spans.reserve(node.source_spans.size());
    for (auto const &span : node.source_spans) live.source_spans.push_back(to_live_span(span));
    return live;
}

void IvModuleSourceIntrospection::handle_iv_package_definitions_changed(
    IvPackageDefinitionsChanged const &package_diff)
{
    std::vector<std::string> replace_instance_ids;
    std::vector<IvModuleInstanceInfo> updated_instances;

    {
        std::scoped_lock lock(mutex);
        std::unordered_set<std::string> changed_definition_ids;

        auto apply_definition = [&](IvModuleDefinition const &definition) {
            changed_definition_ids.insert(definition.definition_id);
            invalidate_source_texts(definition.dependencies);
            graph_indexes_by_definition_id[definition.definition_id] =
                build_graph_introspection_index(
                    definition.definition_id,
                    definition.introspection,
                    definition.package_root,
                    definition.module_id,
                    definition.dependencies);
        };

        for (auto const &definition : package_diff.modules.created) {
            apply_definition(definition);
        }
        for (auto const &definition : package_diff.modules.updated) {
            apply_definition(definition);
        }
        for (auto const &definition_id : package_diff.modules.deleted_definition_ids) {
            changed_definition_ids.insert(definition_id);
            if (auto index = graph_indexes_by_definition_id.find(definition_id);
                index != graph_indexes_by_definition_id.end()) {
                for (auto const &path : index->second.dependency_file_paths) {
                    invalidate_source_text(path);
                }
                graph_indexes_by_definition_id.erase(index);
            }
        }

        for (auto const &[instance_id, instance] : instances_by_id) {
            if (!changed_definition_ids.contains(instance.definition_id)) {
                continue;
            }
            replace_instance_ids.push_back(instance_id);
            if (graph_indexes_by_definition_id.contains(instance.definition_id)) {
                updated_instances.push_back(instance);
            }
        }
    }

    if (replace_instance_ids.empty()) {
        return;
    }

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_source_introspection_nodes_updated_event,
        ProjectVirtualNodesNotification{
            .nodes = get_virtual_nodes_for_instances(updated_instances),
            .replace_instance_ids = std::move(replace_instance_ids),
        });
}

void IvModuleSourceIntrospection::handle_iv_module_instance_declarations_changed(
    std::vector<IvModuleInstanceInfo> const &instances)
{
    std::vector<std::string> replace_instance_ids;
    std::vector<IvModuleInstanceInfo> updated_instances;

    {
        std::scoped_lock lock(mutex);
        std::unordered_map<std::string, IvModuleInstanceInfo> next_instances;
        for (auto const &instance : instances) {
            if (instance.instance_id.empty() || instance.definition_id.empty()) {
                continue;
            }
            next_instances.emplace(instance.instance_id, instance);
        }

        for (auto const &[instance_id, previous] : instances_by_id) {
            auto const next = next_instances.find(instance_id);
            if (next == next_instances.end()
                || next->second.definition_id != previous.definition_id) {
                replace_instance_ids.push_back(instance_id);
            }
        }
        for (auto const &[instance_id, instance] : next_instances) {
            auto const previous = instances_by_id.find(instance_id);
            if (previous == instances_by_id.end()
                || previous->second.definition_id != instance.definition_id) {
                replace_instance_ids.push_back(instance_id);
                updated_instances.push_back(instance);
            }
        }

        instances_by_id = std::move(next_instances);
    }

    std::ranges::sort(replace_instance_ids);
    replace_instance_ids.erase(
        std::unique(replace_instance_ids.begin(), replace_instance_ids.end()),
        replace_instance_ids.end());
    if (replace_instance_ids.empty()) {
        return;
    }

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_source_introspection_nodes_updated_event,
        ProjectVirtualNodesNotification{
            .nodes = get_virtual_nodes_for_instances(updated_instances),
            .replace_instance_ids = std::move(replace_instance_ids),
        });
}

ProjectQueryResult IvModuleSourceIntrospection::query_by_spans(
    std::filesystem::path const &file_path,
    std::vector<SourceRange> const &ranges,
    SourceRangeMatchMode match_mode,
    std::optional<std::string> instance_id) const
{
    std::scoped_lock lock(mutex);
    if (graph_indexes_by_definition_id.empty() || instances_by_id.empty()) {
        return {};
    }

    std::string const normalized_file_path = normalized_path_string(file_path);
    std::vector<std::pair<uint32_t, uint32_t>> requested_ranges;
    requested_ranges.reserve(ranges.size());
    for (auto const &range : ranges) {
        requested_ranges.push_back(byte_range_for(normalized_file_path, range));
    }

    ProjectQueryResult result;

    auto byte_span_touches_range =
        [](uint32_t span_begin,
           uint32_t span_end,
           std::pair<uint32_t, uint32_t> const &requested_range) {
            if (span_begin > span_end) return false;
            auto const [begin, end] = requested_range;
            // Query ranges are intentionally inclusive at both boundaries.
            // In particular, a cursor positioned at span.end still selects
            // the highlighted source span.
            if (begin == end) {
                return span_begin <= begin && begin <= span_end;
            }
            return span_begin <= end && begin <= span_end;
        };

    auto span_touches_range =
        [&](SourceSpan const &span,
            std::pair<uint32_t, uint32_t> const &requested_range) {
            return byte_span_touches_range(span.begin, span.end, requested_range);
        };

    auto span_distance_to_range =
        [&](SourceSpan const &span,
            std::pair<uint32_t, uint32_t> const &requested_range) {
            auto const [begin, end] = requested_range;
            if (byte_span_touches_range(span.begin, span.end, requested_range)) {
                return 0u;
            }
            if (span.end <= begin) {
                return begin - span.end;
            }
            return span.begin - end;
        };

    struct RankedRuntimeVirtualNode {
        std::string definition_id;
        size_t virtual_index = 0;
        bool full_node = false;
        std::vector<size_t> sample_input_ordinals {};
        std::vector<size_t> event_input_ordinals {};
        std::vector<SourceSpan> selected_port_spans {};
        uint32_t best_span_size = std::numeric_limits<uint32_t>::max();
        uint32_t best_distance = std::numeric_limits<uint32_t>::max();
        uint32_t best_begin = std::numeric_limits<uint32_t>::max();
        uint32_t best_end = std::numeric_limits<uint32_t>::max();
    };

    auto record_rank = [&](RankedRuntimeVirtualNode &ranked,
                           SourceSpan const &span,
                           std::pair<uint32_t, uint32_t> const &requested_range) {
        auto const span_size = span.end >= span.begin ? span.end - span.begin : 0u;
        auto const distance = span_distance_to_range(span, requested_range);
        ranked.best_span_size = std::min(ranked.best_span_size, span_size);
        ranked.best_distance = std::min(ranked.best_distance, distance);
        ranked.best_begin = std::min(ranked.best_begin, span.begin);
        ranked.best_end = std::min(ranked.best_end, span.end);
    };

    std::vector<RankedRuntimeVirtualNode> ranked_nodes;
    for (auto const &[definition_id, graph_index] : graph_indexes_by_definition_id) {
        for (size_t virtual_index = 0; virtual_index < graph_index.virtual_nodes.size();
             ++virtual_index) {
            auto const &node = graph_index.virtual_nodes[virtual_index];
            RankedRuntimeVirtualNode ranked{
                .definition_id = definition_id,
                .virtual_index = virtual_index,
            };

            if (requested_ranges.empty()) {
                ranked.full_node = true;
                if (!node.source_spans.empty()) {
                    auto const &span = node.source_spans.front();
                    ranked.best_span_size = span.end >= span.begin
                        ? span.end - span.begin : 0u;
                    ranked.best_distance = 0u;
                    ranked.best_begin = span.begin;
                    ranked.best_end = span.end;
                }
                ranked_nodes.push_back(std::move(ranked));
                continue;
            }

            std::vector<bool> matched_ranges(requested_ranges.size(), false);
            auto inspect_spans = [&](std::span<SourceSpan const> spans,
                                     auto &&on_match) {
                for (auto const &span : spans) {
                    if (span.file_path != normalized_file_path) continue;
                    bool matched_span = false;
                    for (size_t range_i = 0; range_i < requested_ranges.size(); ++range_i) {
                        auto const &requested_range = requested_ranges[range_i];
                        if (!span_touches_range(span, requested_range)) continue;
                        matched_ranges[range_i] = true;
                        matched_span = true;
                        record_rank(ranked, span, requested_range);
                    }
                    if (matched_span) on_match(span);
                }
            };

            inspect_spans(node.source_spans, [&](SourceSpan const &) {
                ranked.full_node = true;
            });

            auto collect_port_matches = [&](auto const &ports, auto &ordinals) {
                for (auto const &port : ports) {
                    bool matched_port = false;
                    inspect_spans(port.source_spans, [&](SourceSpan const &span) {
                        matched_port = true;
                        ranked.selected_port_spans.push_back(span);
                    });
                    if (matched_port) ordinals.push_back(port.ordinal);
                }
            };
            collect_port_matches(node.sample_inputs, ranked.sample_input_ordinals);
            collect_port_matches(node.event_inputs, ranked.event_input_ordinals);

            auto const is_matched = [](bool value) { return value; };
            auto const matches = match_mode == SourceRangeMatchMode::union_
                ? std::ranges::any_of(matched_ranges, is_matched)
                : std::ranges::all_of(matched_ranges, is_matched);
            if (!matches) continue;

            sort_and_deduplicate_spans(ranked.selected_port_spans);
            ranked_nodes.push_back(std::move(ranked));
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
        auto const &a_node = a_index.virtual_nodes[a.virtual_index];
        auto const &b_node = b_index.virtual_nodes[b.virtual_index];
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
        auto const &node = graph_index.virtual_nodes[ranked.virtual_index];
        std::vector<std::string> matching_instance_ids;
        for (auto const &[candidate_instance_id, instance] : instances_by_id) {
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
            auto live = to_virtual_node(node, matching_instance_id);
            if (!ranked.full_node) {
                auto keep_ordinal = [](auto const &ordinals, auto const &port) {
                    return std::ranges::contains(ordinals, port.ordinal);
                };
                std::erase_if(live.sample_inputs, [&](auto const &port) {
                    return !keep_ordinal(ranked.sample_input_ordinals, port);
                });
                std::erase_if(live.event_inputs, [&](auto const &port) {
                    return !keep_ordinal(ranked.event_input_ordinals, port);
                });
                live.sample_outputs.clear();
                live.event_outputs.clear();
                for (auto &member : live.members) {
                    std::erase_if(member.sample_inputs, [&](auto const &port) {
                        return !keep_ordinal(ranked.sample_input_ordinals, port);
                    });
                    std::erase_if(member.event_inputs, [&](auto const &port) {
                        return !keep_ordinal(ranked.event_input_ordinals, port);
                    });
                    member.sample_outputs.clear();
                    member.event_outputs.clear();
                }
                live.source_spans.clear();
                live.source_spans.reserve(ranked.selected_port_spans.size());
                for (auto const &span : ranked.selected_port_spans) {
                    live.source_spans.push_back(to_live_span(span));
                }
            }
            result.nodes.push_back(std::move(live));
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
        for (auto const &node : graph_index.virtual_nodes) {
            auto append_spans = [&](std::span<SourceSpan const> spans) {
              for (auto const &span : spans) {
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
            };
            append_spans(node.source_spans);
            for (auto const &port : node.sample_inputs) {
                append_spans(port.source_spans);
            }
            for (auto const &port : node.event_inputs) {
                append_spans(port.source_spans);
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

VirtualNodeInfo IvModuleSourceIntrospection::get_virtual_node(std::string const &node_id) const
{
    std::scoped_lock lock(mutex);
    auto const resolved = parse_runtime_node_id(node_id);
    if (!resolved.has_value()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    auto const instance_it = instances_by_id.find(resolved->instance_id);
    if (instance_it == instances_by_id.end()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    auto const index_it = graph_indexes_by_definition_id.find(instance_it->second.definition_id);
    if (index_it == graph_indexes_by_definition_id.end()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    auto const virtual_it = index_it->second.virtual_node_index_by_id.find(resolved->virtual_node_id);
    if (virtual_it == index_it->second.virtual_node_index_by_id.end()) {
        throw std::runtime_error("unknown node id: " + node_id);
    }
    return to_virtual_node(
        index_it->second.virtual_nodes[virtual_it->second],
        resolved->instance_id);
}

std::vector<VirtualNodeInfo> IvModuleSourceIntrospection::get_virtual_nodes(
    std::vector<std::string> const &node_ids) const
{
    std::scoped_lock lock(mutex);
    std::vector<VirtualNodeInfo> nodes;
    nodes.reserve(node_ids.size());
    for (auto const &node_id : node_ids) {
        auto const resolved = parse_runtime_node_id(node_id);
        if (!resolved.has_value()) {
            throw std::runtime_error("unknown node id: " + node_id);
        }
        auto const instance_it = instances_by_id.find(resolved->instance_id);
        if (instance_it == instances_by_id.end()) {
            throw std::runtime_error("unknown node id: " + node_id);
        }
        auto const index_it = graph_indexes_by_definition_id.find(instance_it->second.definition_id);
        if (index_it == graph_indexes_by_definition_id.end()) {
            throw std::runtime_error("unknown node id: " + node_id);
        }
        auto const virtual_it = index_it->second.virtual_node_index_by_id.find(resolved->virtual_node_id);
        if (virtual_it == index_it->second.virtual_node_index_by_id.end()) {
            throw std::runtime_error("unknown node id: " + node_id);
        }
        nodes.push_back(to_virtual_node(
            index_it->second.virtual_nodes[virtual_it->second],
            resolved->instance_id));
    }
    return nodes;
}

std::vector<VirtualNodeInfo> IvModuleSourceIntrospection::get_virtual_nodes_for_instances(
    std::vector<IvModuleInstanceInfo> const &instances) const
{
    std::scoped_lock lock(mutex);
    std::vector<VirtualNodeInfo> nodes;
    for (auto const &instance : instances) {
        if (instance.instance_id.empty() || instance.definition_id.empty()) {
            continue;
        }
        auto const index_it = graph_indexes_by_definition_id.find(instance.definition_id);
        if (index_it == graph_indexes_by_definition_id.end()) {
            continue;
        }
        for (auto const &virtual_node : index_it->second.virtual_nodes) {
            nodes.push_back(to_virtual_node(virtual_node, instance.instance_id));
        }
    }
    return nodes;
}

void IvModuleSourceIntrospection::handle_iv_module_instances_source_file_filter(
    std::filesystem::path const &source_file_path,
    std::vector<IvModuleInstanceInfo> const &instances,
    IvModuleInstancesSourceFileFilterBuilder &builder) const
{
    auto matching_instances = instances;
    std::erase_if(matching_instances, [&](IvModuleInstanceInfo const &instance) {
        return !definition_uses_source_file(
            instance.definition_id,
            source_file_path);
    });
    builder.succeed(std::move(matching_instances));
}

void IvModuleSourceIntrospection::handle_socket_rpc_graph_query_by_spans(
    GraphQueryBySpansRequest const &request,
    SocketRpcGraphQueryResultBuilder &builder) const
{
    builder.succeed(query_by_spans(
        request.file_path,
        request.ranges,
        request.match_mode,
        request.instance_id));
}

void IvModuleSourceIntrospection::handle_socket_rpc_graph_query_active_regions(
    GraphQueryActiveRegionsRequest const &request,
    SocketRpcRegionQueryResultBuilder &builder) const
{
    builder.succeed(query_active_regions(request.file_path));
}

void IvModuleSourceIntrospection::handle_socket_rpc_get_virtual_node(
    GetVirtualNodeRequest const &request,
    SocketRpcVirtualNodeResultBuilder &builder) const
{
    builder.succeed(get_virtual_node(request.node_id));
}

void IvModuleSourceIntrospection::handle_socket_rpc_get_virtual_nodes(
    GetVirtualNodesRequest const &request,
    SocketRpcVirtualNodesResultBuilder &builder) const
{
    builder.succeed(get_virtual_nodes(request.node_ids));
}


} // namespace iv
