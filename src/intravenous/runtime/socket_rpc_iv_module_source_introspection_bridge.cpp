#include <intravenous/runtime/socket_rpc_iv_module_source_introspection_bridge.h>

#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <ranges>

namespace iv {
namespace {
IvModuleSourceIntrospection *bound_introspection = nullptr;
GraphInputLanes *bound_graph_input_lanes = nullptr;
std::function<void()> *bound_shutdown = nullptr;

void refresh_public_inputs()
{
    if (bound_introspection == nullptr || bound_graph_input_lanes == nullptr) return;
    bound_introspection->set_public_sample_inputs(bound_graph_input_lanes->public_sample_inputs());
    bound_introspection->set_public_event_inputs(bound_graph_input_lanes->public_event_inputs());
}

void notify_updated_node_ids(std::vector<std::string> node_ids)
{
    if (bound_introspection == nullptr || node_ids.empty()) {
        return;
    }
    try {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_source_introspection_nodes_updated_event,
            ProjectLogicalNodesNotification{
                .nodes = bound_introspection->get_logical_nodes(std::move(node_ids)),
            });
    } catch (...) {
    }
}

void notify_replaced_instances(IvModuleInstanceBuildersChanged const &diff)
{
    if (bound_introspection == nullptr) {
        return;
    }

    std::vector<std::string> replace_instance_ids;
    std::vector<IvModuleInstanceInfo> instances;

    auto append_instance = [&](IvModuleInstanceBuilderRef const &ref) {
        auto const *instance = ref.instance;
        if (instance == nullptr) {
            return;
        }
        if (std::ranges::find(replace_instance_ids, instance->instance_id) != replace_instance_ids.end()) {
            return;
        }
        replace_instance_ids.push_back(instance->instance_id);
        instances.push_back(IvModuleInstanceInfo{
            .instance_id = instance->instance_id,
            .definition_id = instance->definition_id,
            .module_root = instance->module_root,
            .default_silence_ttl_samples = instance->default_silence_ttl_samples,
            .realized = true,
            .module_id = instance->module_id,
        });
    };

    for (auto const &created : diff.created) {
        append_instance(created);
    }
    for (auto const &updated : diff.updated) {
        append_instance(updated);
    }
    for (auto const &deleted_instance_id : diff.deleted_instance_ids) {
        if (std::ranges::find(replace_instance_ids, deleted_instance_id) == replace_instance_ids.end()) {
            replace_instance_ids.push_back(deleted_instance_id);
        }
    }

    if (replace_instance_ids.empty()) {
        return;
    }

    try {
        bound_introspection->replace_public_input_instances(replace_instance_ids);
        if (bound_graph_input_lanes != nullptr) {
            bound_introspection->set_public_sample_inputs(
                bound_graph_input_lanes->public_sample_inputs());
            bound_introspection->set_public_event_inputs(
                bound_graph_input_lanes->public_event_inputs());
        }
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_source_introspection_nodes_updated_event,
            ProjectLogicalNodesNotification{
                .nodes = bound_introspection->get_logical_nodes_for_instances(instances),
                .replace_instance_ids = std::move(replace_instance_ids),
            });
    } catch (...) {
    }
}

ProjectSampleInputState parse_project_sample_input_state(std::string const &state)
{
    if (state == "default") {
        return ProjectSampleInputState::default_;
    }
    if (state == "overridden") {
        return ProjectSampleInputState::overridden;
    }
    if (state == "logicalFollow") {
        return ProjectSampleInputState::logical_follow;
    }
    if (state == "timelineLane") {
        return ProjectSampleInputState::timeline_lane;
    }
    if (state == "disconnected") {
        return ProjectSampleInputState::disconnected;
    }
    throw std::runtime_error("unknown sample input state: " + state);
}

ProjectEventInputState parse_project_event_input_state(std::string const &state)
{
    if (state == "default") {
        return ProjectEventInputState::default_;
    }
    if (state == "logicalFollow") {
        return ProjectEventInputState::logical_follow;
    }
    if (state == "timelineLane") {
        return ProjectEventInputState::timeline_lane;
    }
    if (state == "disconnected") {
        return ProjectEventInputState::disconnected;
    }
    throw std::runtime_error("unknown event input state: " + state);
}

