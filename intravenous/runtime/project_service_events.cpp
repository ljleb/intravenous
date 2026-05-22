#include "runtime/project_service_events.h"

#include <stdexcept>
#include <utility>

namespace iv {
void RuntimeProjectAckBuilder::succeed()
{
    handled = true;
    error_message.reset();
}

void RuntimeProjectAckBuilder::fail(std::string message)
{
    handled = false;
    error_message = std::move(message);
}

void RuntimeProjectAckBuilder::build() const
{
    if (error_message.has_value()) {
        throw std::runtime_error(*error_message);
    }
    if (!handled) {
        throw std::runtime_error("runtime project event was not handled");
    }
}

void RuntimeProjectLiveInputSnapshotsBuilder::succeed(
    std::vector<RuntimeProjectLiveInputSnapshot> value)
{
    result = std::move(value);
}

std::vector<RuntimeProjectLiveInputSnapshot>
RuntimeProjectLiveInputSnapshotsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime project live input snapshots were not provided");
    }
    return *result;
}

void RuntimeProjectGraphInputLaneBindingsBuilder::succeed(
    GraphInputLaneBindings value)
{
    result = std::move(value);
}

GraphInputLaneBindings RuntimeProjectGraphInputLaneBindingsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime project graph input lane bindings were not provided");
    }
    return *result;
}

void RuntimeProjectLaneOutputsBuilder::succeed(
    std::vector<RuntimeProjectLaneOutputs> value)
{
    result = std::move(value);
}

std::vector<RuntimeProjectLaneOutputs>
RuntimeProjectLaneOutputsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime project lane outputs were not provided");
    }
    return *result;
}

IV_DEFINE_LINKER_EVENT(
    RuntimeProjectNotificationEvent,
    iv_runtime_project_notification_event)
IV_DEFINE_LINKER_EVENT(
    RuntimeProjectLiveInputSnapshotsRequestedEvent,
    iv_runtime_project_live_input_snapshots_requested_event)
IV_DEFINE_LINKER_EVENT(
    RuntimeProjectGraphInputLaneBindingsEnsuredEvent,
    iv_runtime_project_graph_input_lane_bindings_ensured_event)
IV_DEFINE_LINKER_EVENT(
    RuntimeProjectGraphInputLaneBindingsRequestedEvent,
    iv_runtime_project_graph_input_lane_bindings_requested_event)
IV_DEFINE_LINKER_EVENT(
    RuntimeProjectLaneOutputsRequestedEvent,
    iv_runtime_project_lane_outputs_requested_event)
IV_DEFINE_LINKER_EVENT(
    RuntimeProjectSetSampleInputValueRequestedEvent,
    iv_runtime_project_set_sample_input_value_requested_event)
IV_DEFINE_LINKER_EVENT(
    RuntimeProjectClearSampleInputValueOverrideRequestedEvent,
    iv_runtime_project_clear_sample_input_value_override_requested_event)
} // namespace iv
