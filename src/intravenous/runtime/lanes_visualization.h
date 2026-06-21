#pragma once

#include <intravenous/runtime/lane_view_service.h>
#include <intravenous/runtime/sample_stream_blocks.h>
#include <intravenous/runtime/visualization_lane_nodes.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
struct TasksRunnerPassFinished;

class LanesVisualization {
    struct SampleHistory {
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        std::vector<Sample::storage> storage {};
        size_t next_frame = 0;
        size_t size_frames = 0;

        void append(SampleStorageBlock const& block, size_t history_block_count);
        [[nodiscard]] SampleStorageBlock snapshot() const;
    };

    struct TrackedRealtimeSampleLane {
        LaneId source_lane {};
        LaneId vis_lane {};
        std::shared_ptr<RealtimeSampleBlockQueue> queue {};
        ChannelTypeId sample_channel_type = ChannelTypeId::stereo;
        SampleHistory history {};
        bool registered_in_timeline = false;
    };

    struct TrackedRealtimeEventLane {
        LaneId source_lane {};
        LaneId vis_lane {};
        std::shared_ptr<RealtimeEventBlockQueue> queue {};
        std::vector<TimedEvent> history {};
        bool registered_in_timeline = false;
    };

    struct ActiveView {
        std::string view_id {};
        std::vector<LaneId> realtime_sample_lanes {};
        std::vector<LaneId> realtime_event_lanes {};
        std::vector<LaneId> compiled_sample_lanes {};
        std::vector<LaneId> compiled_event_lanes {};
        size_t first_sample_index = 0;
        size_t last_sample_index = 0;
        size_t display_sample_count = 0;
        std::unordered_map<uint64_t, SampleStorageBlock> compiled_sample_data {};
        std::unordered_map<uint64_t, std::vector<TimedEvent>> compiled_event_data {};
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ActiveView> active_views_;
    std::unordered_map<LaneId, TrackedRealtimeSampleLane, LaneIdHash> tracked_sample_lanes_;
    std::unordered_map<LaneId, TrackedRealtimeEventLane, LaneIdHash> tracked_event_lanes_;
    std::vector<std::shared_ptr<RealtimeSampleBlockQueue>> draining_sample_queues_;
    std::vector<std::shared_ptr<RealtimeEventBlockQueue>> draining_event_queues_;
    std::vector<LaneId> pending_timeline_lane_removals_;
    LaneIdAllocator lane_id_allocator_;
    size_t history_block_count_ = 16;
    size_t block_size_ = 512;
    std::optional<std::jthread> publisher_thread_ {};
    std::chrono::milliseconds publish_interval_ { 33 };

    void publisher_loop(std::stop_token stop_token);

public:
    explicit LanesVisualization(
        std::optional<std::chrono::milliseconds> publish_interval = std::chrono::milliseconds(33),
        size_t history_block_count = 16,
        size_t block_size = 512);
    ~LanesVisualization() = default;

    void handle_lane_views_updated(LaneViewResult const &update);
    void handle_lane_view_closed(std::string const &view_id);
    void handle_task_runner_pass_finished(TasksRunnerPassFinished const &finished);
    void publish_now();
};
} // namespace iv
