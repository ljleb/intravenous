#include <intravenous/runtime/lanes_visualization_timeline_bridge.h>

#include <intravenous/runtime/lanes_visualization_events.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_events.h>

#include <memory>
#include <utility>

namespace iv {
namespace {
LanesVisualization *bound_visualization = nullptr;
Timeline *bound_timeline = nullptr;
query::LaneQuerySchema last_schema;
std::uint64_t schema_revision = 0;

std::vector<TimelineLaneOutputs> outputs_for_lanes(
    Timeline &timeline,
    std::vector<LaneId> const &lanes)
{
    std::vector<TimelineLaneOutputs> results;
    results.reserve(lanes.size());
    for (auto const lane : lanes) {
        results.push_back(TimelineLaneOutputs{
            .lane = lane,
            .outputs = timeline.lane_outputs_for(lane),
        });
    }
    return results;
}

void emit_lane_change(
    bool lane_set_changed,
    std::vector<LaneId> created_lanes = {},
    std::vector<LaneId> removed_lanes = {})
{
    if (bound_timeline == nullptr) {
        return;
    }
    auto candidate_schema = bound_timeline->lane_query_schema(schema_revision + 1);
    auto diff = query::diff_lane_query_schemas(last_schema, candidate_schema);
    if (!diff.changed) {
        candidate_schema = bound_timeline->lane_query_schema(schema_revision);
        diff = query::diff_lane_query_schemas(last_schema, candidate_schema);
    } else {
        schema_revision += 1;
    }
    last_schema = candidate_schema;
    auto dataset = std::make_shared<query::LaneQuerySchema>(candidate_schema);
    (void)dataset; // dataset is built by the timeline bridge; we reuse emit_lane_change pattern

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_timeline_lanes_changed_event,
        TimelineLanesChanged{
            .lane_set_changed = lane_set_changed,
            .dataset = nullptr,
            .schema_change = diff,
            .metadata_for_lane = [timeline = bound_timeline](LaneId lane) {
                return timeline->lane_metadata(lane);
            },
            .outputs_for_lanes = [timeline = bound_timeline](std::vector<LaneId> const &lanes) {
                return outputs_for_lanes(*timeline, lanes);
            },
            .visit_lanes = [timeline = bound_timeline](std::vector<LaneId> const &lanes, TimelineLaneVisitFn const &visit) {
                timeline->with_graph([&](LaneGraph const& graph) {
                    for (auto const lane : lanes) {
                        if (!graph.contains(lane)) {
                            continue;
                        }
                        auto const& record = graph.lane(lane);
                        visit(
                            lane,
                            record.node,
                            record.output,
                            graph.inputs_for(lane),
                            record.external_task_dependencies);
                    }
                });
            },
            .created_lanes = std::move(created_lanes),
            .removed_lanes = std::move(removed_lanes),
        });
}

void handle_timeline_batch_requested(TimelineLaneBatchUpdate const &batch)
{
    if (bound_timeline == nullptr) {
        return;
    }
    std::vector<LaneId> created;
    created.reserve(batch.upserts.size());
    for (auto const &upsert : batch.upserts) {
        created.push_back(upsert.lane);
    }
    bound_timeline->apply_lane_batch(batch);
    emit_lane_change(true, std::move(created), batch.removals);
}

void handle_lane_output_query(
    LaneId lane,
    LanesVisualizationLaneOutputQueryBuilder &builder)
{
    if (bound_timeline == nullptr) {
        return;
    }
    bool found = false;
    LaneOutputConfig config;
    bound_timeline->with_graph([&](LaneGraph const& graph) {
        if (!graph.contains(lane)) {
            return;
        }
        config = graph.lane(lane).output;
        found = true;
    });
    if (found) {
        builder.succeed(std::move(config));
    }
}

IV_SUBSCRIBE_LINKER_EVENT(
    LanesVisualizationTimelineBatchRequestedEvent,
    iv_runtime_lanes_visualization_timeline_batch_requested_event,
    handle_timeline_batch_requested);
IV_SUBSCRIBE_LINKER_EVENT(
    LanesVisualizationLaneOutputQueryEvent,
    iv_runtime_lanes_visualization_lane_output_query_event,
    handle_lane_output_query);
} // namespace

void bind_lanes_visualization_timeline_bridge(
    LanesVisualization &visualization,
    Timeline &timeline)
{
    bound_visualization = &visualization;
    bound_timeline = &timeline;
    schema_revision = 0;
    last_schema = timeline.lane_query_schema(schema_revision);
}

void unbind_lanes_visualization_timeline_bridge(
    LanesVisualization const &visualization,
    Timeline const &timeline)
{
    if (bound_visualization == &visualization) {
        bound_visualization = nullptr;
    }
    if (bound_timeline == &timeline) {
        bound_timeline = nullptr;
    }
}
} // namespace iv
