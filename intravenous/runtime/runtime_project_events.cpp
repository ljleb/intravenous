#include "runtime/runtime_project_events.h"

#include <stdexcept>
#include <utility>

namespace iv {
void ProjectAckBuilder::succeed()
{
    handled = true;
    error_message.reset();
}

void ProjectAckBuilder::fail(std::string message)
{
    handled = false;
    error_message = std::move(message);
}

void ProjectAckBuilder::build() const
{
    if (error_message.has_value()) {
        throw std::runtime_error(*error_message);
    }
    if (!handled) {
        throw std::runtime_error("runtime project event was not handled");
    }
}

void ProjectLiveInputSnapshotsBuilder::succeed(
    std::vector<ProjectLiveInputSnapshot> value)
{
    result = std::move(value);
}

std::vector<ProjectLiveInputSnapshot>
ProjectLiveInputSnapshotsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime project live input snapshots were not provided");
    }
    return *result;
}

void ProjectGraphInputLaneBindingsBuilder::succeed(
    GraphInputLaneBindings value)
{
    result = std::move(value);
}

GraphInputLaneBindings ProjectGraphInputLaneBindingsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime project graph input lane bindings were not provided");
    }
    return *result;
}

void ProjectLaneOutputsBuilder::succeed(
    std::vector<ProjectLaneOutputs> value)
{
    result = std::move(value);
}

std::vector<ProjectLaneOutputs>
ProjectLaneOutputsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime project lane outputs were not provided");
    }
    return *result;
}

IV_DEFINE_LINKER_EVENT(
    ProjectNotificationEvent,
    iv_runtime_project_notification_event)
IV_DEFINE_LINKER_EVENT(
    ProjectLiveInputSnapshotsRequestedEvent,
    iv_runtime_project_live_input_snapshots_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphInputLaneBindingsEnsuredEvent,
    iv_runtime_project_graph_input_lane_bindings_ensured_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphInputLaneBindingsRequestedEvent,
    iv_runtime_project_graph_input_lane_bindings_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectLaneOutputsRequestedEvent,
    iv_runtime_project_lane_outputs_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectSetSampleInputValueRequestedEvent,
    iv_runtime_project_set_sample_input_value_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectClearSampleInputValueOverrideRequestedEvent,
    iv_runtime_project_clear_sample_input_value_override_requested_event)
} // namespace iv