ProjectSampleOutputState parse_project_sample_output_state(std::string const &state)
{
    if (state == "disconnected") {
        return ProjectSampleOutputState::disconnected;
    }
    if (state == "logical") {
        return ProjectSampleOutputState::logical;
    }
    if (state == "timelineLane") {
        return ProjectSampleOutputState::timeline_lane;
    }
    throw std::runtime_error("unknown sample output state: " + state);
}

ProjectEventOutputState parse_project_event_output_state(std::string const &state)
{
    if (state == "disconnected") {
        return ProjectEventOutputState::disconnected;
    }
    if (state == "logical") {
        return ProjectEventOutputState::logical;
    }
    if (state == "timelineLane") {
        return ProjectEventOutputState::timeline_lane;
    }
    throw std::runtime_error("unknown event output state: " + state);
}

void handle_graph_query_by_spans(
    GraphQueryBySpansRequest const &request,
    SocketRpcGraphQueryResultBuilder &builder)
{
    if (bound_introspection == nullptr) {
        return;
    }
    builder.succeed(bound_introspection->query_by_spans(
        request.file_path,
        request.ranges,
        request.match_mode,
        request.instance_id));
}

void handle_graph_query_active_regions(
    GraphQueryActiveRegionsRequest const &request,
    SocketRpcRegionQueryResultBuilder &builder)
{
    if (bound_introspection == nullptr) {
        return;
    }
    builder.succeed(bound_introspection->query_active_regions(request.file_path));
}

void handle_get_logical_node(
    GetLogicalNodeRequest const &request,
    SocketRpcLogicalNodeResultBuilder &builder)
{
    if (bound_introspection == nullptr) {
        return;
    }
    builder.succeed(bound_introspection->get_logical_node(request.node_id));
}

void handle_get_logical_nodes(
    GetLogicalNodesRequest const &request,
    SocketRpcLogicalNodesResultBuilder &builder)
{
    if (bound_introspection == nullptr) {
        return;
    }
    builder.succeed(bound_introspection->get_logical_nodes(request.node_ids));
}

void handle_set_sample_input_value(
    SetSampleInputValueRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        if (auto const public_input = parse_public_sample_input_node_id(request.node_id)) {
            ProjectAckBuilder project_builder;
            IV_INVOKE_LINKER_EVENT(
                iv_runtime_project_set_public_sample_input_value_requested_event,
                public_input->first,
                public_input->second,
                request.value,
                project_builder);
            project_builder.build();
            refresh_public_inputs();
            notify_updated_node_ids({request.node_id});
            return;
        }
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_sample_input_value_requested_event,
            ProjectSetSampleInputValueRequest{
                .node_id = request.node_id,
                .member_ordinal = request.member_ordinal,
                .input_ordinal = request.input_ordinal,
                .value = request.value,
            },
            project_builder);
        project_builder.build();
        refresh_public_inputs();
        notify_updated_node_ids({request.node_id});
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

void handle_set_sample_input_state(
    SetSampleInputStateRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        if (auto const public_input = parse_public_sample_input_node_id(request.node_id)) {
            ProjectAckBuilder project_builder;
            IV_INVOKE_LINKER_EVENT(
                iv_runtime_project_set_public_sample_input_state_requested_event,
                ProjectSetPublicSampleInputStateRequest{
                    .instance_id = public_input->first,
                    .source_identity = public_input->second,
                    .member_ordinal = request.member_ordinal,
                    .state = parse_project_sample_input_state(request.state),
                },
                project_builder);
            project_builder.build();
            refresh_public_inputs();
            notify_updated_node_ids({request.node_id});
            return;
        }
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_sample_input_state_requested_event,
            ProjectSetSampleInputStateRequest{
                .node_id = request.node_id,
                .member_ordinal = request.member_ordinal,
                .input_ordinal = request.input_ordinal,
                .state = parse_project_sample_input_state(request.state),
            },
            project_builder);
        project_builder.build();
        refresh_public_inputs();
        notify_updated_node_ids({request.node_id});
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

