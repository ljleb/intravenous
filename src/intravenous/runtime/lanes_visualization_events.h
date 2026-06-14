#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/lanes_visualization_api_types.h>
#include <intravenous/runtime/timeline_events.h>

#include <optional>
#include <vector>

namespace iv {

// ---- Lane output config query ----

class LanesVisualizationLaneOutputQueryBuilder {
    std::optional<LaneOutputConfig> result_ {};

public:
    void succeed(LaneOutputConfig config)
    {
        result_ = std::move(config);
    }

    [[nodiscard]] std::optional<LaneOutputConfig> build()
    {
        return std::move(result_);
    }
};

using LanesVisualizationLaneOutputQueryEvent =
    void (*)(LaneId, LanesVisualizationLaneOutputQueryBuilder&);

// ---- Compiled sample window query ----

class LanesVisualizationCompiledSampleWindowBuilder {
    std::vector<Sample> samples_ {};

public:
    void succeed(std::vector<Sample> samples)
    {
        samples_ = std::move(samples);
    }

    [[nodiscard]] std::vector<Sample> build()
    {
        return std::move(samples_);
    }
};

using LanesVisualizationCompiledSampleWindowRequestedEvent =
    void (*)(LaneId, size_t first, size_t last, size_t count,
             LanesVisualizationCompiledSampleWindowBuilder&);

// ---- Compiled event range query ----

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

// ---- Timeline batch request ----

using LanesVisualizationTimelineBatchRequestedEvent =
    void (*)(TimelineLaneBatchUpdate const&);

// ---- Content update notification ----

using LaneViewContentUpdatedEvent =
    void (*)(LaneViewContentUpdate const&);

IV_DECLARE_LINKER_EVENT(
    LanesVisualizationLaneOutputQueryEvent,
    iv_runtime_lanes_visualization_lane_output_query_event);
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
