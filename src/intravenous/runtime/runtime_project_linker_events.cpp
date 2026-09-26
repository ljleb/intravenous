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
    ProjectCreateIvModuleInstanceRequestedEvent,
    iv_runtime_project_create_iv_module_instance_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectDeleteIvModuleInstanceRequestedEvent,
    iv_runtime_project_delete_iv_module_instance_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectUpdateIvModuleInstancesRequestedEvent,
    iv_runtime_project_update_iv_module_instances_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectUpsertGraphConnectionRequestedEvent,
    iv_runtime_project_upsert_graph_connection_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectDeleteGraphConnectionRequestedEvent,
    iv_runtime_project_delete_graph_connection_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectSetAudioDevicesRequestedEvent,
    iv_runtime_project_set_audio_devices_requested_event)
} // namespace iv