void handle_set_event_input_state(
    SetEventInputStateRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_event_input_state_requested_event,
            ProjectSetEventInputStateRequest{
                .node_id = request.node_id,
                .member_ordinal = request.member_ordinal,
                .input_ordinal = request.input_ordinal,
                .state = parse_project_event_input_state(request.state),
            },
            project_builder);
        project_builder.build();
        refresh_public_inputs();
        notify_updated_node_ids({request.node_id});
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

void handle_set_sample_output_state(
    SetSampleOutputStateRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_sample_output_state_requested_event,
            ProjectSetSampleOutputStateRequest{
                .node_id = request.node_id,
                .member_ordinal = request.member_ordinal,
                .output_ordinal = request.output_ordinal,
                .state = parse_project_sample_output_state(request.state),
            },
            project_builder);
        project_builder.build();
        notify_updated_node_ids({request.node_id});
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

void handle_set_event_output_state(
    SetEventOutputStateRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_event_output_state_requested_event,
            ProjectSetEventOutputStateRequest{
                .node_id = request.node_id,
                .member_ordinal = request.member_ordinal,
                .output_ordinal = request.output_ordinal,
                .state = parse_project_event_output_state(request.state),
            },
            project_builder);
        project_builder.build();
        notify_updated_node_ids({request.node_id});
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

void handle_server_shutdown(
    ServerShutdownRequest const &,
    SocketRpcAckResponseBuilder &)
{
    if (bound_shutdown != nullptr) {
        (*bound_shutdown)();
    }
}

IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcGraphQueryBySpansEvent,
    iv_socket_rpc_graph_query_by_spans_event,
    handle_graph_query_by_spans);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcGraphQueryActiveRegionsEvent,
    iv_socket_rpc_graph_query_active_regions_event,
    handle_graph_query_active_regions);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcGetLogicalNodeEvent,
    iv_socket_rpc_get_logical_node_event,
    handle_get_logical_node);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcGetLogicalNodesEvent,
    iv_socket_rpc_get_logical_nodes_event,
    handle_get_logical_nodes);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetSampleInputValueEvent,
    iv_socket_rpc_set_sample_input_value_event,
    handle_set_sample_input_value);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetSampleInputStateEvent,
    iv_socket_rpc_set_sample_input_state_event,
    handle_set_sample_input_state);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetEventInputStateEvent,
    iv_socket_rpc_set_event_input_state_event,
    handle_set_event_input_state);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetSampleOutputStateEvent,
    iv_socket_rpc_set_sample_output_state_event,
    handle_set_sample_output_state);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetEventOutputStateEvent,
    iv_socket_rpc_set_event_output_state_event,
    handle_set_event_output_state);
IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleInstanceBuildersCompletedEvent,
    iv_runtime_iv_module_instance_builders_completed_event,
    notify_replaced_instances);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcServerShutdownEvent,
    iv_socket_rpc_server_shutdown_event,
    handle_server_shutdown);
} // namespace

void bind_socket_rpc_iv_module_source_introspection_bridge(
    IvModuleSourceIntrospection &introspection,
    GraphInputLanes &graph_input_lanes,
    std::function<void()> *shutdown_callback)
{
    bound_introspection = &introspection;
    bound_graph_input_lanes = &graph_input_lanes;
    bound_shutdown = shutdown_callback;
}

void unbind_socket_rpc_iv_module_source_introspection_bridge(
    IvModuleSourceIntrospection const &introspection,
    GraphInputLanes const &graph_input_lanes)
{
    if (bound_introspection == &introspection) {
        bound_introspection = nullptr;
    }
    if (bound_graph_input_lanes == &graph_input_lanes) {
        bound_graph_input_lanes = nullptr;
    }
    bound_shutdown = nullptr;
}
} // namespace iv
