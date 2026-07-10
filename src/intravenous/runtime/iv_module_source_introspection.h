#pragma once

#include <intravenous/module/dependency.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/runtime_project_api_types.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
struct SourceTextLineMap {
    std::string text;
    std::vector<size_t> line_offsets;

    static SourceTextLineMap from_file(std::filesystem::path const &path);
    size_t offset_for(SourcePosition position) const;
    SourcePosition position_for(size_t offset) const;
};

struct LoadedGraphIntrospectionIndex {
    std::string definition_id;
    std::filesystem::path module_root;
    std::string module_id;
    std::vector<IntrospectionLogicalNode> logical_nodes;
    std::unordered_map<std::string, size_t> logical_node_index_by_id;
};

class IvModuleSourceIntrospection {
    mutable std::mutex mutex;
    mutable std::unordered_map<std::string, SourceTextLineMap> source_text_cache;
    std::unordered_map<std::string, LoadedGraphIntrospectionIndex> graph_indexes_by_definition_id;
    std::unordered_map<std::string, IvModuleInstanceInfo> realized_instances_by_id;

    SourceTextLineMap const &source_text_for(std::string const &normalized_path) const;
    void invalidate_source_text(std::string const &normalized_path);
    void invalidate_source_texts(std::span<ModuleDependency const> dependencies);
    std::pair<uint32_t, uint32_t>
    byte_range_for(std::string const &normalized_path, SourceRange const &range) const;
    LiveSourceSpan to_live_span(SourceSpan const &span) const;
    LogicalNodeInfo to_logical_node(
        IntrospectionLogicalNode const &node,
        std::string const &instance_id) const;

public:
    IvModuleSourceIntrospection() = default;

    void handle_iv_module_definitions_changed(
        IvModuleDefinitionsChanged const &diff);
    void handle_iv_module_instances_list_changed(
        std::vector<IvModuleInstanceInfo> const &instances);
    ProjectQueryResult
    query_by_spans(
        std::filesystem::path const &file_path,
        std::vector<SourceRange> const &ranges,
        SourceRangeMatchMode match_mode = SourceRangeMatchMode::intersection,
        std::optional<std::string> instance_id = std::nullopt) const;
    ProjectRegionQueryResult
    query_active_regions(std::filesystem::path const &file_path) const;
    LogicalNodeInfo get_logical_node(std::string const &node_id) const;
    std::vector<LogicalNodeInfo>
    get_logical_nodes(std::vector<std::string> const &node_ids) const;
    std::vector<LogicalNodeInfo>
    get_logical_nodes_for_instances(std::vector<IvModuleInstanceInfo> const &instances) const;
    GraphInputPortDescriptor sample_graph_input_port_for_node(
        std::string const &node_id,
        std::optional<size_t> concrete_member_ordinal,
        size_t input_ordinal) const;
};
} // namespace iv
