#include <intravenous/runtime/project_persistence_authored_lanes_bridge.h>

#include <intravenous/runtime/authored_lanes.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_persistence_authored_lanes_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_authored_lanes_bridge,
    iv_runtime_project_get_timeline_lane_types_requested_event,
    &AuthoredLanes::handle_project_get_timeline_lane_types)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_authored_lanes_bridge,
    iv_runtime_project_create_timeline_lane_requested_event,
    &AuthoredLanes::handle_project_create_timeline_lane)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_authored_lanes_bridge,
    iv_runtime_project_delete_timeline_lane_requested_event,
    &AuthoredLanes::handle_project_delete_timeline_lane)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_authored_lanes_bridge,
    iv_runtime_project_duplicate_timeline_lane_requested_event,
    &AuthoredLanes::handle_project_duplicate_timeline_lane)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_authored_lanes_bridge,
    iv_runtime_project_persistence_collect_state_event,
    &AuthoredLanes::handle_project_persistence_collect_state)
} // namespace iv
