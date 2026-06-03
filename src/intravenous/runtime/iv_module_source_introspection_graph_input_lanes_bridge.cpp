#include <intravenous/runtime/iv_module_source_introspection_graph_input_lanes_bridge.h>

#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>

namespace iv {
namespace {
GraphInputLanes *bound_lanes = nullptr;

void handle_live_input_snapshots_requested(
    std::vector<IvModuleSourceIntrospectionLiveInputSnapshotRequest> const &requests,
    IvModuleSourceIntrospectionLiveInputSnapshotsBuilder &builder)
{
    if (bound_lanes == nullptr) {
        return;
    }
    builder.succeed(bound_lanes->collect_live_input_snapshots(requests));
}

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleSourceIntrospectionLiveInputSnapshotsRequestedEvent,
    iv_runtime_iv_module_source_introspection_live_input_snapshots_requested_event,
    handle_live_input_snapshots_requested);
} // namespace

void bind_iv_module_source_introspection_graph_input_lanes_bridge(GraphInputLanes &lanes)
{
    bound_lanes = &lanes;
}

void unbind_iv_module_source_introspection_graph_input_lanes_bridge(
    GraphInputLanes const &lanes)
{
    if (bound_lanes == &lanes) {
        bound_lanes = nullptr;
    }
}
} // namespace iv
