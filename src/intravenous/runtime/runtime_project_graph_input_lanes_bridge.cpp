#include <intravenous/runtime/runtime_project_graph_input_lanes_bridge.h>

#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
namespace {
GraphInputLanes *bound_graph_input_lanes = nullptr;

void handle_set_sample_input_value(
    ProjectSetSampleInputValueRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_graph_input_lanes == nullptr) {
        return;
    }
    if (auto const public_input = parse_public_sample_input_node_id(request.node_id)) {
        bound_graph_input_lanes->set_public_sample_input_value(
            public_input->first, public_input->second, request.value);
        builder.succeed();
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
        return;
    }
    bound_graph_input_lanes->set_sample_input_value(request);
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_set_sample_input_state(
    ProjectSetSampleInputStateRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_graph_input_lanes == nullptr) {
        return;
    }
    if (auto const public_input = parse_public_sample_input_node_id(request.node_id)) {
        bound_graph_input_lanes->set_public_sample_input_state(ProjectSetPublicSampleInputStateRequest{
            .instance_id = public_input->first,
            .source_identity = public_input->second,
            .member_ordinal = request.member_ordinal,
            .state = request.state,
            .lane_id = request.lane_id,
        });
        builder.succeed();
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
        return;
    }
    bound_graph_input_lanes->set_sample_input_state(request);
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_set_public_sample_input_state(
    ProjectSetPublicSampleInputStateRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_graph_input_lanes == nullptr) return;
    bound_graph_input_lanes->set_public_sample_input_state(request);
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_set_public_sample_input_value(
    std::string const &instance_id,
    std::string const &source_identity,
    Sample value,
    ProjectAckBuilder &builder)
{
    if (bound_graph_input_lanes == nullptr) return;
    bound_graph_input_lanes->set_public_sample_input_value(instance_id, source_identity, value);
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_set_event_input_state(
    ProjectSetEventInputStateRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_graph_input_lanes == nullptr) {
        return;
    }
    if (auto const public_input = parse_public_sample_input_node_id(request.node_id)) {
        bound_graph_input_lanes->set_public_event_input_state(
            public_input->first, public_input->second, request.member_ordinal,
            request.state, request.lane_id);
        builder.succeed();
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
        return;
    }
    bound_graph_input_lanes->set_event_input_state(request);
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_set_sample_output_state(
    ProjectSetSampleOutputStateRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_graph_input_lanes == nullptr) {
        return;
    }
    bound_graph_input_lanes->set_sample_output_state(request);
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_set_event_output_state(
    ProjectSetEventOutputStateRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_graph_input_lanes == nullptr) {
        return;
    }
    bound_graph_input_lanes->set_event_output_state(request);
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

IV_SUBSCRIBE_LINKER_EVENT(
    ProjectSetSampleInputValueRequestedEvent,
    iv_runtime_project_set_sample_input_value_requested_event,
    handle_set_sample_input_value);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectSetSampleInputStateRequestedEvent,
    iv_runtime_project_set_sample_input_state_requested_event,
    handle_set_sample_input_state);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectSetPublicSampleInputStateRequestedEvent,
    iv_runtime_project_set_public_sample_input_state_requested_event,
    handle_set_public_sample_input_state);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectSetPublicSampleInputValueRequestedEvent,
    iv_runtime_project_set_public_sample_input_value_requested_event,
    handle_set_public_sample_input_value);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectSetEventInputStateRequestedEvent,
    iv_runtime_project_set_event_input_state_requested_event,
    handle_set_event_input_state);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectSetSampleOutputStateRequestedEvent,
    iv_runtime_project_set_sample_output_state_requested_event,
    handle_set_sample_output_state);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectSetEventOutputStateRequestedEvent,
    iv_runtime_project_set_event_output_state_requested_event,
    handle_set_event_output_state);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectPersistenceCollectStateEvent,
    iv_runtime_project_persistence_collect_state_event,
    [](ProjectPersistenceBuilder &builder) {
        if (bound_graph_input_lanes == nullptr) {
            return;
        }
        builder.add_graph_input_authored_state(bound_graph_input_lanes->authored_state());
    });
} // namespace

void bind_runtime_project_graph_input_lanes_bridge(GraphInputLanes &graph_input_lanes)
{
    bound_graph_input_lanes = &graph_input_lanes;
}

void unbind_runtime_project_graph_input_lanes_bridge(
    GraphInputLanes const &graph_input_lanes)
{
    if (bound_graph_input_lanes == &graph_input_lanes) {
        bound_graph_input_lanes = nullptr;
    }
}
} // namespace iv
