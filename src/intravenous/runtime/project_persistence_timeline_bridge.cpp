#include <intravenous/runtime/project_persistence_timeline_bridge.h>

#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/timeline.h>

namespace iv {
IV_DEFINE_BRIDGE(project_persistence_timeline_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_timeline_bridge,
    iv_runtime_project_set_timeline_lane_sample_channel_type_requested_event,
    &Timeline::handle_project_set_timeline_lane_sample_channel_type)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_timeline_bridge,
    iv_runtime_project_set_timeline_lane_ui_state_requested_event,
    &Timeline::handle_project_set_timeline_lane_ui_state)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_timeline_bridge,
    iv_runtime_project_connect_timeline_lanes_requested_event,
    &Timeline::handle_project_connect_timeline_lanes)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_timeline_bridge,
    iv_runtime_project_disconnect_timeline_lanes_requested_event,
    &Timeline::handle_project_disconnect_timeline_lanes)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_timeline_bridge,
    iv_runtime_project_persistence_collect_state_event,
    &Timeline::handle_project_persistence_collect_state)
} // namespace iv
