#include <intravenous/runtime/system_audio_devices.h>

#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <algorithm>
#include <ranges>
#include <stdexcept>

namespace iv {
namespace {
std::optional<AudioDeviceDescriptor> find_device_descriptor(
    std::vector<AudioDeviceDescriptor> const &devices,
    std::string const &device_id)
{
    auto const it = std::ranges::find(
        devices,
        device_id,
        &AudioDeviceDescriptor::device_id);
    if (it == devices.end()) {
        return std::nullopt;
    }
    return *it;
}
} // namespace

SystemAudioDevices::SystemAudioDevices(
    size_t sample_rate,
    size_t block_size,
    SystemAudioDevicesBackend backend,
    std::optional<std::string> selected_output_device_id,
    std::optional<std::string> selected_input_device_id)
    : sample_rate_(sample_rate)
    , block_size_(block_size)
    , backend_(std::move(backend))
    , output_render_config_(RenderConfig{
          .sample_rate = sample_rate,
          .num_channels = 2,
          .max_block_frames = std::max<size_t>(block_size * 4, 4096),
          .preferred_block_size = block_size,
      })
    , input_render_config_(RenderConfig{
          .sample_rate = sample_rate,
          .num_channels = 2,
          .max_block_frames = std::max<size_t>(block_size * 4, 4096),
          .preferred_block_size = block_size,
      })
    , selected_output_device_id_(std::move(selected_output_device_id))
    , selected_input_device_id_(std::move(selected_input_device_id))
{
    if (sample_rate_ == 0 || block_size_ == 0) {
        throw std::invalid_argument(
            "system audio devices require non-zero sample rate and block size");
    }
    if (!backend_.list_output_devices || !backend_.list_input_devices
        || !backend_.make_output_device || !backend_.make_input_device) {
        throw std::invalid_argument("system audio devices backend is incomplete");
    }

    replace_output_device(create_output_device(selected_output_device_id_));
    replace_input_device(create_input_device(selected_input_device_id_));
}

SystemAudioDevices::~SystemAudioDevices()
{
    request_shutdown();
    if (input_capture_thread_.joinable()) {
        input_capture_thread_.join();
    }
}

std::vector<AudioDeviceDescriptor> SystemAudioDevices::list_output_devices_unlocked() const
{
    return backend_.list_output_devices();
}

std::vector<AudioDeviceDescriptor> SystemAudioDevices::list_input_devices_unlocked() const
{
    return backend_.list_input_devices();
}

std::shared_ptr<SystemAudioDevices::ActiveOutputDevice>
SystemAudioDevices::create_output_device(std::optional<std::string> const &device_id) const
{
    if (!device_id.has_value()) {
        return nullptr;
    }

    auto const descriptor = find_device_descriptor(list_output_devices_unlocked(), *device_id);
    if (!descriptor.has_value()) {
        return nullptr;
    }

    try {
        return std::make_shared<ActiveOutputDevice>(ActiveOutputDevice{
            .descriptor = *descriptor,
            .device = backend_.make_output_device(*device_id, output_render_config_),
        });
    } catch (std::exception const &) {
        return nullptr;
    }
}

std::shared_ptr<SystemAudioDevices::ActiveInputDevice>
SystemAudioDevices::create_input_device(std::optional<std::string> const &device_id) const
{
    if (!device_id.has_value()) {
        return nullptr;
    }

    auto const descriptor = find_device_descriptor(list_input_devices_unlocked(), *device_id);
    if (!descriptor.has_value()) {
        return nullptr;
    }

    try {
        auto device = backend_.make_input_device(*device_id, input_render_config_);
        auto const target_latency_frames = std::max(
            block_size_ * 2,
            device.config().preferred_block_size * 2);
        auto synchronizer = std::make_unique<AudioInputSynchronizer>(
            sample_rate_,
            block_size_,
            device.config().sample_rate,
            target_latency_frames);
        return std::make_shared<ActiveInputDevice>(
            *descriptor,
            std::move(device),
            std::move(synchronizer));
    } catch (std::exception const &) {
        return nullptr;
    }
}

void SystemAudioDevices::start_input_capture_thread(
    std::shared_ptr<ActiveInputDevice> input_device)
{
    if (!input_device) {
        return;
    }
    input_capture_thread_ = std::thread([this, input_device = std::move(input_device)] {
        input_capture_loop(input_device);
    });
}

void SystemAudioDevices::input_capture_loop(std::shared_ptr<ActiveInputDevice> input_device)
{
    while (!shutdown_requested_.load()) {
        try {
            auto block = input_device->device.wait_for_captured_block();
            if (!block.samples.empty()) {
                input_device->synchronizer->push_captured_block(block);
            }
            input_device->device.release_captured_block();
        } catch (std::exception const &) {
            return;
        }
    }
}

void SystemAudioDevices::request_shutdown()
{
    if (shutdown_requested_.exchange(true)) {
        return;
    }

    std::shared_ptr<ActiveOutputDevice> output_device;
    std::shared_ptr<ActiveInputDevice> input_device;
    {
        std::scoped_lock lock(mutex_);
        output_device = std::move(active_output_device_);
        input_device = std::move(active_input_device_);
    }
    if (output_device) {
        output_device->device.request_shutdown();
    }
    if (input_device) {
        input_device->device.request_shutdown();
    }
}

AudioDeviceSelectionState SystemAudioDevices::selection_state_for(
    std::optional<std::string> const &selected_id,
    std::shared_ptr<ActiveOutputDevice> const &active_device,
    std::vector<AudioDeviceDescriptor> const &available_devices) const
{
    AudioDeviceSelectionState selection;
    selection.device_id = selected_id;
    selection.available = static_cast<bool>(active_device);
    if (active_device) {
        selection.name = active_device->descriptor.name;
    } else if (selected_id.has_value()) {
        if (auto descriptor = find_device_descriptor(available_devices, *selected_id)) {
            selection.name = descriptor->name;
        }
    }
    return selection;
}

AudioDeviceSelectionState SystemAudioDevices::selection_state_for(
    std::optional<std::string> const &selected_id,
    std::shared_ptr<ActiveInputDevice> const &active_device,
    std::vector<AudioDeviceDescriptor> const &available_devices) const
{
    AudioDeviceSelectionState selection;
    selection.device_id = selected_id;
    selection.available = static_cast<bool>(active_device);
    if (active_device) {
        selection.name = active_device->descriptor.name;
    } else if (selected_id.has_value()) {
        if (auto descriptor = find_device_descriptor(available_devices, *selected_id)) {
            selection.name = descriptor->name;
        }
    }
    return selection;
}

AudioDevicesSnapshot SystemAudioDevices::audio_devices_snapshot() const
{
    std::shared_ptr<ActiveOutputDevice> active_output;
    std::shared_ptr<ActiveInputDevice> active_input;
    std::optional<std::string> selected_output;
    std::optional<std::string> selected_input;
    {
        std::scoped_lock lock(mutex_);
        active_output = active_output_device_;
        active_input = active_input_device_;
        selected_output = selected_output_device_id_;
        selected_input = selected_input_device_id_;
    }

    AudioDevicesSnapshot snapshot;
    snapshot.output_devices = list_output_devices_unlocked();
    snapshot.input_devices = list_input_devices_unlocked();
    snapshot.selected_output =
        selection_state_for(selected_output, active_output, snapshot.output_devices);
    snapshot.selected_input =
        selection_state_for(selected_input, active_input, snapshot.input_devices);
    return snapshot;
}

void SystemAudioDevices::replace_output_device(
    std::shared_ptr<ActiveOutputDevice> output_device)
{
    std::shared_ptr<ActiveOutputDevice> old_output;
    {
        std::scoped_lock lock(mutex_);
        old_output = std::move(active_output_device_);
        active_output_device_ = std::move(output_device);
    }
    if (old_output) {
        old_output->device.request_shutdown();
    }
}

void SystemAudioDevices::replace_input_device(
    std::shared_ptr<ActiveInputDevice> input_device)
{
    std::shared_ptr<ActiveInputDevice> old_input;
    std::thread old_thread;
    {
        std::scoped_lock lock(mutex_);
        old_input = std::move(active_input_device_);
        active_input_device_ = input_device;
        if (input_capture_thread_.joinable()) {
            old_thread = std::move(input_capture_thread_);
        }
    }

    if (old_input) {
        old_input->device.request_shutdown();
    }
    if (old_thread.joinable()) {
        old_thread.join();
    }
    start_input_capture_thread(std::move(input_device));
}

AudioDevicesSnapshot SystemAudioDevices::set_selected_devices(
    std::optional<std::string> output_device_id,
    std::optional<std::string> input_device_id)
{
    {
        std::scoped_lock lock(mutex_);
        selected_output_device_id_ = std::move(output_device_id);
        selected_input_device_id_ = std::move(input_device_id);
    }

    replace_output_device(create_output_device(selected_output_device_id_));
    replace_input_device(create_input_device(selected_input_device_id_));
    return audio_devices_snapshot();
}

void SystemAudioDevices::handle_socket_rpc_get_audio_devices(
    GetAudioDevicesRequest const &,
    SocketRpcAudioDevicesResultBuilder &builder) const
{
    builder.succeed(audio_devices_snapshot());
}

void SystemAudioDevices::handle_socket_rpc_set_audio_devices(
    SetAudioDevicesRequest const &request,
    SocketRpcAudioDevicesResultBuilder &builder)
{
    try {
        builder.succeed(set_selected_devices(
            request.output_device_id,
            request.input_device_id));
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
    } catch (std::exception const &error) {
        builder.fail(error.what());
    }
}

void SystemAudioDevices::handle_project_set_audio_devices(
    ProjectSetAudioDevicesRequest const &request,
    ProjectAudioDevicesBuilder &builder)
{
    builder.succeed(set_selected_devices(
        request.output_device_id,
        request.input_device_id));
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void SystemAudioDevices::handle_project_override_settings(
    ProjectOverrideSettingsRequest const &request)
{
    if (!request.output_device_id.has_value() && !request.input_device_id.has_value()) {
        return;
    }
    auto const current = audio_devices_snapshot();
    (void)set_selected_devices(
        request.output_device_id.has_value()
            ? request.output_device_id
            : current.selected_output.device_id,
        request.input_device_id.has_value()
            ? request.input_device_id
            : current.selected_input.device_id);
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void SystemAudioDevices::handle_project_persistence_collect_state(
    ProjectPersistenceBuilder &builder) const
{
    auto const snapshot = audio_devices_snapshot();
    builder.add_project_audio_device_selection(
        snapshot.selected_output.device_id,
        snapshot.selected_input.device_id);
}

} // namespace iv
