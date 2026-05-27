#include "runtime/graph_input_lanes_events.h"

#include <stdexcept>
#include <utility>

namespace iv {
void GraphInputLanesAckBuilder::succeed()
{
    handled = true;
    error_message.reset();
}

void GraphInputLanesAckBuilder::fail(std::string message)
{
    handled = false;
    error_message = std::move(message);
}

void GraphInputLanesAckBuilder::build() const
{
    if (error_message.has_value()) {
        throw std::runtime_error(*error_message);
    }
    if (!handled) {
        throw std::runtime_error("runtime graph input lanes event was not handled");
    }
}

void GraphInputLanesLiveInputSnapshotsBuilder::succeed(
    std::vector<GraphInputLanesLiveInputSnapshot> value)
{
    result = std::move(value);
}

std::vector<GraphInputLanesLiveInputSnapshot>
GraphInputLanesLiveInputSnapshotsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime graph input lane snapshots were not provided");
    }
    return *result;
}

void GraphInputLanesLaneBindingsBuilder::succeed(
    GraphInputLaneBindings value)
{
    result = std::move(value);
}

GraphInputLaneBindings GraphInputLanesLaneBindingsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime graph input lane bindings were not provided");
    }
    return *result;
}

void GraphInputLanesLaneOutputsBuilder::succeed(
    std::vector<GraphInputLanesLaneOutputs> value)
{
    result = std::move(value);
}

std::vector<GraphInputLanesLaneOutputs>
GraphInputLanesLaneOutputsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime graph input lane outputs were not provided");
    }
    return *result;
}

void GraphInputLanesSampleInputLaneRefBuilder::succeed(RealtimeLaneRef value)
{
    result = std::move(value);
}

RealtimeLaneRef GraphInputLanesSampleInputLaneRefBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime graph input lane ref was not provided");
    }
    return *result;
}

IV_DEFINE_LINKER_EVENT(
    GraphInputLanesPortsChangedEvent,
    iv_runtime_graph_input_lanes_ports_changed_event);
IV_DEFINE_LINKER_EVENT(
    GraphInputLanesLiveInputSnapshotsRequestedEvent,
    iv_runtime_graph_input_lanes_live_input_snapshots_requested_event);
IV_DEFINE_LINKER_EVENT(
    GraphInputLanesLaneBindingsEnsuredEvent,
    iv_runtime_graph_input_lanes_lane_bindings_ensured_event);
IV_DEFINE_LINKER_EVENT(
    GraphInputLanesLaneBindingsRequestedEvent,
    iv_runtime_graph_input_lanes_lane_bindings_requested_event);
IV_DEFINE_LINKER_EVENT(
    GraphInputLanesLaneOutputsRequestedEvent,
    iv_runtime_graph_input_lanes_lane_outputs_requested_event);
IV_DEFINE_LINKER_EVENT(
    GraphInputLanesSetSampleInputValueRequestedEvent,
    iv_runtime_graph_input_lanes_set_sample_input_value_requested_event);
IV_DEFINE_LINKER_EVENT(
    GraphInputLanesClearSampleInputValueOverrideRequestedEvent,
    iv_runtime_graph_input_lanes_clear_sample_input_value_override_requested_event);
IV_DEFINE_LINKER_EVENT(
    GraphInputLanesSampleInputLaneRefRequestedEvent,
    iv_runtime_graph_input_lanes_sample_input_lane_ref_requested_event);
IV_DEFINE_LINKER_EVENT(
    GraphInputLanesLaneViewUpdatedEvent,
    iv_runtime_graph_input_lanes_lane_view_updated_event);
} // namespace iv
