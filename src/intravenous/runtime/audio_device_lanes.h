#pragma once

#include <intravenous/devices/audio_device.h>
#include <intravenous/runtime/audio_input_synchronizer.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/sample_stream_blocks.h>
#include <intravenous/runtime/task_runner_events.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace iv {

class AudioDeviceLanes {
    struct PendingOutputRequest {
        std::span<Sample> samples {};
        size_t written_samples = 0;
    };

    size_t timeline_sample_rate_ = 48000;
    size_t timeline_block_size_ = 256;
    std::optional<AudioOutputDevice> output_device_ {};
    std::optional<AudioInputDevice> input_device_ {};
    std::optional<AudioInputSynchronizer> input_synchronizer_ {};
    LaneId output_lane_id_ {};
    LaneId input_lane_id_ {};
    std::atomic<bool> shutdown_requested_ = false;
    mutable std::mutex mutex_;
    std::vector<Sample> output_reservoir_ {};
    size_t output_reservoir_read_offset_ = 0;
    std::optional<PendingOutputRequest> pending_output_request_ {};
    OwnedSampleBlock current_input_block_ {};
    size_t next_realtime_start_index_ = 0;
    std::thread input_capture_thread_ {};

    void start_input_capture_thread();
    void input_capture_loop();
    void initialize_current_input_block();
    void initialize_timeline_lanes();
    void append_output_block_locked(SampleBlockView<Sample const> view);
    bool try_fill_pending_output_request_locked();
    void compact_output_reservoir_locked();
    void prepare_next_input_block();

public:
    AudioDeviceLanes(
        size_t timeline_sample_rate,
        size_t timeline_block_size,
        std::optional<AudioOutputDevice> output_device,
        std::optional<AudioInputDevice> input_device);
    ~AudioDeviceLanes();

    AudioDeviceLanes(AudioDeviceLanes const &) = delete;
    AudioDeviceLanes &operator=(AudioDeviceLanes const &) = delete;
    AudioDeviceLanes(AudioDeviceLanes &&) = delete;
    AudioDeviceLanes &operator=(AudioDeviceLanes &&) = delete;

    void request_shutdown();
    void bind();

    void handle_task_runner_before_pass(TasksRunnerBeforePass const &pass);
    void handle_task_runner_after_pass(TasksRunnerAfterPass const &pass);
    BorrowedSampleBlock handle_input_block_requested(LaneId lane) const;
};

} // namespace iv
