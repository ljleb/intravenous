#include "runtime/socket_rpc_runtime_project_bridge.h"

#include "runtime/project_service.h"
#include "runtime/socket_rpc_server.h"

namespace iv {
namespace {
    RuntimeProjectService *bound_service = nullptr;

    RuntimeProjectService *bound_service_or_null() {
        return bound_service;
    }

    void handle_graph_query_by_spans(
        GraphQueryBySpansRequest const &request,
        SocketRpcGraphQueryResultBuilder &builder)
    {
        auto *service = bound_service_or_null();
        if (service == nullptr) {
            return;
        }
        builder.succeed(service->query_by_spans(
            request.file_path,
            request.ranges,
            request.match_mode));
    }

    void handle_graph_query_active_regions(
        GraphQueryActiveRegionsRequest const &request,
        SocketRpcRegionQueryResultBuilder &builder)
    {
        auto *service = bound_service_or_null();
        if (service == nullptr) {
            return;
        }
        builder.succeed(service->query_active_regions(request.file_path));
    }

    void handle_get_logical_node(
        GetLogicalNodeRequest const &request,
        SocketRpcLogicalNodeResultBuilder &builder)
    {
        auto *service = bound_service_or_null();
        if (service == nullptr) {
            return;
        }
        builder.succeed(service->get_logical_node(request.node_id));
    }

    void handle_get_logical_nodes(
        GetLogicalNodesRequest const &request,
        SocketRpcLogicalNodesResultBuilder &builder)
    {
        auto *service = bound_service_or_null();
        if (service == nullptr) {
            return;
        }
        builder.succeed(service->get_logical_nodes(request.node_ids));
    }

    void handle_set_sample_input_value(
        SetSampleInputValueRequest const &request,
        SocketRpcAckResponseBuilder &)
    {
        auto *service = bound_service_or_null();
        if (service == nullptr) {
            return;
        }
        service->set_sample_input_value(
            request.node_id,
            request.input_ordinal,
            request.value,
            request.member_ordinal);
    }

    void handle_clear_sample_input_value_override(
        ClearSampleInputValueOverrideRequest const &request,
        SocketRpcAckResponseBuilder &)
    {
        auto *service = bound_service_or_null();
        if (service == nullptr) {
            return;
        }
        service->clear_sample_input_value_override(
            request.node_id,
            request.member_ordinal,
            request.input_ordinal);
    }

    void handle_server_shutdown(
        ServerShutdownRequest const &,
        SocketRpcAckResponseBuilder &)
    {
        auto *service = bound_service_or_null();
        if (service == nullptr) {
            return;
        }
        service->request_shutdown();
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

void bind_socket_rpc_runtime_project_bridge(RuntimeProjectService &service) {
    bound_service = &service;
}

void unbind_socket_rpc_runtime_project_bridge(RuntimeProjectService const &service) {
    if (bound_service == &service) {
        bound_service = nullptr;
    }
}
} // namespace iv
