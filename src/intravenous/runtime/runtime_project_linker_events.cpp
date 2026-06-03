#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    ProjectNotificationEvent,
    iv_runtime_project_notification_event)
IV_DEFINE_LINKER_EVENT(
    ProjectLiveInputSnapshotsRequestedEvent,
    iv_runtime_project_live_input_snapshots_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphInputLaneBindingsEnsuredEvent,
    iv_runtime_project_graph_input_lane_bindings_ensured_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphInputLaneBindingsRequestedEvent,
    iv_runtime_project_graph_input_lane_bindings_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectLaneOutputsRequestedEvent,
    iv_runtime_project_lane_outputs_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectSetSampleInputValueRequestedEvent,
    iv_runtime_project_set_sample_input_value_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectClearSampleInputValueOverrideRequestedEvent,
    iv_runtime_project_clear_sample_input_value_override_requested_event)
} // namespace iv
