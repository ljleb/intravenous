#pragma once

#include <intravenous/devices/audio_device.h>
#include <intravenous/linker_event.h>
#include <intravenous/runtime/project_connection_types.h>
#include <intravenous/runtime/runtime_project_api_types.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace iv {
class ProjectAckBuilder {
    std::optional<std::string> error_message;
    bool handled = false;

public:
    void succeed();
    void fail(std::string message);
    void build() const;
};

class ProjectStringBuilder {
    std::optional<std::string> result;

public:
    void succeed(std::string value);
    [[nodiscard]] std::string build() const;
};

class ProjectAudioDevicesBuilder {
    std::optional<AudioDevicesSnapshot> result;

public:
    void succeed(AudioDevicesSnapshot value);
    [[nodiscard]] AudioDevicesSnapshot build() const;
};

struct ProjectCreateIvModuleInstanceRequest {
    std::optional<std::string> instance_id {};
    std::string module_id {};
    std::optional<std::filesystem::path> package_root {};
    std::optional<std::string> display_name {};
};

struct ProjectDeleteIvModuleInstanceRequest {
    std::string instance_id {};
};

struct ProjectUpdateIvModuleInstance {
    std::string instance_id {};
    std::optional<std::string> display_name {};
};

struct ProjectUpdateIvModuleInstancesRequest {
    std::vector<ProjectUpdateIvModuleInstance> updates {};
};

struct ProjectUpsertGraphConnectionRequest {
    ProjectConnection connection{};
};

struct ProjectDeleteGraphConnectionRequest {
    std::string connection_id{};
};

struct ProjectSetAudioDevicesRequest {
    std::optional<std::string> output_device_id {};
    std::optional<std::string> input_device_id {};
};

struct ProjectOverrideSettingsRequest {
    std::optional<std::filesystem::path> c_compiler {};
    std::optional<std::filesystem::path> cxx_compiler {};
    std::optional<std::filesystem::path> cmake_program {};
    std::optional<std::string> cmake_generator {};
    std::optional<std::filesystem::path> make_program {};
    std::optional<std::filesystem::path> juce_dir {};
    std::optional<std::filesystem::path> iv_package_pch {};
    std::optional<std::string> output_device_id {};
    std::optional<std::string> input_device_id {};
};

struct ProjectSetAutosaveEnabledRequest {
    bool enabled = true;
};

using ProjectNotificationEvent = void (*)(ProjectNotification const&);
using ProjectStateChangedEvent = void (*)();
using ProjectLoadedEvent = void (*)();
using ProjectSetAutosaveEnabledRequestedEvent =
    void (*)(ProjectSetAutosaveEnabledRequest const&, ProjectAckBuilder&);
using ProjectCreateIvModuleInstanceRequestedEvent =
    void (*)(ProjectCreateIvModuleInstanceRequest const&, ProjectStringBuilder&);
using ProjectDeleteIvModuleInstanceRequestedEvent =
    void (*)(ProjectDeleteIvModuleInstanceRequest const&, ProjectAckBuilder&);
using ProjectUpdateIvModuleInstancesRequestedEvent =
    void (*)(ProjectUpdateIvModuleInstancesRequest const&, ProjectAckBuilder&);
using ProjectUpsertGraphConnectionRequestedEvent =
    void (*)(ProjectUpsertGraphConnectionRequest const&, ProjectAckBuilder&);
using ProjectDeleteGraphConnectionRequestedEvent =
    void (*)(ProjectDeleteGraphConnectionRequest const&, ProjectAckBuilder&);
using ProjectSetAudioDevicesRequestedEvent =
    void (*)(ProjectSetAudioDevicesRequest const&, ProjectAudioDevicesBuilder&);
using ProjectOverrideSettingsRequestedEvent =
    void (*)(ProjectOverrideSettingsRequest const&);

IV_DECLARE_LINKER_EVENT(ProjectNotificationEvent, iv_runtime_project_notification_event);
IV_DECLARE_LINKER_EVENT(ProjectStateChangedEvent, iv_runtime_project_state_changed_event);
IV_DECLARE_LINKER_EVENT(ProjectLoadedEvent, iv_runtime_project_loaded_event);
IV_DECLARE_LINKER_EVENT(
    ProjectSetAutosaveEnabledRequestedEvent,
    iv_runtime_project_set_autosave_enabled_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectOverrideSettingsRequestedEvent,
    iv_runtime_project_override_settings_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectCreateIvModuleInstanceRequestedEvent,
    iv_runtime_project_create_iv_module_instance_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectDeleteIvModuleInstanceRequestedEvent,
    iv_runtime_project_delete_iv_module_instance_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectUpdateIvModuleInstancesRequestedEvent,
    iv_runtime_project_update_iv_module_instances_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectUpsertGraphConnectionRequestedEvent,
    iv_runtime_project_upsert_graph_connection_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectDeleteGraphConnectionRequestedEvent,
    iv_runtime_project_delete_graph_connection_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectSetAudioDevicesRequestedEvent,
    iv_runtime_project_set_audio_devices_requested_event);
} // namespace iv
