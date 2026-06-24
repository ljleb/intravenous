#include <intravenous/runtime/runtime_project_audio_device_lanes_bridge.h>

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
namespace {
AudioDeviceLanes *bound_audio_device_lanes = nullptr;

std::optional<std::string> resolve_device_override(
    nlohmann::ordered_json const &args,
    char const *key,
    std::optional<std::string> const &startup_default)
{
    auto const it = args.find(key);
    if (it == args.end()) {
        return std::nullopt;
    }
    if (it->is_null()) {
        return std::optional<std::string>{};
    }
    if (!it->is_string()) {
        throw std::runtime_error(std::string("project override setting '") + key + "' must be a string or null");
    }
    auto value = it->get<std::string>();
    if (value == "default") {
        return startup_default;
    }
    return value;
}

void handle_set_audio_devices(
    ProjectSetAudioDevicesRequest const &request,
    ProjectAudioDevicesBuilder &builder)
{
    if (bound_audio_device_lanes == nullptr) {
        return;
    }
    builder.succeed(bound_audio_device_lanes->set_selected_devices(
        request.output_device_id,
        request.input_device_id));
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_set_audio_device_lane_ids(
    ProjectSetAudioDeviceLaneIdsRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_audio_device_lanes == nullptr) {
        return;
    }
    bound_audio_device_lanes->set_lane_external_ids(
        request.output_lane_id,
        request.input_lane_id);
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_override_settings(ProjectOverrideSettingsRequest const &request)
{
    if (bound_audio_device_lanes == nullptr) {
        return;
    }
    auto const output_device_id = resolve_device_override(
        request.args,
        "output_device_id",
        request.startup.output_device_id);
    auto const input_device_id = resolve_device_override(
        request.args,
        "input_device_id",
        request.startup.input_device_id);
    if (!request.args.contains("output_device_id") && !request.args.contains("input_device_id")) {
        return;
    }
    (void)bound_audio_device_lanes->set_selected_devices(
        request.args.contains("output_device_id")
            ? output_device_id
            : bound_audio_device_lanes->audio_devices_snapshot().selected_output.device_id,
        request.args.contains("input_device_id")
            ? input_device_id
            : bound_audio_device_lanes->audio_devices_snapshot().selected_input.device_id);
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_collect_state(ProjectPersistenceBuilder &builder)
{
    if (bound_audio_device_lanes == nullptr) {
        return;
    }
    auto const snapshot = bound_audio_device_lanes->audio_devices_snapshot();
    builder.add_project_audio_device_selection(
        snapshot.selected_output.device_id,
        snapshot.selected_input.device_id);
    builder.add_audio_device_lane_ids(
        bound_audio_device_lanes->output_lane_external_id().str(),
        bound_audio_device_lanes->input_lane_external_id().str());
}

IV_SUBSCRIBE_LINKER_EVENT(
    ProjectSetAudioDevicesRequestedEvent,
    iv_runtime_project_set_audio_devices_requested_event,
    handle_set_audio_devices);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectSetAudioDeviceLaneIdsRequestedEvent,
    iv_runtime_project_set_audio_device_lane_ids_requested_event,
    handle_set_audio_device_lane_ids);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectOverrideSettingsRequestedEvent,
    iv_runtime_project_override_settings_requested_event,
    handle_override_settings);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectPersistenceCollectStateEvent,
    iv_runtime_project_persistence_collect_state_event,
    handle_collect_state);
} // namespace

void bind_runtime_project_audio_device_lanes_bridge(AudioDeviceLanes &audio_device_lanes)
{
    bound_audio_device_lanes = &audio_device_lanes;
}

void unbind_runtime_project_audio_device_lanes_bridge(
    AudioDeviceLanes const &audio_device_lanes)
{
    if (bound_audio_device_lanes == &audio_device_lanes) {
        bound_audio_device_lanes = nullptr;
    }
}
} // namespace iv
