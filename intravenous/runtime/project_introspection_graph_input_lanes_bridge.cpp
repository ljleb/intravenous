#include "runtime/project_introspection_graph_input_lanes_bridge.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/project_introspection_events.h"

namespace iv {
namespace {
GraphInputLanes *bound_lanes = nullptr;

void handle_live_input_snapshots_requested(
    std::vector<ProjectIntrospectionLiveInputSnapshotRequest> const &requests,
    ProjectIntrospectionLiveInputSnapshotsBuilder &builder)
{
    if (bound_lanes == nullptr) {
        return;
    }
    builder.succeed(bound_lanes->collect_live_input_snapshots(requests));
}

IV_SUBSCRIBE_LINKER_EVENT(
    ProjectIntrospectionLiveInputSnapshotsRequestedEvent,
    iv_runtime_project_introspection_live_input_snapshots_requested_event,
    handle_live_input_snapshots_requested);
} // namespace

void bind_project_introspection_graph_input_lanes_bridge(GraphInputLanes &lanes)
{
    bound_lanes = &lanes;
}

void unbind_project_introspection_graph_input_lanes_bridge(
    GraphInputLanes const &lanes)
{
    if (bound_lanes == &lanes) {
        bound_lanes = nullptr;
    }
}
} // namespace iv
