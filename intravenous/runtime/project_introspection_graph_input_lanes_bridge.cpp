#include "runtime/project_introspection_graph_input_lanes_bridge.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/project_introspection_events.h"

namespace iv {
namespace {
RuntimeGraphInputLanes *bound_lanes = nullptr;

void handle_live_input_snapshots_requested(
    std::vector<RuntimeProjectIntrospectionLiveInputSnapshotRequest> const &requests,
    RuntimeProjectIntrospectionLiveInputSnapshotsBuilder &builder)
{
    if (bound_lanes == nullptr) {
        return;
    }
    builder.succeed(bound_lanes->collect_live_input_snapshots(requests));
}

IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeProjectIntrospectionLiveInputSnapshotsRequestedEvent,
    iv_runtime_project_introspection_live_input_snapshots_requested_event,
    handle_live_input_snapshots_requested);
} // namespace

void bind_project_introspection_graph_input_lanes_bridge(RuntimeGraphInputLanes &lanes)
{
    bound_lanes = &lanes;
}

void unbind_project_introspection_graph_input_lanes_bridge(
    RuntimeGraphInputLanes const &lanes)
{
    if (bound_lanes == &lanes) {
        bound_lanes = nullptr;
    }
}
} // namespace iv
