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
#include <unordered_set>
#include <vector>

namespace iv {
class IvModuleInstancesSourceFileFilterBuilder;
class SocketRpcAckResponseBuilder;
class SocketRpcGraphQueryResultBuilder;
class SocketRpcRegionQueryResultBuilder;
class SocketRpcVirtualNodeResultBuilder;
class SocketRpcVirtualNodesResultBuilder;
struct IvModuleInstanceBuildersChanged;
struct GraphQueryBySpansRequest;
struct GraphQueryActiveRegionsRequest;
struct GetVirtualNodeRequest;
struct GetVirtualNodesRequest;
struct SetSampleInputValueRequest;
struct SetSampleInputStateRequest;
struct SetEventInputStateRequest;
struct SetSampleOutputStateRequest;
struct SetEventOutputStateRequest;

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
    std::unordered_map<std::string, IvModuleInstanceInfo> realized_instances_by_id;
    std::unordered_map<std::string, std::vector<PublicSampleInputInfo>> public_inputs_by_instance_id;
    std::unordered_map<std::string, std::vector<PublicEventInputInfo>> public_event_inputs_by_instance_id;
    std::unordered_map<std::string, std::vector<PublicSampleOutputInfo>> public_outputs_by_instance_id;
    std::unordered_map<std::string, std::vector<PublicEventOutputInfo>> public_event_outputs_by_instance_id;

    SourceTextLineMap const &source_text_for(std::string const &normalized_path) const;
    void invalidate_source_text(std::string const &normalized_path);
    void invalidate_source_texts(std::span<ModuleDependency const> dependencies);
    std::pair<uint32_t, uint32_t>
    byte_range_for(std::string const &normalized_path, SourceRange const &range) const;
    LiveSourceSpan to_live_span(SourceSpan const &span) const;
    VirtualNodeInfo to_virtual_node(
        IntrospectionVirtualNode const &node,
        std::string const &instance_id) const;
    VirtualNodeInfo to_public_sample_input(PublicSampleInputInfo const &input) const;
    VirtualNodeInfo to_public_event_input(PublicEventInputInfo const &input) const;
    VirtualNodeInfo to_public_sample_output(PublicSampleOutputInfo const &output) const;
    VirtualNodeInfo to_public_event_output(PublicEventOutputInfo const &output) const;

public:
    IvModuleSourceIntrospection() = default;

    void handle_iv_module_definitions_changed(
        IvModuleDefinitionsChanged const &diff);
    void handle_iv_module_instances_list_changed(
        std::vector<IvModuleInstanceInfo> const &instances);
    void handle_iv_module_instance_builders_completed(
        IvModuleInstanceBuildersChanged const &diff);
    void set_public_sample_inputs(std::vector<PublicSampleInputInfo> inputs);
    void set_public_event_inputs(std::vector<PublicEventInputInfo> inputs);
    void set_public_sample_outputs(std::vector<PublicSampleOutputInfo> outputs);
    void set_public_event_outputs(std::vector<PublicEventOutputInfo> outputs);
    void replace_public_input_instances(std::span<std::string const> instance_ids);
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
    GraphInputPortDescriptor sample_graph_input_port_for_node(
        std::string const &node_id,
        std::optional<size_t> concrete_member_ordinal,
        size_t input_ordinal) const;
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
    void handle_socket_rpc_set_sample_input_value(
        SetSampleInputValueRequest const &request,
        SocketRpcAckResponseBuilder &builder);
    void handle_socket_rpc_set_sample_input_state(
        SetSampleInputStateRequest const &request,
        SocketRpcAckResponseBuilder &builder);
    void handle_socket_rpc_set_event_input_state(
        SetEventInputStateRequest const &request,
        SocketRpcAckResponseBuilder &builder);
    void handle_socket_rpc_set_sample_output_state(
        SetSampleOutputStateRequest const &request,
        SocketRpcAckResponseBuilder &builder);
    void handle_socket_rpc_set_event_output_state(
        SetEventOutputStateRequest const &request,
        SocketRpcAckResponseBuilder &builder);
};
} // namespace iv
