#pragma once

#include <intravenous/lane_node/ui_state.h>
#include <intravenous/linker_event.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/lanes_visualization_api_types.h>
#include <intravenous/runtime/timeline_events.h>

#include <optional>
#include <vector>

namespace iv {

struct LaneVisualizationOutputDescriptor {
    LanePortConfig config {};
    std::optional<ChannelTypeId> sample_channel_type {};
    bool subscribes_to_compiled_output_changes = false;
};

class LanesVisualizationLaneOutputQueryBuilder {
    std::optional<LaneVisualizationOutputDescriptor> result_ {};

public:
    void succeed(LaneVisualizationOutputDescriptor descriptor)
    {
        result_ = std::move(descriptor);
    }

    [[nodiscard]] std::optional<LaneVisualizationOutputDescriptor> build()
    {
        return std::move(result_);
    }
};

using LanesVisualizationLaneOutputQueryEvent =
    void (*)(LaneId, LanesVisualizationLaneOutputQueryBuilder&);

class LanesVisualizationPlaybackPositionBuilder {
    std::optional<size_t> result_ {};

public:
    void succeed(size_t sample_index) { result_ = sample_index; }
    [[nodiscard]] std::optional<size_t> build() { return result_; }
};

using LanesVisualizationPlaybackPositionQueryEvent =
    void (*)(LanesVisualizationPlaybackPositionBuilder&);

class LanesVisualizationLaneUiStateBuilder {
    std::optional<LaneUiStateSnapshot> result_ {};
public:
    void succeed(LaneUiStateSnapshot snapshot) { result_ = std::move(snapshot); }
    [[nodiscard]] std::optional<LaneUiStateSnapshot> build() { return std::move(result_); }
};
using LanesVisualizationLaneUiStateQueryEvent =
    void (*)(LaneId, bool changed_only, LanesVisualizationLaneUiStateBuilder&);

class LanesVisualizationCompiledSampleWindowBuilder {
    std::optional<CompiledSampleWindow> window_ {};

public:
    void succeed(CompiledSampleWindow window)
    {
        window_ = std::move(window);
    }

    [[nodiscard]] std::optional<CompiledSampleWindow> build()
    {
        return std::move(window_);
    }
};

using LanesVisualizationCompiledSampleWindowRequestedEvent =
    void (*)(LaneId, size_t first, size_t last, size_t point_count,
             LanesVisualizationCompiledSampleWindowBuilder&);

class LanesVisualizationCompiledEventWindowBuilder {
    std::vector<TimedEvent> events_ {};

public:
    void succeed(std::vector<TimedEvent> events)
    {
        events_ = std::move(events);
    }

    [[nodiscard]] std::vector<TimedEvent> build()
    {
        return std::move(events_);
    }
};

using LanesVisualizationCompiledEventWindowRequestedEvent =
    void (*)(LaneId, size_t first, size_t last,
             LanesVisualizationCompiledEventWindowBuilder&);

using LanesVisualizationTimelineBatchRequestedEvent =
    void (*)(TimelineLaneBatchUpdate const&);

using LaneViewContentUpdatedEvent =
    void (*)(LaneViewContentUpdate const&);

IV_DECLARE_LINKER_EVENT(
    LanesVisualizationLaneOutputQueryEvent,
    iv_runtime_lanes_visualization_lane_output_query_event);
IV_DECLARE_LINKER_EVENT(
    LanesVisualizationPlaybackPositionQueryEvent,
    iv_runtime_lanes_visualization_playback_position_query_event);
IV_DECLARE_LINKER_EVENT(
    LanesVisualizationLaneUiStateQueryEvent,
    iv_runtime_lanes_visualization_lane_ui_state_query_event);
IV_DECLARE_LINKER_EVENT(
    LanesVisualizationCompiledSampleWindowRequestedEvent,
    iv_runtime_lanes_visualization_compiled_sample_window_requested_event);
IV_DECLARE_LINKER_EVENT(
    LanesVisualizationCompiledEventWindowRequestedEvent,
    iv_runtime_lanes_visualization_compiled_event_window_requested_event);
IV_DECLARE_LINKER_EVENT(
    LanesVisualizationTimelineBatchRequestedEvent,
    iv_runtime_lanes_visualization_timeline_batch_requested_event);
IV_DECLARE_LINKER_EVENT(
    LaneViewContentUpdatedEvent,
    iv_runtime_lane_view_content_updated_event);

} // namespace iv
