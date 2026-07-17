#include <intravenous/runtime/timeline_lane_query_schema_bridge.h>

#include <intravenous/runtime/lane_query_schema_service.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_events.h>

namespace iv {
namespace {
LaneQuerySchemaService *bound_service = nullptr;
Timeline *bound_timeline = nullptr;

void handle_timeline_lanes_changed(TimelineLanesChanged const &change)
{
    (void)change;
    if (bound_service != nullptr && bound_timeline != nullptr) {
        bound_service->handle_timeline_lanes_changed(*bound_timeline);
    }
}

IV_SUBSCRIBE_LINKER_EVENT(
    TimelineLanesChangedEvent,
    iv_runtime_timeline_lanes_changed_event,
    handle_timeline_lanes_changed);
} // namespace

void bind_timeline_lane_query_schema_bridge(
    LaneQuerySchemaService &service,
    Timeline &timeline)
{
    service.initialize(timeline.lane_query_schema(0));
    bound_service = &service;
    bound_timeline = &timeline;
}

void unbind_timeline_lane_query_schema_bridge(
    LaneQuerySchemaService const &service,
    Timeline const &timeline)
{
    (void)timeline;
    if (bound_service == &service) {
        bound_service = nullptr;
    }
    if (bound_timeline == &timeline) {
        bound_timeline = nullptr;
    }
}
} // namespace iv
