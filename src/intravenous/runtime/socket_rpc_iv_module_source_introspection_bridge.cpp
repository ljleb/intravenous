#include "runtime/socket_rpc_iv_module_source_introspection_bridge.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/iv_module_source_introspection.h"
#include "runtime/socket_rpc_server.h"

namespace iv {
namespace {
IvModuleSourceIntrospection *bound_introspection = nullptr;
GraphInputLanes *bound_graph_input_lanes = nullptr;
std::function<void()> *bound_shutdown = nullptr;

void handle_graph_query_by_spans(
    GraphQueryBySpansRequest const &request,
    SocketRpcGraphQueryResultBuilder &builder)
{
    if (bound_introspection == nullptr) {
        return;
    }
    builder.succeed(bound_introspection->query_by_spans(
        request.file_path,
        request.ranges,
        request.match_mode));
}

void handle_graph_query_active_regions(
    GraphQueryActiveRegionsRequest const &request,
    SocketRpcRegionQueryResultBuilder &builder)
{
    if (bound_introspection == nullptr) {
        return;
    }
    builder.succeed(bound_introspection->query_active_regions(request.file_path));
}

void handle_get_logical_node(
    GetLogicalNodeRequest const &request,
    SocketRpcLogicalNodeResultBuilder &builder)
{
    if (bound_introspection == nullptr) {
        return;
    }
    builder.succeed(bound_introspection->get_logical_node(request.node_id));
}

void handle_get_logical_nodes(
    GetLogicalNodesRequest const &request,
    SocketRpcLogicalNodesResultBuilder &builder)
{
    if (bound_introspection == nullptr) {
        return;
    }
    builder.succeed(bound_introspection->get_logical_nodes(request.node_ids));
}

void handle_set_sample_input_value(
    SetSampleInputValueRequest const &request,
    SocketRpcAckResponseBuilder &)
{
    if (bound_graph_input_lanes == nullptr) {
        return;
    }
    bound_graph_input_lanes->set_sample_input_value(
        ProjectSetSampleInputValueRequest{
            .node_id = request.node_id,
            .member_ordinal = request.member_ordinal,
            .input_ordinal = request.input_ordinal,
            .value = request.value,
        });
}

void handle_clear_sample_input_value_override(
    ClearSampleInputValueOverrideRequest const &request,
    SocketRpcAckResponseBuilder &)
{
    if (bound_graph_input_lanes == nullptr) {
        return;
    }
    bound_graph_input_lanes->clear_sample_input_value_override(
        ProjectClearSampleInputValueOverrideRequest{
            .node_id = request.node_id,
            .member_ordinal = request.member_ordinal,
            .input_ordinal = request.input_ordinal,
        });
}

void handle_server_shutdown(
    ServerShutdownRequest const &,
    SocketRpcAckResponseBuilder &)
{
    if (bound_shutdown != nullptr) {
        (*bound_shutdown)();
    }
}

IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcGraphQueryBySpansEvent,
    iv_socket_rpc_graph_query_by_spans_event,
    handle_graph_query_by_spans);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcGraphQueryActiveRegionsEvent,
    iv_socket_rpc_graph_query_active_regions_event,
    handle_graph_query_active_regions);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcGetLogicalNodeEvent,
    iv_socket_rpc_get_logical_node_event,
    handle_get_logical_node);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcGetLogicalNodesEvent,
    iv_socket_rpc_get_logical_nodes_event,
    handle_get_logical_nodes);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetSampleInputValueEvent,
    iv_socket_rpc_set_sample_input_value_event,
    handle_set_sample_input_value);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcClearSampleInputValueOverrideEvent,
    iv_socket_rpc_clear_sample_input_value_override_event,
    handle_clear_sample_input_value_override);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcServerShutdownEvent,
    iv_socket_rpc_server_shutdown_event,
    handle_server_shutdown);
} // namespace

void bind_socket_rpc_iv_module_source_introspection_bridge(
    IvModuleSourceIntrospection &introspection,
    GraphInputLanes &graph_input_lanes,
    std::function<void()> *shutdown_callback)
{
    bound_introspection = &introspection;
    bound_graph_input_lanes = &graph_input_lanes;
    bound_shutdown = shutdown_callback;
}

void unbind_socket_rpc_iv_module_source_introspection_bridge(
    IvModuleSourceIntrospection const &introspection,
    GraphInputLanes const &graph_input_lanes)
{
    if (bound_introspection == &introspection) {
        bound_introspection = nullptr;
    }
    if (bound_graph_input_lanes == &graph_input_lanes) {
        bound_graph_input_lanes = nullptr;
    }
    bound_shutdown = nullptr;
}
} // namespace iv
