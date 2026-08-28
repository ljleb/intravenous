#include <intravenous/runtime/timeline_lane_batch_bridge.h>

#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_events.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <memory>

namespace iv {
namespace {
Timeline *bound_timeline = nullptr;

class TimelineLaneQueryDatasetView final : public query::LaneQueryDataset {
    Timeline *timeline_;
    query::LaneQuerySchema schema_;
    std::vector<LaneId> lanes_;
public:
    TimelineLaneQueryDatasetView(Timeline &timeline, query::LaneQuerySchema schema, std::vector<LaneId> lanes)
        : timeline_(&timeline), schema_(std::move(schema)), lanes_(std::move(lanes)) {}
    query::LaneQuerySchema const &schema() const override { return schema_; }
    size_t lane_count() const override { return lanes_.size(); }
    std::uint64_t lane_id_at(size_t index) const override { return lanes_.at(index).value; }
    bool in_filter(size_t, std::string_view) const override { return false; }
    bool has_unit(size_t index, query::LaneQueryPropertyId property) const override {
        return timeline_->lane_has_unit_metadata(lanes_.at(index), schema_.key_of(property));
    }
    std::optional<int> int_value(size_t index, query::LaneQueryPropertyId property) const override {
        return timeline_->lane_int_metadata(lanes_.at(index), schema_.key_of(property));
    }
    std::optional<float> float_value(size_t index, query::LaneQueryPropertyId property) const override {
        return timeline_->lane_float_metadata(lanes_.at(index), schema_.key_of(property));
    }
};

std::vector<TimelineLaneOutputs> outputs_for_lanes(Timeline &timeline, std::vector<LaneId> const &lanes)
{
    std::vector<TimelineLaneOutputs> result;
    result.reserve(lanes.size());
    for (auto lane : lanes) result.push_back({.lane = lane, .outputs = timeline.lane_outputs_for(lane)});
    return result;
}

void handle_timeline_lane_batch(TimelineLaneBatchUpdate const &batch)
{
    if (bound_timeline == nullptr) return;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectMessageNotification{
            .level = "debug",
            .message = "timeline batch received: upserts=" + std::to_string(batch.upserts.size())
                + " removals=" + std::to_string(batch.removals.size()),
        }));
    bound_timeline->apply_lane_batch(batch);
    auto [candidate_schema, schema_change] =
        bound_timeline->reconcile_lane_query_schema();
    std::vector<LaneId> created;
    created.reserve(batch.upserts.size());
    for (auto const &upsert : batch.upserts) created.push_back(upsert.lane);
    TimelineLanesChanged change{
        .version_index = batch.version_index,
        .lane_set_changed = true,
        .dataset = std::make_shared<TimelineLaneQueryDatasetView>(*bound_timeline, candidate_schema, bound_timeline->persistent_lane_ids()),
        .schema_change = schema_change,
        .metadata_for_lane = [timeline = bound_timeline](LaneId lane) { return timeline->lane_metadata(lane); },
        .model_type_id_for_lane = [timeline = bound_timeline](LaneId lane) { return timeline->lane_model_type_id(lane); },
        .public_id_for_lane = [timeline = bound_timeline](LaneId lane) { return timeline->lane_public_id(lane); },
        .outputs_for_lanes = [timeline = bound_timeline](std::vector<LaneId> const &lanes) { return outputs_for_lanes(*timeline, lanes); },
        .visit_lanes = [timeline = bound_timeline](std::vector<LaneId> const &lanes, TimelineLaneVisitFn const &visit) {
            timeline->with_graph([&](LaneGraph const &graph) {
                for (auto lane : lanes) {
                    if (!graph.contains(lane)) continue;
                    auto const &record = graph.lane(lane);
                    visit(lane, record.node, record.output, record.sample_channel_type, graph.inputs_for(lane), record.external_task_dependencies);
                }
            });
        },
        .created_lanes = std::move(created),
        .removed_lanes = batch.removals,
    };
    IV_INVOKE_LINKER_EVENT(iv_runtime_timeline_lanes_changed_event, change);
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectMessageNotification{
            .level = "debug",
            .message = "timeline batch published lane set: persistent="
                + std::to_string(bound_timeline->persistent_lane_ids().size())
                + " created=" + std::to_string(change.created_lanes.size())
                + " removed=" + std::to_string(change.removed_lanes.size()),
        }));
}

IV_SUBSCRIBE_LINKER_EVENT(TimelineLaneBatchRequestedEvent, iv_runtime_timeline_lane_batch_requested_event, handle_timeline_lane_batch);
} // namespace

void bind_timeline_lane_batch_bridge(Timeline &timeline)
{
    bound_timeline = &timeline;
    timeline.reset_lane_query_schema();
}

void unbind_timeline_lane_batch_bridge(Timeline const &timeline)
{
    if (bound_timeline == &timeline) bound_timeline = nullptr;
}
} // namespace iv
