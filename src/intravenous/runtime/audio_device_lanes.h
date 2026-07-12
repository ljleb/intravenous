#pragma once

#include <intravenous/devices/audio_device.h>
#include <intravenous/runtime/audio_input_synchronizer.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/sample_stream_blocks.h>
#include <intravenous/runtime/task_runner_events.h>
#include <intravenous/runtime/uuid.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace iv {

struct AudioDeviceLanesBackend {
    std::function<std::vector<AudioDeviceDescriptor>()> list_output_devices {};
    std::function<std::vector<AudioDeviceDescriptor>()> list_input_devices {};
    std::function<AudioOutputDevice(std::string const &, RenderConfig const &)> make_output_device {};
    std::function<AudioInputDevice(std::string const &, RenderConfig const &)> make_input_device {};
};

class AudioDeviceLanes {
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
            : descriptor(std::move(descriptor_)),
              device(std::move(device_)),
              synchronizer(std::move(synchronizer_)) {}
    };

    struct PendingOutputRequest {
        std::span<Sample> samples {};
        size_t written_samples = 0;
    };

    size_t timeline_sample_rate_ = 48000;
    size_t timeline_block_size_ = 256;
    AudioDeviceLanesBackend backend_ {};
    RenderConfig output_render_config_ {};
    RenderConfig input_render_config_ {};
    std::optional<std::string> selected_output_device_id_ {};
    std::optional<std::string> selected_input_device_id_ {};
    std::shared_ptr<ActiveOutputDevice> active_output_device_ {};
    std::shared_ptr<ActiveOutputDevice> pass_output_device_ {};
    std::shared_ptr<ActiveInputDevice> active_input_device_ {};
    LaneId output_lane_id_ {};
    LaneId input_lane_id_ {};
    InternedString output_lane_external_id_ {};
    InternedString input_lane_external_id_ {};
    std::atomic<bool> shutdown_requested_ = false;
    mutable std::mutex mutex_;
    std::vector<Sample> output_reservoir_ {};
    size_t output_reservoir_read_offset_ = 0;
    std::optional<PendingOutputRequest> pending_output_request_ {};
    OwnedSampleBlock current_input_block_ {};
    size_t next_realtime_start_index_ = 0;
    std::thread input_capture_thread_ {};

    std::vector<AudioDeviceDescriptor> list_output_devices_unlocked() const;
    std::vector<AudioDeviceDescriptor> list_input_devices_unlocked() const;
    std::shared_ptr<ActiveOutputDevice> create_output_device(std::optional<std::string> const &device_id) const;
    std::shared_ptr<ActiveInputDevice> create_input_device(std::optional<std::string> const &device_id) const;
    void start_input_capture_thread(std::shared_ptr<ActiveInputDevice> input_device);
    void input_capture_loop(std::shared_ptr<ActiveInputDevice> input_device);
    void initialize_current_input_block();
    void initialize_timeline_lanes();
    void append_output_block_locked(SampleBlockView<Sample const> view);
    bool try_fill_pending_output_request_locked();
    void compact_output_reservoir_locked();
    void prepare_next_input_block();
    AudioDeviceSelectionState selection_state_for(
        std::optional<std::string> const &selected_id,
        std::shared_ptr<ActiveOutputDevice> const &active_device,
        std::vector<AudioDeviceDescriptor> const &available_devices) const;
    AudioDeviceSelectionState selection_state_for(
        std::optional<std::string> const &selected_id,
        std::shared_ptr<ActiveInputDevice> const &active_device,
        std::vector<AudioDeviceDescriptor> const &available_devices) const;
    void replace_output_device(std::shared_ptr<ActiveOutputDevice> output_device);
    void replace_input_device(std::shared_ptr<ActiveInputDevice> input_device);

public:
    AudioDeviceLanes(
        size_t timeline_sample_rate,
        size_t timeline_block_size,
        AudioDeviceLanesBackend backend,
        std::optional<std::string> selected_output_device_id = std::optional<std::string>("default"),
        std::optional<std::string> selected_input_device_id = std::optional<std::string>("default"));
    ~AudioDeviceLanes();

    AudioDeviceLanes(AudioDeviceLanes const &) = delete;
    AudioDeviceLanes &operator=(AudioDeviceLanes const &) = delete;
    AudioDeviceLanes(AudioDeviceLanes &&) = delete;
    AudioDeviceLanes &operator=(AudioDeviceLanes &&) = delete;

    void request_shutdown();
    void bind();
    AudioDevicesSnapshot audio_devices_snapshot() const;
    AudioDevicesSnapshot set_selected_devices(
        std::optional<std::string> output_device_id,
        std::optional<std::string> input_device_id);
    void set_lane_external_ids(
        InternedString output_lane_external_id,
        InternedString input_lane_external_id);
    [[nodiscard]] InternedString output_lane_external_id() const;
    [[nodiscard]] InternedString input_lane_external_id() const;
    void seek_realtime_start_index(size_t sample_index);

    void handle_task_runner_before_pass(TasksRunnerBeforePass const &pass);
    void handle_task_runner_after_pass(TasksRunnerAfterPass const &pass);
    OwnedSampleBlock handle_input_block_requested(LaneId lane) const;
};

} // namespace iv
