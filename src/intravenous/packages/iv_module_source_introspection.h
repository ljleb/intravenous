#pragma once

#include <intravenous/module/dependency.h>
#include <intravenous/packages/node_definitions.h>
#include <intravenous/project/iv_module_instance_types.h>
#include <intravenous/project/runtime_project_api_types.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
class IvModuleInstancesSourceFileFilterBuilder;
struct IvModuleSourceIntrospectionProjectGraphRequest;
class SocketRpcGraphQueryResultBuilder;
class SocketRpcRegionQueryResultBuilder;
class SocketRpcSymbolicNodeResultBuilder;
class SocketRpcSymbolicNodesResultBuilder;
struct GraphQueryBySpansRequest;
struct GraphQueryActiveRegionsRequest;
struct GetSymbolicNodeRequest;
struct GetSymbolicNodesRequest;

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
    std::vector<IntrospectionSymbolicNode> symbolic_nodes;
    std::unordered_map<std::string, size_t> symbolic_node_index_by_id;
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
    SymbolicNodeInfo to_symbolic_node(
        IntrospectionSymbolicNode const &node,
        std::string const &instance_id) const;

public:
    IvModuleSourceIntrospection() = default;

    // Applies one ProjectGraph root build's definition and instance-declaration
    // changes and returns the symbolic-node replacements they cause.
    void handle_project_graph_source_introspection(
        IvModuleSourceIntrospectionProjectGraphRequest &request);
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
    SymbolicNodeInfo get_symbolic_node(std::string const &node_id) const;
    std::vector<SymbolicNodeInfo>
    get_symbolic_nodes(std::vector<std::string> const &node_ids) const;
    std::vector<SymbolicNodeInfo>
    get_symbolic_nodes_for_instances(std::vector<IvModuleInstanceInfo> const &instances) const;
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
    void handle_socket_rpc_get_symbolic_node(
        GetSymbolicNodeRequest const &request,
        SocketRpcSymbolicNodeResultBuilder &builder) const;
    void handle_socket_rpc_get_symbolic_nodes(
        GetSymbolicNodesRequest const &request,
        SocketRpcSymbolicNodesResultBuilder &builder) const;
};
} // namespace iv
