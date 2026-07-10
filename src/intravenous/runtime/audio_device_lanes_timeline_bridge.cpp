#include <intravenous/runtime/audio_device_lanes_timeline_bridge.h>

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/audio_device_lanes_events.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_events.h>

#include <memory>

namespace iv {
namespace {
Timeline *bound_timeline = nullptr;
query::LaneQuerySchema last_schema;
std::uint64_t schema_revision = 0;

class TimelineLaneQueryDatasetView final : public query::LaneQueryDataset {
    Timeline *timeline_ = nullptr;
    query::LaneQuerySchema schema_snapshot_ {};
    std::vector<LaneId> lanes_ {};

public:
    TimelineLaneQueryDatasetView(
        Timeline &timeline,
        query::LaneQuerySchema schema_snapshot,
        std::vector<LaneId> lanes)
        : timeline_(&timeline),
          schema_snapshot_(std::move(schema_snapshot)),
          lanes_(std::move(lanes))
    {}

    query::LaneQuerySchema const &schema() const override
    {
        return schema_snapshot_;
    }

    size_t lane_count() const override
    {
        return lanes_.size();
    }

    std::uint64_t lane_id_at(size_t lane_index) const override
    {
        return lanes_.at(lane_index).value;
    }

    bool in_filter(size_t, std::string_view) const override
    {
        return false;
    }

    bool has_unit(size_t lane_index, query::LaneQueryPropertyId property) const override
    {
        return timeline_->lane_has_unit_metadata(lanes_.at(lane_index), schema_snapshot_.key_of(property));
    }

    std::optional<int> int_value(size_t lane_index, query::LaneQueryPropertyId property) const override
    {
        return timeline_->lane_int_metadata(lanes_.at(lane_index), schema_snapshot_.key_of(property));
    }

    std::optional<float> float_value(size_t lane_index, query::LaneQueryPropertyId property) const override
    {
        return timeline_->lane_float_metadata(lanes_.at(lane_index), schema_snapshot_.key_of(property));
    }
};

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
    std::uint64_t version_index,
    bool lane_set_changed,
    std::vector<LaneId> created_lanes = {},
    std::vector<LaneId> removed_lanes = {},
    std::vector<LaneId> changed_lanes = {})
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

    auto const schema = candidate_schema;
    auto const schema_change = diff;
    last_schema = schema;
    auto dataset = std::make_shared<TimelineLaneQueryDatasetView>(
        *bound_timeline,
        schema,
        bound_timeline->lane_ids());

    TimelineLanesChanged notification{
        .version_index = version_index,
        .lane_set_changed = lane_set_changed,
        .dataset = std::move(dataset),
        .schema_change = schema_change,
        .metadata_for_lane = [timeline = bound_timeline](LaneId lane) {
            return timeline->lane_metadata(lane);
        },
        .public_id_for_lane = [timeline = bound_timeline](LaneId lane) {
            return timeline->lane_public_id(lane);
        },
        .outputs_for_lanes = [timeline = bound_timeline](std::vector<LaneId> const &lanes) {
            return outputs_for_lanes(*timeline, lanes);
        },
        .visit_lanes = [timeline = bound_timeline](std::vector<LaneId> const &lanes, TimelineLaneVisitFn const &visit) {
            timeline->with_graph([&](LaneGraph const& graph) {
                for (auto const lane : lanes) {
                    auto const& record = graph.lane(lane);
                    visit(
                        lane,
                        record.node,
                        record.output,
                        record.sample_channel_type,
                        graph.inputs_for(lane),
                        record.external_task_dependencies);
                }
            });
        },
        .created_lanes = std::move(created_lanes),
        .removed_lanes = std::move(removed_lanes),
        .changed_lanes = std::move(changed_lanes),
    };
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_timeline_lanes_changed_event,
        notification);
}

void handle_timeline_batch_requested(TimelineLaneBatchUpdate const &batch)
{
    if (bound_timeline == nullptr) {
        return;
    }
    bound_timeline->apply_lane_batch(batch);
    std::vector<LaneId> upserted_lanes;
    upserted_lanes.reserve(batch.upserts.size());
    for (auto const &upsert : batch.upserts) {
        upserted_lanes.push_back(upsert.lane);
    }
    emit_lane_change(batch.version_index, true, std::move(upserted_lanes), batch.removals);
}

IV_SUBSCRIBE_LINKER_EVENT(
    AudioDeviceLanesTimelineBatchRequestedEvent,
    iv_runtime_audio_device_lanes_timeline_batch_requested_event,
    handle_timeline_batch_requested);
} // namespace

void bind_audio_device_lanes_timeline_bridge(AudioDeviceLanes &audio_device_lanes, Timeline &timeline)
{
    (void)audio_device_lanes;
    bound_timeline = &timeline;
    schema_revision = 0;
    last_schema = timeline.lane_query_schema(schema_revision);
}

void unbind_audio_device_lanes_timeline_bridge(
    AudioDeviceLanes const &,
    Timeline const &timeline)
{
    if (bound_timeline == &timeline) {
        bound_timeline = nullptr;
    }
}
} // namespace iv
