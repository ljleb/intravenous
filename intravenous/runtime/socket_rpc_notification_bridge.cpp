#include "runtime/socket_rpc_notification_bridge.h"

#include "runtime/graph_input_lanes_events.h"
#include "runtime/iv_modules.h"
#include "runtime/iv_modules_events.h"
#include "runtime/lane_views_events.h"
#include "runtime/project_service_events.h"
#include "runtime/socket_rpc_server.h"

#include <type_traits>
#include <variant>

namespace iv {
namespace {
    SocketRpcServer *bound_server = nullptr;

    void forward_runtime_project_notification(
        RuntimeProjectNotification const &notification)
    {
        if (bound_server == nullptr) {
            return;
        }

        std::visit(
            [&](auto const &payload) {
                using Payload = std::remove_cvref_t<decltype(payload)>;
                if constexpr (std::same_as<
                                  Payload,
                                  RuntimeProjectMessageNotification>) {
                    bound_server->send_server_message(payload);
                } else if constexpr (std::same_as<
                                         Payload,
                                         RuntimeProjectStatusNotification>) {
                    bound_server->send_server_status(payload);
                } else if constexpr (std::same_as<
                                         Payload,
                                         RuntimeProjectLaneViewNotification>) {
                    bound_server->send_lane_view_updated(payload.lane_view);
                }
            },
            notification);
    }

    void forward_runtime_iv_modules_notification(
        RuntimeIvModulesNotification const &notification)
    {
        if (bound_server == nullptr) {
            return;
        }

        std::visit(
            [&](auto const &payload) {
                using Payload = std::remove_cvref_t<decltype(payload)>;
                if constexpr (std::same_as<Payload, RuntimeIvModulesMessage>) {
                    bound_server->send_server_message(RuntimeProjectMessageNotification{
                        .level = payload.level,
                        .message = payload.message,
                        .module_root = payload.module_root,
                    });
                } else if constexpr (std::same_as<Payload, RuntimeIvModulesStatus>) {
                    bound_server->send_server_status(RuntimeProjectStatusNotification{
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

    IV_SUBSCRIBE_LINKER_EVENT(
        RuntimeProjectNotificationEvent,
        iv_runtime_project_notification_event,
        forward_runtime_project_notification);
    IV_SUBSCRIBE_LINKER_EVENT(
        RuntimeIvModulesNotificationEvent,
        iv_runtime_iv_modules_notification_event,
        forward_runtime_iv_modules_notification);
    IV_SUBSCRIBE_LINKER_EVENT(
        RuntimeLaneViewsUpdatedEvent,
        iv_runtime_lane_views_updated_event,
        forward_runtime_lane_views_updated);
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
