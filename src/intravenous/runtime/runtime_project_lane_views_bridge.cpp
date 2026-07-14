#include <intravenous/runtime/runtime_project_lane_views_bridge.h>

#include <intravenous/runtime/lane_views.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
namespace {
LaneViews *bound_lane_views = nullptr;

void handle_open_lane_view(
    ProjectOpenLaneViewRequest const &request,
    ProjectLaneViewBuilder &builder)
{
    if (bound_lane_views == nullptr) {
        return;
    }
    builder.succeed(bound_lane_views->open_view(request.request));
}

void handle_update_lane_view(
    ProjectUpdateLaneViewRequest const &request,
    ProjectLaneViewBuilder &builder)
{
    if (bound_lane_views == nullptr) {
        return;
    }
    builder.succeed(bound_lane_views->update_view(request.request));
}

void handle_close_lane_view(
    ProjectCloseLaneViewRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_lane_views == nullptr) {
        return;
    }
    bound_lane_views->close_view(request.view_id.str());
    builder.succeed();
}

IV_SUBSCRIBE_LINKER_EVENT(
    ProjectOpenLaneViewRequestedEvent,
    iv_runtime_project_open_lane_view_requested_event,
    handle_open_lane_view);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectUpdateLaneViewRequestedEvent,
    iv_runtime_project_update_lane_view_requested_event,
    handle_update_lane_view);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectCloseLaneViewRequestedEvent,
    iv_runtime_project_close_lane_view_requested_event,
    handle_close_lane_view);
} // namespace

void bind_runtime_project_lane_views_bridge(LaneViews &lane_views)
{
    bound_lane_views = &lane_views;
}

void unbind_runtime_project_lane_views_bridge(LaneViews const &lane_views)
{
    if (bound_lane_views == &lane_views) {
        bound_lane_views = nullptr;
    }
}
} // namespace iv
