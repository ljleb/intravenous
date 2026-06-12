#pragma once

#include <intravenous/lane_node/graph.h>
#include <intravenous/runtime/task_ids.h>
#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/timeline_events.h>

#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace iv {
class TimelineExecution {
public:
    explicit TimelineExecution(size_t block_size);

    TaskGraphUpdate synchronize_from_graph(LaneGraph const &graph);
    TaskGraphUpdate handle_timeline_lanes_changed(TimelineLanesChanged const &change);

    std::vector<LaneId> realtime_sample_output_lanes() const;
    void set_realtime_start_index(size_t start_index);
    std::span<Sample const> realtime_sample_block(LaneId lane) const;
    std::span<TimedEvent const> realtime_event_block(LaneId lane) const;
    std::vector<Sample> compiled_sample_block(LaneId lane, size_t start_index);
    std::vector<TimedEvent> compiled_event_block(LaneId lane, size_t start_index);

private:
    struct TrackedLane {
        LaneId id {};
        TypeErasedLaneNode const *node = nullptr;
        LaneOutputConfig output {};
        std::vector<LaneInputConnection> inputs {};
        TaskCallback callback {};
    };

    struct LaneCallbackContext {
        TimelineExecution *execution = nullptr;
        LaneId lane {};
    };

    struct CompiledSampleCacheEntry {
        size_t start_index = 0;
        std::vector<Sample> samples {};
        bool valid = false;
    };

    struct CompiledEventCacheEntry {
        size_t start_index = 0;
        std::vector<TimedEvent> events {};
        bool valid = false;
    };

    struct RealtimeSamplePortDescriptor {
        LaneId lane {};
    };

    size_t block_size_ = 0;
    mutable std::mutex mutex_;
    std::unordered_map<LaneId, TrackedLane, LaneIdHash> tracked_lanes_;
    std::unordered_map<LaneId, std::unique_ptr<LaneCallbackContext>, LaneIdHash> callback_contexts_;
    std::vector<RealtimeSamplePortDescriptor> realtime_sample_descriptors_;
    std::unordered_map<LaneId, size_t, LaneIdHash> realtime_sample_slot_by_lane_;
    std::vector<Sample> realtime_sample_storage_;
    std::unordered_map<LaneId, std::vector<TimedEvent>, LaneIdHash> realtime_event_blocks_;
    std::unordered_map<LaneId, CompiledSampleCacheEntry, LaneIdHash> compiled_sample_cache_;
    std::unordered_map<LaneId, CompiledEventCacheEntry, LaneIdHash> compiled_event_cache_;
    size_t current_start_index_ = 0;

    static void invoke_lane_task(void *context);
    void execute_lane_task(LaneId lane);
    void rebuild_runtime_storage_locked();
    TaskGraphUpdate replace_all_lanes_locked(std::vector<TrackedLane> lanes);
    std::vector<LaneId> topological_order_locked() const;
    void execute_lane_locked(LaneId lane, size_t start_index);
    std::span<Sample> realtime_sample_block_mutable_locked(LaneId lane);
    std::span<Sample const> realtime_sample_block_locked(LaneId lane) const;
    std::vector<Sample> const& ensure_compiled_sample_block_locked(LaneId lane, size_t start_index);
    std::vector<TimedEvent> const& ensure_compiled_event_block_locked(LaneId lane, size_t start_index);
    static bool has_task_graph_changes(TaskGraphUpdate const &update);
};
}
