#include <intravenous/runtime/socket_rpc_notification_bridge.h>

#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/lane_views_events.h>
#include <intravenous/runtime/lanes_visualization_events.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <type_traits>
#include <variant>

namespace iv {
namespace {
    SocketRpcServer *bound_server = nullptr;

    void forward_runtime_project_notification(
        ProjectNotification const &notification)
    {
        if (bound_server == nullptr) {
            return;
        }

        std::visit(
            [&](auto const &payload) {
                using Payload = std::remove_cvref_t<decltype(payload)>;
                if constexpr (std::same_as<
                                  Payload,
                                  ProjectMessageNotification>) {
                    bound_server->send_server_message(payload);
                } else if constexpr (std::same_as<
                                         Payload,
                                         ProjectStatusNotification>) {
                    bound_server->send_server_status(payload);
                } else if constexpr (std::same_as<
                                         Payload,
                                         ProjectLaneViewNotification>) {
                    bound_server->send_lane_view_updated(payload.lane_view);
                } else if constexpr (std::same_as<
                                         Payload,
                                         ProjectLogicalNodesNotification>) {
                    bound_server->send_logical_nodes_updated(payload);
                }
            },
            notification);
    }

    void forward_runtime_iv_module_definitions_notification(
        IvModuleDefinitionsNotification const &notification)
    {
        if (bound_server == nullptr) {
            return;
        }

        std::visit(
            [&](auto const &payload) {
                using Payload = std::remove_cvref_t<decltype(payload)>;
                if constexpr (std::same_as<Payload, IvModuleDefinitionsMessage>) {
                    bound_server->send_server_message(ProjectMessageNotification{
                        .level = payload.level,
                        .message = payload.message,
                        .module_root = payload.module_root,
                    });
                } else if constexpr (std::same_as<Payload, IvModuleDefinitionsStatus>) {
                    bound_server->send_server_status(ProjectStatusNotification{
                        .level = payload.level,
                        .code = payload.code,
                        .message = payload.message,
                        .module_root = payload.module_root,
                        .created_node_ids = payload.created_definition_ids,
                        .deleted_node_ids = payload.deleted_definition_ids,
                    });
                }
            },
            notification);
    }

    void forward_runtime_lane_views_updated(
        LaneViewResult const &lane_view)
    {
        if (bound_server == nullptr) {
            return;
        }
        bound_server->send_lane_view_updated(lane_view);
    }

    void forward_runtime_lane_view_content_updated(
        LaneViewContentUpdate const &update)
    {
        if (bound_server == nullptr) {
            return;
        }
        bound_server->send_lane_view_content_updated(update);
    }

    void forward_runtime_iv_module_instances_list_changed(
        std::vector<IvModuleInstanceInfo> const &instances)
    {
        if (bound_server == nullptr) {
            return;
        }
        bound_server->send_iv_module_instances_updated(instances);
    }

    IV_SUBSCRIBE_LINKER_EVENT(
        ProjectNotificationEvent,
        iv_runtime_project_notification_event,
        forward_runtime_project_notification);
    IV_SUBSCRIBE_LINKER_EVENT(
        IvModuleDefinitionsNotificationEvent,
        iv_runtime_iv_module_definitions_notification_event,
        forward_runtime_iv_module_definitions_notification);
    IV_SUBSCRIBE_LINKER_EVENT(
        LaneViewsUpdatedEvent,
        iv_runtime_lane_views_updated_event,
        forward_runtime_lane_views_updated);
    IV_SUBSCRIBE_LINKER_EVENT(
        LaneViewContentUpdatedEvent,
        iv_runtime_lane_view_content_updated_event,
        forward_runtime_lane_view_content_updated);
    IV_SUBSCRIBE_LINKER_EVENT(
        IvModuleInstancesListChangedEvent,
        iv_runtime_iv_module_instances_list_changed_event,
        forward_runtime_iv_module_instances_list_changed);
    IV_SUBSCRIBE_LINKER_EVENT(
        IvModuleSourceIntrospectionNodesUpdatedEvent,
        iv_runtime_iv_module_source_introspection_nodes_updated_event,
        +[](ProjectLogicalNodesNotification const &notification) {
            if (bound_server == nullptr) {
                return;
            }
            bound_server->send_logical_nodes_updated(notification);
        });
} // namespace

void bind_socket_rpc_notification_bridge(SocketRpcServer &server) {
    bound_server = &server;
}

void unbind_socket_rpc_notification_bridge(SocketRpcServer const &server) {
    if (bound_server == &server) {
        bound_server = nullptr;
    }
}
} // namespace iv
