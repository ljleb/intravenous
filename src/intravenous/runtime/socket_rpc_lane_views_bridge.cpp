#include <intravenous/runtime/socket_rpc_lane_views_bridge.h>

#include <intravenous/runtime/lane_views.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
namespace {
    void handle_open_lane_view(
        LaneViewRequest const &request,
        SocketRpcLaneViewResultBuilder &builder)
    {
        try {
            ProjectLaneViewBuilder project_builder;
            IV_INVOKE_LINKER_EVENT(
                iv_runtime_project_open_lane_view_requested_event,
                ProjectOpenLaneViewRequest{
                    .request = request,
                },
                project_builder);
            builder.succeed(project_builder.build());
        } catch (std::exception const &e) {
            builder.fail(e.what());
        }
    }

    void handle_update_lane_view(
        LaneViewRequest const &request,
        SocketRpcLaneViewResultBuilder &builder)
    {
        try {
            ProjectLaneViewBuilder project_builder;
            IV_INVOKE_LINKER_EVENT(
                iv_runtime_project_update_lane_view_requested_event,
                ProjectUpdateLaneViewRequest{
                    .request = request,
                },
                project_builder);
            builder.succeed(project_builder.build());
        } catch (std::exception const &e) {
            builder.fail(e.what());
        }
    }

    void handle_close_lane_view(
        std::string const &view_id,
        SocketRpcAckResponseBuilder &builder)
    {
        try {
            ProjectAckBuilder project_builder;
            IV_INVOKE_LINKER_EVENT(
                iv_runtime_project_close_lane_view_requested_event,
                ProjectCloseLaneViewRequest{
                    .view_id = InternedString::from_string(view_id),
                },
                project_builder);
            project_builder.build();
        } catch (std::exception const &e) {
            builder.fail(e.what());
        }
    }

    IV_SUBSCRIBE_LINKER_EVENT(
        SocketRpcOpenLaneViewEvent,
        iv_socket_rpc_open_lane_view_event,
        handle_open_lane_view);
    IV_SUBSCRIBE_LINKER_EVENT(
        SocketRpcUpdateLaneViewEvent,
        iv_socket_rpc_update_lane_view_event,
        handle_update_lane_view);
    IV_SUBSCRIBE_LINKER_EVENT(
        SocketRpcCloseLaneViewEvent,
        iv_socket_rpc_close_lane_view_event,
        handle_close_lane_view);
} // namespace

void bind_socket_rpc_lane_views_bridge(LaneViews &lane_views)
{
    (void)lane_views;
}

void unbind_socket_rpc_lane_views_bridge(LaneViews const &lane_views)
{
    (void)lane_views;
}
} // namespace iv
