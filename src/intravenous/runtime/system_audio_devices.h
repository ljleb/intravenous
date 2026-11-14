#pragma once

#include <intravenous/runtime/project_settings.h>

#include <intravenous/devices/audio_device.h>
#include <intravenous/runtime/audio_input_synchronizer.h>
#include <intravenous/node/resources.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace iv {
struct GetAudioDevicesRequest;
class ProjectAudioDevicesBuilder;
class ProjectPersistenceBuilder;
struct ProjectOverrideSettingsRequest;
struct ProjectSetAudioDevicesRequest;
struct SetAudioDevicesRequest;
class SocketRpcAudioDevicesResultBuilder;
class GraphExecutor;

struct SystemAudioDevicesBackend {
    std::function<std::vector<AudioDeviceDescriptor>()> list_output_devices {};
    std::function<std::vector<AudioDeviceDescriptor>()> list_input_devices {};
    std::function<AudioOutputDevice(std::string const &, RenderConfig const &)> make_output_device {};
    std::function<AudioInputDevice(std::string const &, RenderConfig const &)> make_input_device {};
};

// Owns only system audio-device state and lifetime. The application attaches
// GraphExecutor as the output-clock callback target, but this module does not
// own project graph nodes, connections, or the executor itself.
class SystemAudioDevices {
    struct LogicalInputBinding {
        SystemAudioDevices* devices = nullptr;
        std::string device_id{};
    };

    struct LogicalOutputBinding {
        SystemAudioDevices* devices = nullptr;
        std::string device_id{};
    };

    struct ActiveOutputDevice {
        AudioDeviceDescriptor descriptor {};
        AudioOutputDevice device;
        bool hardware_available = false;
        std::atomic<bool> stop_requested = false;

        ActiveOutputDevice(
            AudioDeviceDescriptor descriptor_,
            AudioOutputDevice device_,
            bool hardware_available_)
            : descriptor(std::move(descriptor_))
            , device(std::move(device_))
            , hardware_available(hardware_available_)
        {
        }
    };

    struct ActiveInputDevice {
        AudioDeviceDescriptor descriptor {};
        AudioInputDevice device;
        std::unique_ptr<AudioInputSynchronizer> synchronizer {};
        std::atomic<bool> failed = false;

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
    std::optional<std::string> output_activation_error_{};
    std::optional<std::string> input_activation_error_{};
    std::shared_ptr<ActiveOutputDevice> active_output_device_ {};
    std::shared_ptr<ActiveInputDevice> active_input_device_ {};
    std::atomic<bool> shutdown_requested_ = false;
    std::atomic<bool> output_render_failed_ = false;
    std::atomic<GraphExecutor*> graph_executor_ = nullptr;
    std::atomic<ActiveInputDevice*> input_render_device_ = nullptr;
    std::atomic<ActiveInputDevice*> input_render_hazard_ = nullptr;
    std::atomic<LogicalInputBinding*> active_input_binding_ = nullptr;
    std::atomic<LogicalOutputBinding*> active_output_binding_ = nullptr;
    std::mutex selection_mutex_;
    bool selection_initialized_ = false;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<LogicalInputBinding>>
        input_bindings_{};
    std::unordered_map<std::string, std::unique_ptr<LogicalOutputBinding>>
        output_bindings_{};
    std::thread input_capture_thread_ {};
    std::thread output_render_thread_ {};
    std::vector<Sample> input_render_buffer_{};
    GlobalIndex realtime_cursor_{};

    friend class AudioDeviceSelectionTransaction;
    AudioDevicesSnapshot apply_selected_devices_locked(ProjectAudioDeviceSelection selection);
    [[nodiscard]] std::vector<AudioDeviceDescriptor> list_output_devices_unlocked() const;
    [[nodiscard]] std::vector<AudioDeviceDescriptor> list_input_devices_unlocked() const;
    [[nodiscard]] std::shared_ptr<ActiveOutputDevice> create_output_device(
        std::optional<std::string> const &device_id, std::optional<std::string>* error = nullptr) const;
    [[nodiscard]] std::shared_ptr<ActiveInputDevice> create_input_device(
        std::optional<std::string> const &device_id, std::optional<std::string>* error = nullptr) const;
    void start_input_capture_thread(std::shared_ptr<ActiveInputDevice> input_device);
    void start_output_render_thread(std::shared_ptr<ActiveOutputDevice> output_device);
    void input_capture_loop(std::shared_ptr<ActiveInputDevice> input_device);
    void output_render_loop(std::shared_ptr<ActiveOutputDevice> output_device) noexcept;
    void refresh_active_bindings_locked();
    [[nodiscard]] SystemAudioInputBinding resolve_input_binding(
        std::string_view device_id);
    [[nodiscard]] SystemAudioOutputBinding resolve_output_binding(
        std::string_view device_id);
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
    void wait_until_input_device_is_unused(
        ActiveInputDevice const* device) const noexcept;

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
    // Completes device-thread shutdown. It is idempotent and must run before
    // an attached GraphExecutor or its ResourceContext owners are destroyed.
    void stop();
    void attach_graph_executor(GraphExecutor& executor) noexcept;
    void detach_graph_executor(GraphExecutor& executor) noexcept;
    [[nodiscard]] bool output_render_failed() const noexcept;
    [[nodiscard]] ResourceContext resources() noexcept;
    [[nodiscard]] AudioDevicesSnapshot audio_devices_snapshot() const;
    [[nodiscard]] ProjectAudioDeviceSelection selected_devices() const;
    void handle_project_graph_audio_selection(AudioDeviceSelectionTransactionRequest& request);
    AudioDevicesSnapshot set_selected_devices(
        std::optional<std::string> output_device_id,
        std::optional<std::string> input_device_id);

    void handle_socket_rpc_get_audio_devices(
        GetAudioDevicesRequest const &request,
        SocketRpcAudioDevicesResultBuilder &builder) const;
    void handle_project_override_settings(ProjectOverrideSettingsRequest const &request);
    void handle_project_persistence_collect_state(ProjectPersistenceBuilder &builder) const;
};

} // namespace iv
