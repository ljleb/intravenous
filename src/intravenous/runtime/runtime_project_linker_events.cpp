#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(ProjectNotificationEvent, iv_runtime_project_notification_event)
IV_DEFINE_LINKER_EVENT(ProjectStateChangedEvent, iv_runtime_project_state_changed_event)
IV_DEFINE_LINKER_EVENT(ProjectLoadedEvent, iv_runtime_project_loaded_event)
IV_DEFINE_LINKER_EVENT(
    ProjectSetAutosaveEnabledRequestedEvent,
    iv_runtime_project_set_autosave_enabled_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectOverrideSettingsRequestedEvent,
    iv_runtime_project_override_settings_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectCreateNodeInstanceRequestedEvent,
    iv_runtime_project_create_node_instance_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectDeleteNodeInstanceRequestedEvent,
    iv_runtime_project_delete_node_instance_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectUpdateNodeInstancesRequestedEvent,
    iv_runtime_project_update_node_instances_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectUpsertGraphConnectionRequestedEvent,
    iv_runtime_project_upsert_graph_connection_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectDeleteGraphConnectionRequestedEvent,
    iv_runtime_project_delete_graph_connection_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectReplaceGraphRequestedEvent,
    iv_runtime_project_replace_graph_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectUpsertManualControlRequestedEvent,
    iv_runtime_project_upsert_manual_control_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectDeleteManualControlRequestedEvent,
    iv_runtime_project_delete_manual_control_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectSetManualControlHoverRequestedEvent,
    iv_runtime_project_set_manual_control_hover_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectBeginManualControlInteractionRequestedEvent,
    iv_runtime_project_begin_manual_control_interaction_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectUpdateManualControlInteractionRequestedEvent,
    iv_runtime_project_update_manual_control_interaction_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectEndManualControlInteractionRequestedEvent,
    iv_runtime_project_end_manual_control_interaction_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectSetAudioDevicesRequestedEvent,
    iv_runtime_project_set_audio_devices_requested_event)
} // namespace iv
