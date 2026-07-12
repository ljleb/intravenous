#pragma once

#include <intravenous/lane_node/graph.h>
#include <intravenous/runtime/sample_stream_blocks.h>
#include <intravenous/runtime/task_ids.h>
#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/timeline_events.h>

#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace iv {
struct CompiledSupportChunkRange {
    size_t start_chunk_index = 0;
    size_t end_chunk_index = 0;
};

class TimelineExecution {
public:
    explicit TimelineExecution(
        size_t block_size,
        size_t compiled_sample_cache_chunk_size_multiplier = 16);

    VersionedTaskGraphUpdate synchronize_from_graph(LaneGraph const &graph);
    VersionedTaskGraphUpdate handle_timeline_lanes_changed(TimelineLanesChanged const &change);

    std::vector<LaneId> realtime_sample_output_lanes() const;
    void pause();
    void resume(size_t start_index);
    void seek(size_t sample_index);
    [[nodiscard]] bool is_paused() const;
    void set_realtime_start_index(size_t start_index);
    [[nodiscard]] size_t realtime_start_index() const;
    void set_compiled_sample_cache_chunk_size_multiplier(size_t multiplier);
    size_t compiled_sample_cache_chunk_size_multiplier() const;
    BorrowedSampleBlock realtime_sample_block(LaneId lane) const;
    std::span<TimedEvent const> realtime_event_block(LaneId lane) const;
    OwnedSampleBlock compiled_sample_block(LaneId lane, size_t start_index);
    std::vector<TimedEvent> compiled_event_block(LaneId lane, size_t start_index);
    Sample::storage compiled_sample_level(LaneId lane, size_t first, size_t last);
    std::vector<TimedEvent> compiled_events_in_range(LaneId lane, size_t first, size_t last);

private:
    struct TrackedLane {
        LaneId id {};
        TypeErasedLaneNode const *node = nullptr;
        LaneOutputConfig output {};
        std::optional<ChannelTypeId> sample_channel_type {};
        std::vector<LaneInputConnection> inputs {};
        std::vector<std::string> external_task_dependencies {};
        TaskCallback callback {};
    };

    struct LaneCallbackContext {
        TimelineExecution *execution = nullptr;
        LaneId lane {};
    };

    struct CompiledSampleChunkCache {
        std::unordered_map<size_t, std::vector<Sample>> chunks {};
    };

    struct CompiledEventCacheEntry {
        size_t start_index = 0;
        std::vector<TimedEvent> events {};
        bool valid = false;
    };

    struct CompiledSupportState {
        std::vector<CompiledSupportRange> sample_ranges {};
        std::vector<CompiledSupportChunkRange> chunk_ranges {};
    };

    struct RealtimeSamplePortDescriptor {
        LaneId lane {};
        size_t sample_offset = 0;
        size_t sample_count = 0;
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
    };

    size_t block_size_ = 0;
    size_t compiled_sample_cache_chunk_size_multiplier_ = 16;
    mutable std::mutex mutex_;
    std::unordered_map<LaneId, TrackedLane, LaneIdHash> tracked_lanes_;
    std::unordered_map<LaneId, std::unique_ptr<LaneCallbackContext>, LaneIdHash> callback_contexts_;
    std::vector<RealtimeSamplePortDescriptor> realtime_sample_descriptors_;
    std::unordered_map<LaneId, RealtimeSamplePortDescriptor, LaneIdHash> realtime_sample_slot_by_lane_;
    std::vector<Sample> realtime_sample_storage_;
    std::unordered_map<LaneId, std::vector<TimedEvent>, LaneIdHash> realtime_event_blocks_;
    std::unordered_map<LaneId, CompiledSupportState, LaneIdHash> compiled_support_by_lane_;
    std::unordered_map<LaneId, CompiledSampleChunkCache, LaneIdHash> compiled_sample_cache_;
    std::unordered_map<LaneId, CompiledEventCacheEntry, LaneIdHash> compiled_event_cache_;
    size_t current_start_index_ = 0;
    bool paused_ = false;

    static void invoke_lane_task(void *context);
    void execute_lane_task(LaneId lane);
    void rebuild_runtime_storage_locked();
    void rebuild_compiled_support_and_notify_locked();
    VersionedTaskGraphUpdate replace_all_lanes_locked(std::vector<TrackedLane> lanes);
    std::vector<LaneId> topological_order_locked() const;
    void execute_lane_locked(LaneId lane, size_t start_index);
    SampleBlockView<Sample> realtime_sample_block_mutable_locked(LaneId lane);
    SampleBlockView<Sample const> realtime_sample_block_locked(LaneId lane) const;
    std::span<CompiledSupportRange const> compiled_support_ranges_locked(LaneId lane) const;
    std::span<CompiledSupportChunkRange const> compiled_support_chunk_ranges_locked(LaneId lane) const;
    bool compiled_support_intersects_request_locked(LaneId lane, size_t start_index, size_t sample_count) const;
    size_t compiled_sample_cache_chunk_size_locked() const;
    void execute_compiled_sample_chunk_locked(LaneId lane, size_t chunk_index);
    OwnedSampleBlock read_compiled_sample_block_locked(LaneId lane, size_t start_index);
    std::vector<TimedEvent> const& ensure_compiled_event_block_locked(LaneId lane, size_t start_index);
    static bool has_task_graph_changes(TaskGraphUpdate const &update);
};
}
