#pragma once

#include <intravenous/devices/audio_device.h>
#include <intravenous/runtime/audio_input_synchronizer.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace iv {
struct GetAudioDevicesRequest;
class ProjectAudioDevicesBuilder;
class ProjectPersistenceBuilder;
struct ProjectOverrideSettingsRequest;
struct ProjectSetAudioDevicesRequest;
struct SetAudioDevicesRequest;
class SocketRpcAudioDevicesResultBuilder;

struct SystemAudioDevicesBackend {
    std::function<std::vector<AudioDeviceDescriptor>()> list_output_devices {};
    std::function<std::vector<AudioDeviceDescriptor>()> list_input_devices {};
    std::function<AudioOutputDevice(std::string const &, RenderConfig const &)> make_output_device {};
    std::function<AudioInputDevice(std::string const &, RenderConfig const &)> make_input_device {};
};

// Owns only system audio-device state and lifetime. Graph routing and execution
// attach to this module through future focused bridges; they are deliberately
// not modeled as device-owned graph objects.
class SystemAudioDevices {
    struct ActiveOutputDevice {
        AudioDeviceDescriptor descriptor {};
        AudioOutputDevice device;
    };

    struct ActiveInputDevice {
        AudioDeviceDescriptor descriptor {};
        AudioInputDevice device;
        std::unique_ptr<AudioInputSynchronizer> synchronizer {};

        ActiveInputDevice(
            AudioDeviceDescriptor descriptor_,
            AudioInputDevice device_,
            std::unique_ptr<AudioInputSynchronizer> synchronizer_)
            : descriptor(std::move(descriptor_))
            , device(std::move(device_))
            , synchronizer(std::move(synchronizer_))
        {
        }
    };

    size_t sample_rate_ = 48000;
    size_t block_size_ = 256;
    SystemAudioDevicesBackend backend_ {};
    RenderConfig output_render_config_ {};
    RenderConfig input_render_config_ {};
    std::optional<std::string> selected_output_device_id_ {};
    std::optional<std::string> selected_input_device_id_ {};
    std::shared_ptr<ActiveOutputDevice> active_output_device_ {};
    std::shared_ptr<ActiveInputDevice> active_input_device_ {};
    std::atomic<bool> shutdown_requested_ = false;
    mutable std::mutex mutex_;
    std::thread input_capture_thread_ {};

    [[nodiscard]] std::vector<AudioDeviceDescriptor> list_output_devices_unlocked() const;
    [[nodiscard]] std::vector<AudioDeviceDescriptor> list_input_devices_unlocked() const;
    [[nodiscard]] std::shared_ptr<ActiveOutputDevice> create_output_device(
        std::optional<std::string> const &device_id) const;
    [[nodiscard]] std::shared_ptr<ActiveInputDevice> create_input_device(
        std::optional<std::string> const &device_id) const;
    void start_input_capture_thread(std::shared_ptr<ActiveInputDevice> input_device);
    void input_capture_loop(std::shared_ptr<ActiveInputDevice> input_device);
    [[nodiscard]] AudioDeviceSelectionState selection_state_for(
        std::optional<std::string> const &selected_id,
        std::shared_ptr<ActiveOutputDevice> const &active_device,
        std::vector<AudioDeviceDescriptor> const &available_devices) const;
    [[nodiscard]] AudioDeviceSelectionState selection_state_for(
        std::optional<std::string> const &selected_id,
        std::shared_ptr<ActiveInputDevice> const &active_device,
        std::vector<AudioDeviceDescriptor> const &available_devices) const;
    void replace_output_device(std::shared_ptr<ActiveOutputDevice> output_device);
    void replace_input_device(std::shared_ptr<ActiveInputDevice> input_device);

public:
    SystemAudioDevices(
        size_t sample_rate,
        size_t block_size,
        SystemAudioDevicesBackend backend,
        std::optional<std::string> selected_output_device_id = std::optional<std::string>("default"),
        std::optional<std::string> selected_input_device_id = std::optional<std::string>("default"));
    ~SystemAudioDevices();

    SystemAudioDevices(SystemAudioDevices const &) = delete;
    SystemAudioDevices &operator=(SystemAudioDevices const &) = delete;
    SystemAudioDevices(SystemAudioDevices &&) = delete;
    SystemAudioDevices &operator=(SystemAudioDevices &&) = delete;

    void request_shutdown();
    [[nodiscard]] AudioDevicesSnapshot audio_devices_snapshot() const;
    AudioDevicesSnapshot set_selected_devices(
        std::optional<std::string> output_device_id,
        std::optional<std::string> input_device_id);

    void handle_socket_rpc_get_audio_devices(
        GetAudioDevicesRequest const &request,
        SocketRpcAudioDevicesResultBuilder &builder) const;
    void handle_socket_rpc_set_audio_devices(
        SetAudioDevicesRequest const &request,
        SocketRpcAudioDevicesResultBuilder &builder);
    void handle_project_set_audio_devices(
        ProjectSetAudioDevicesRequest const &request,
        ProjectAudioDevicesBuilder &builder);
    void handle_project_override_settings(ProjectOverrideSettingsRequest const &request);
    void handle_project_persistence_collect_state(ProjectPersistenceBuilder &builder) const;
};

} // namespace iv
