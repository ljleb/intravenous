#pragma once

#include <intravenous/module/dependency.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/runtime_project_api_types.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
class IvModuleInstancesSourceFileFilterBuilder;
class SocketRpcGraphQueryResultBuilder;
class SocketRpcRegionQueryResultBuilder;
class SocketRpcVirtualNodeResultBuilder;
class SocketRpcVirtualNodesResultBuilder;
struct GraphQueryBySpansRequest;
struct GraphQueryActiveRegionsRequest;
struct GetVirtualNodeRequest;
struct GetVirtualNodesRequest;

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
    std::vector<IntrospectionVirtualNode> virtual_nodes;
    std::unordered_map<std::string, size_t> virtual_node_index_by_id;
    std::unordered_set<std::string> dependency_file_paths;
};

class IvModuleSourceIntrospection {
    mutable std::mutex mutex;
    mutable std::unordered_map<std::string, SourceTextLineMap> source_text_cache;
    std::unordered_map<std::string, LoadedGraphIntrospectionIndex> graph_indexes_by_definition_id;
    std::unordered_map<std::string, IvModuleInstanceInfo> instances_by_id;

    SourceTextLineMap const &source_text_for(std::string const &normalized_path) const;
    void invalidate_source_text(std::string const &normalized_path);
    void invalidate_source_texts(std::span<ModuleDependency const> dependencies);
    std::pair<uint32_t, uint32_t>
    byte_range_for(std::string const &normalized_path, SourceRange const &range) const;
    LiveSourceSpan to_live_span(SourceSpan const &span) const;
    VirtualNodeInfo to_virtual_node(
        IntrospectionVirtualNode const &node,
        std::string const &instance_id) const;

public:
    IvModuleSourceIntrospection() = default;

    void handle_iv_package_definitions_changed(
        IvPackageDefinitionsChanged const &diff);
    void handle_iv_module_instance_declarations_changed(
        std::vector<IvModuleInstanceInfo> const &instances);
    ProjectQueryResult
    query_by_spans(
        std::filesystem::path const &file_path,
        std::vector<SourceRange> const &ranges,
        SourceRangeMatchMode match_mode = SourceRangeMatchMode::intersection,
        std::optional<std::string> instance_id = std::nullopt) const;
    ProjectRegionQueryResult
    query_active_regions(std::filesystem::path const &file_path) const;
    [[nodiscard]] bool definition_uses_source_file(
        std::string const &definition_id,
        std::filesystem::path const &file_path) const;
    VirtualNodeInfo get_virtual_node(std::string const &node_id) const;
    std::vector<VirtualNodeInfo>
    get_virtual_nodes(std::vector<std::string> const &node_ids) const;
    std::vector<VirtualNodeInfo>
    get_virtual_nodes_for_instances(std::vector<IvModuleInstanceInfo> const &instances) const;
    void handle_iv_module_instances_source_file_filter(
        std::filesystem::path const &source_file_path,
        std::vector<IvModuleInstanceInfo> const &instances,
        IvModuleInstancesSourceFileFilterBuilder &builder) const;
    void handle_socket_rpc_graph_query_by_spans(
        GraphQueryBySpansRequest const &request,
        SocketRpcGraphQueryResultBuilder &builder) const;
    void handle_socket_rpc_graph_query_active_regions(
        GraphQueryActiveRegionsRequest const &request,
        SocketRpcRegionQueryResultBuilder &builder) const;
    void handle_socket_rpc_get_virtual_node(
        GetVirtualNodeRequest const &request,
        SocketRpcVirtualNodeResultBuilder &builder) const;
    void handle_socket_rpc_get_virtual_nodes(
        GetVirtualNodesRequest const &request,
        SocketRpcVirtualNodesResultBuilder &builder) const;
};
} // namespace iv
