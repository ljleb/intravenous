#pragma once

#include <intravenous/devices/audio_device.h>
#include <intravenous/linker_event.h>
#include <intravenous/runtime/project_connection_types.h>
#include <intravenous/runtime/project_graph_publication.h>
#include <intravenous/runtime/project_node_instance_types.h>
#include <intravenous/runtime/project_node_replacement_types.h>
#include <intravenous/runtime/runtime_project_api_types.h>
#include <intravenous/node/manual_control.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace iv {
// The read-model changes a project-graph request committed travel back on its
// result builder, so the cause's root publishes them once.
class ProjectAckBuilder {
    std::optional<std::string> error_message;
    bool handled = false;
    ProjectGraphPublication publication_{};

public:
    void succeed();
    void fail(std::string message);
    void build() const;
    void set_publication(ProjectGraphPublication publication)
    {
        publication_ = std::move(publication);
    }
    [[nodiscard]] ProjectGraphPublication take_publication()
    {
        return std::move(publication_);
    }
};

class ProjectStringBuilder {
    std::optional<std::string> result;
    ProjectGraphPublication publication_{};

public:
    void succeed(std::string value);
    [[nodiscard]] std::string build() const;
    void set_publication(ProjectGraphPublication publication)
    {
        publication_ = std::move(publication);
    }
    [[nodiscard]] ProjectGraphPublication take_publication()
    {
        return std::move(publication_);
    }
};

class ProjectAudioDevicesBuilder {
    std::optional<AudioDevicesSnapshot> result;

public:
    void succeed(AudioDevicesSnapshot value);
    [[nodiscard]] AudioDevicesSnapshot build() const;
};

struct ProjectCreateNodeInstanceRequest {
    std::optional<std::string> instance_id {};
    std::string definition_id {};
    std::optional<std::filesystem::path> package_root {};
    std::optional<std::string> display_name {};
    std::string configuration_source {};
};

struct ProjectDeleteNodeInstanceRequest {
    std::string instance_id {};
};

struct ProjectUpdateNodeInstance {
    std::string instance_id {};
    std::optional<std::string> display_name {};
    std::optional<std::string> configuration_source {};
};

struct ProjectUpdateNodeInstancesRequest {
    std::vector<ProjectUpdateNodeInstance> updates {};
};

struct ProjectUpsertGraphConnectionRequest {
    ProjectConnection connection{};
};

struct ProjectDeleteGraphConnectionRequest {
    std::string connection_id{};
};

struct ProjectReplaceGraphRequest {
    std::vector<ProjectNodeInstanceDeclaration> instances{};
    std::vector<ProjectNodeReplacement> node_replacements{};
    std::vector<ProjectConnection> connections{};
    std::vector<ProjectManualControl> manual_controls{};
};

struct ProjectUpsertManualControlRequest {
    ProjectManualControl control{};
};

struct ProjectDeleteManualControlRequest {
    ManualControlId id{};
};

struct ProjectSetManualControlHoverRequest {
    std::string client_id{};
    std::optional<ManualControlId> id{};
};

struct ProjectBeginManualControlInteractionRequest {
    ManualControlInteractionId interaction{};
    ManualControlId id{};
    Sample value{};
};

struct ProjectUpdateManualControlInteractionRequest {
    ManualControlInteractionId interaction{};
    Sample value{};
};

struct ProjectEndManualControlInteractionRequest {
    ManualControlInteractionId interaction{};
    Sample value{};
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
using ProjectCreateNodeInstanceRequestedEvent =
    void (*)(ProjectCreateNodeInstanceRequest const&, ProjectStringBuilder&);
using ProjectDeleteNodeInstanceRequestedEvent =
    void (*)(ProjectDeleteNodeInstanceRequest const&, ProjectAckBuilder&);
using ProjectUpdateNodeInstancesRequestedEvent =
    void (*)(ProjectUpdateNodeInstancesRequest const&, ProjectAckBuilder&);
using ProjectUpsertGraphConnectionRequestedEvent =
    void (*)(ProjectUpsertGraphConnectionRequest const&, ProjectAckBuilder&);
using ProjectDeleteGraphConnectionRequestedEvent =
    void (*)(ProjectDeleteGraphConnectionRequest const&, ProjectAckBuilder&);
using ProjectReplaceGraphRequestedEvent =
    void (*)(ProjectReplaceGraphRequest const&, ProjectAckBuilder&);
using ProjectUpsertManualControlRequestedEvent =
    void (*)(ProjectUpsertManualControlRequest const&, ProjectAckBuilder&);
using ProjectDeleteManualControlRequestedEvent =
    void (*)(ProjectDeleteManualControlRequest const&, ProjectAckBuilder&);
using ProjectSetManualControlHoverRequestedEvent =
    void (*)(ProjectSetManualControlHoverRequest const&, ProjectAckBuilder&);
using ProjectBeginManualControlInteractionRequestedEvent = void (*)(
    ProjectBeginManualControlInteractionRequest const&, ProjectAckBuilder&);
using ProjectUpdateManualControlInteractionRequestedEvent = void (*)(
    ProjectUpdateManualControlInteractionRequest const&, ProjectAckBuilder&);
using ProjectEndManualControlInteractionRequestedEvent = void (*)(
    ProjectEndManualControlInteractionRequest const&, ProjectAckBuilder&);
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
    ProjectCreateNodeInstanceRequestedEvent,
    iv_runtime_project_create_node_instance_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectDeleteNodeInstanceRequestedEvent,
    iv_runtime_project_delete_node_instance_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectUpdateNodeInstancesRequestedEvent,
    iv_runtime_project_update_node_instances_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectUpsertGraphConnectionRequestedEvent,
    iv_runtime_project_upsert_graph_connection_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectDeleteGraphConnectionRequestedEvent,
    iv_runtime_project_delete_graph_connection_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectReplaceGraphRequestedEvent,
    iv_runtime_project_replace_graph_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectUpsertManualControlRequestedEvent,
    iv_runtime_project_upsert_manual_control_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectDeleteManualControlRequestedEvent,
    iv_runtime_project_delete_manual_control_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectSetManualControlHoverRequestedEvent,
    iv_runtime_project_set_manual_control_hover_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectBeginManualControlInteractionRequestedEvent,
    iv_runtime_project_begin_manual_control_interaction_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectUpdateManualControlInteractionRequestedEvent,
    iv_runtime_project_update_manual_control_interaction_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectEndManualControlInteractionRequestedEvent,
    iv_runtime_project_end_manual_control_interaction_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectSetAudioDevicesRequestedEvent,
    iv_runtime_project_set_audio_devices_requested_event);
} // namespace iv
