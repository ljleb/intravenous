#include "runtime/graph_input_lanes_events.h"

#include <stdexcept>
#include <utility>

namespace iv {
void RuntimeGraphInputLanesAckBuilder::succeed()
{
    handled = true;
    error_message.reset();
}

void RuntimeGraphInputLanesAckBuilder::fail(std::string message)
{
    handled = false;
    error_message = std::move(message);
}

void RuntimeGraphInputLanesAckBuilder::build() const
{
    if (error_message.has_value()) {
        throw std::runtime_error(*error_message);
    }
    if (!handled) {
        throw std::runtime_error("runtime graph input lanes event was not handled");
    }
}

void RuntimeGraphInputLanesLiveInputSnapshotsBuilder::succeed(
    std::vector<RuntimeGraphInputLanesLiveInputSnapshot> value)
{
    result = std::move(value);
}

std::vector<RuntimeGraphInputLanesLiveInputSnapshot>
RuntimeGraphInputLanesLiveInputSnapshotsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime graph input lane snapshots were not provided");
    }
    return *result;
}

void RuntimeGraphInputLanesLaneBindingsBuilder::succeed(
    GraphInputLaneBindings value)
{
    result = std::move(value);
}

GraphInputLaneBindings RuntimeGraphInputLanesLaneBindingsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime graph input lane bindings were not provided");
    }
    return *result;
}

void RuntimeGraphInputLanesLaneOutputsBuilder::succeed(
    std::vector<RuntimeGraphInputLanesLaneOutputs> value)
{
    result = std::move(value);
}

std::vector<RuntimeGraphInputLanesLaneOutputs>
RuntimeGraphInputLanesLaneOutputsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime graph input lane outputs were not provided");
    }
    return *result;
}

IV_DEFINE_LINKER_EVENT(
    RuntimeGraphInputLanesPortsChangedEvent,
    iv_runtime_graph_input_lanes_ports_changed_event);
IV_DEFINE_LINKER_EVENT(
    RuntimeGraphInputLanesLiveInputSnapshotsRequestedEvent,
    iv_runtime_graph_input_lanes_live_input_snapshots_requested_event);
IV_DEFINE_LINKER_EVENT(
    RuntimeGraphInputLanesLaneBindingsEnsuredEvent,
    iv_runtime_graph_input_lanes_lane_bindings_ensured_event);
IV_DEFINE_LINKER_EVENT(
    RuntimeGraphInputLanesLaneBindingsRequestedEvent,
    iv_runtime_graph_input_lanes_lane_bindings_requested_event);
IV_DEFINE_LINKER_EVENT(
    RuntimeGraphInputLanesLaneOutputsRequestedEvent,
    iv_runtime_graph_input_lanes_lane_outputs_requested_event);
IV_DEFINE_LINKER_EVENT(
    RuntimeGraphInputLanesSetSampleInputValueRequestedEvent,
    iv_runtime_graph_input_lanes_set_sample_input_value_requested_event);
IV_DEFINE_LINKER_EVENT(
    RuntimeGraphInputLanesClearSampleInputValueOverrideRequestedEvent,
    iv_runtime_graph_input_lanes_clear_sample_input_value_override_requested_event);
IV_DEFINE_LINKER_EVENT(
    RuntimeGraphInputLanesLaneViewUpdatedEvent,
    iv_runtime_graph_input_lanes_lane_view_updated_event);
} // namespace iv
