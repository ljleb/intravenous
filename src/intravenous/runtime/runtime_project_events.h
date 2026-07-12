#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/module/loader.h>
#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/graph_input_lane_controller.h>
#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/runtime_project_api_types.h>
#include <intravenous/runtime/uuid.h>

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

    class ProjectLaneViewBuilder {
        std::optional<LaneViewResult> result;

    public:
        void succeed(LaneViewResult value);
        [[nodiscard]] LaneViewResult build() const;
    };

    struct ProjectSetSampleInputValueRequest {
        std::string node_id;
        std::optional<size_t> member_ordinal;
        size_t input_ordinal = 0;
        Sample value = Sample {0.0f};
    };

    struct ProjectCreateIvModuleInstanceRequest {
        std::optional<std::string> instance_id {};
        std::string module_id {};
        std::optional<std::string> display_name {};
    };

    struct ProjectDeleteIvModuleInstanceRequest {
        std::string instance_id {};
    };

    struct ProjectUpdateIvModuleInstance {
        std::string instance_id {};
        std::optional<std::string> display_name {};
        std::optional<size_t> default_silence_ttl_samples {};
    };

    struct ProjectUpdateIvModuleInstancesRequest {
        std::vector<ProjectUpdateIvModuleInstance> updates {};
    };

    struct ProjectSetTimelineCompiledSampleCacheChunkSizeMultiplierRequest {
        size_t compiled_sample_cache_chunk_size_multiplier = 0;
    };

    struct ProjectSetTimelineLaneSampleChannelTypeRequest {
        InternedString lane_id {};
        ChannelTypeId sample_channel_type = ChannelTypeId::stereo;
    };

    struct ProjectSetAudioDevicesRequest {
        std::optional<std::string> output_device_id {};
        std::optional<std::string> input_device_id {};
    };

    struct ProjectSetAudioDeviceLaneIdsRequest {
        InternedString output_lane_id {};
        InternedString input_lane_id {};
    };

    struct ProjectSetIvModuleToolchainConfigRequest {
        ModuleLoaderToolchainConfig toolchain {};
    };

    struct ProjectOverrideSettingsRequest {
        std::optional<std::filesystem::path> c_compiler {};
        std::optional<std::filesystem::path> cxx_compiler {};
        std::optional<std::filesystem::path> cmake_program {};
        std::optional<std::string> cmake_generator {};
        std::optional<std::filesystem::path> make_program {};
        std::optional<std::filesystem::path> juce_dir {};
        std::optional<size_t> compiled_sample_cache_chunk_size_multiplier {};
        std::optional<std::string> output_device_id {};
        std::optional<std::string> input_device_id {};
    };

    struct ProjectGraphInputLaneBindingsRequest {
        std::vector<GraphInputPortDescriptor> ports;
    };

    enum class ProjectSampleInputState {
        default_,
        overridden,
        logical_follow,
        timeline_lane,
        disconnected,
    };

    struct ProjectSetSampleInputStateRequest {
        std::string node_id;
        std::optional<size_t> member_ordinal;
        size_t input_ordinal = 0;
        ProjectSampleInputState state = ProjectSampleInputState::default_;
        std::optional<InternedString> lane_id {};
    };

    enum class ProjectEventInputState {
        default_,
        logical_follow,
        timeline_lane,
        disconnected,
    };

    struct ProjectSetEventInputStateRequest {
        std::string node_id;
        std::optional<size_t> member_ordinal;
        size_t input_ordinal = 0;
        ProjectEventInputState state = ProjectEventInputState::default_;
        std::optional<InternedString> lane_id {};
    };

    // Output-state requests. `disconnected` is the default and maps to *erasing* the
    // internal entry (mirror of input `default_` -> erase). Logical outputs only
    // support `disconnected` or `timeline_lane`.
    enum class ProjectSampleOutputState {
        disconnected,
        logical,
        timeline_lane,
    };

    struct ProjectSetSampleOutputStateRequest {
        std::string node_id;
        std::optional<size_t> member_ordinal;
        size_t output_ordinal = 0;
        ProjectSampleOutputState state = ProjectSampleOutputState::disconnected;
        std::optional<InternedString> lane_id {};
    };

    enum class ProjectEventOutputState {
        disconnected,
        logical,
        timeline_lane,
    };

    struct ProjectSetEventOutputStateRequest {
        std::string node_id;
        std::optional<size_t> member_ordinal;
        size_t output_ordinal = 0;
        ProjectEventOutputState state = ProjectEventOutputState::disconnected;
        std::optional<InternedString> lane_id {};
    };

    struct ProjectOpenLaneViewRequest {
        LaneViewRequest request {};
    };

    struct ProjectUpdateLaneViewRequest {
        LaneViewRequest request {};
    };

    struct ProjectCloseLaneViewRequest {
        InternedString view_id {};
    };

    struct ProjectConnectTimelineLanesRequest {
        InternedString source_lane_id {};
        InternedString target_lane_id {};
        LanePortDomain port_domain = LanePortDomain::realtime;
        PortKind port_kind = PortKind::sample;
        size_t port_ordinal = 0;
    };

    struct ProjectSetAutosaveEnabledRequest {
        bool enabled = true;
    };

    using ProjectNotificationEvent =
        void (*)(ProjectNotification const &);
    using ProjectStateChangedEvent =
        void (*)();
    using ProjectLoadedEvent =
        void (*)();
    using ProjectSetAutosaveEnabledRequestedEvent =
        void (*)(ProjectSetAutosaveEnabledRequest const &, ProjectAckBuilder &);
    using ProjectCreateIvModuleInstanceRequestedEvent =
        void (*)(ProjectCreateIvModuleInstanceRequest const &, ProjectStringBuilder &);
    using ProjectDeleteIvModuleInstanceRequestedEvent =
        void (*)(ProjectDeleteIvModuleInstanceRequest const &, ProjectAckBuilder &);
    using ProjectUpdateIvModuleInstancesRequestedEvent =
        void (*)(ProjectUpdateIvModuleInstancesRequest const &, ProjectAckBuilder &);
    using ProjectSetTimelineCompiledSampleCacheChunkSizeMultiplierRequestedEvent =
        void (*)(ProjectSetTimelineCompiledSampleCacheChunkSizeMultiplierRequest const &, ProjectAckBuilder &);
    using ProjectSetTimelineLaneSampleChannelTypeRequestedEvent =
        void (*)(ProjectSetTimelineLaneSampleChannelTypeRequest const &, ProjectAckBuilder &);
    using ProjectSetAudioDevicesRequestedEvent =
        void (*)(ProjectSetAudioDevicesRequest const &, ProjectAudioDevicesBuilder &);
    using ProjectSetAudioDeviceLaneIdsRequestedEvent =
        void (*)(ProjectSetAudioDeviceLaneIdsRequest const &, ProjectAckBuilder &);
    using ProjectSetIvModuleToolchainConfigRequestedEvent =
        void (*)(ProjectSetIvModuleToolchainConfigRequest const &, ProjectAckBuilder &);
    using ProjectOverrideSettingsRequestedEvent =
        void (*)(ProjectOverrideSettingsRequest const &);
    using ProjectGraphInputLaneBindingsEnsuredEvent =
        void (*)(ProjectGraphInputLaneBindingsRequest const &, ProjectAckBuilder &);
    using ProjectOpenLaneViewRequestedEvent =
        void (*)(ProjectOpenLaneViewRequest const &, ProjectLaneViewBuilder &);
    using ProjectUpdateLaneViewRequestedEvent =
        void (*)(ProjectUpdateLaneViewRequest const &, ProjectLaneViewBuilder &);
    using ProjectCloseLaneViewRequestedEvent =
        void (*)(ProjectCloseLaneViewRequest const &, ProjectAckBuilder &);
    using ProjectConnectTimelineLanesRequestedEvent =
        void (*)(ProjectConnectTimelineLanesRequest const &, ProjectAckBuilder &);
    using ProjectSetSampleInputValueRequestedEvent =
        void (*)(ProjectSetSampleInputValueRequest const &, ProjectAckBuilder &);
    using ProjectSetSampleInputStateRequestedEvent =
        void (*)(ProjectSetSampleInputStateRequest const &, ProjectAckBuilder &);
    using ProjectSetEventInputStateRequestedEvent =
        void (*)(ProjectSetEventInputStateRequest const &, ProjectAckBuilder &);
    using ProjectSetSampleOutputStateRequestedEvent =
        void (*)(ProjectSetSampleOutputStateRequest const &, ProjectAckBuilder &);
    using ProjectSetEventOutputStateRequestedEvent =
        void (*)(ProjectSetEventOutputStateRequest const &, ProjectAckBuilder &);
    IV_DECLARE_LINKER_EVENT(
        ProjectNotificationEvent,
        iv_runtime_project_notification_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectStateChangedEvent,
        iv_runtime_project_state_changed_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectLoadedEvent,
        iv_runtime_project_loaded_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectSetAutosaveEnabledRequestedEvent,
        iv_runtime_project_set_autosave_enabled_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectSetIvModuleToolchainConfigRequestedEvent,
        iv_runtime_project_set_iv_module_toolchain_config_requested_event);
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
        ProjectSetTimelineCompiledSampleCacheChunkSizeMultiplierRequestedEvent,
        iv_runtime_project_set_timeline_compiled_sample_cache_chunk_size_multiplier_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectSetTimelineLaneSampleChannelTypeRequestedEvent,
        iv_runtime_project_set_timeline_lane_sample_channel_type_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectSetAudioDevicesRequestedEvent,
        iv_runtime_project_set_audio_devices_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectSetAudioDeviceLaneIdsRequestedEvent,
        iv_runtime_project_set_audio_device_lane_ids_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectGraphInputLaneBindingsEnsuredEvent,
        iv_runtime_project_graph_input_lane_bindings_ensured_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectOpenLaneViewRequestedEvent,
        iv_runtime_project_open_lane_view_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectUpdateLaneViewRequestedEvent,
        iv_runtime_project_update_lane_view_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectCloseLaneViewRequestedEvent,
        iv_runtime_project_close_lane_view_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectConnectTimelineLanesRequestedEvent,
        iv_runtime_project_connect_timeline_lanes_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectSetSampleInputValueRequestedEvent,
        iv_runtime_project_set_sample_input_value_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectSetSampleInputStateRequestedEvent,
        iv_runtime_project_set_sample_input_state_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectSetEventInputStateRequestedEvent,
        iv_runtime_project_set_event_input_state_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectSetSampleOutputStateRequestedEvent,
        iv_runtime_project_set_sample_output_state_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectSetEventOutputStateRequestedEvent,
        iv_runtime_project_set_event_output_state_requested_event);
} // namespace iv
