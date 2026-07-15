#include <intravenous/runtime/socket_rpc_timeline_execution_bridge.h>

#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/timeline_execution_events.h>

namespace iv {
namespace {
void handle_set_timeline_compiled_sample_cache_chunk_size(
    SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_timeline_compiled_sample_cache_chunk_size_multiplier_requested_event,
            ProjectSetTimelineCompiledSampleCacheChunkSizeMultiplierRequest{
                .compiled_sample_cache_chunk_size_multiplier =
                    request.compiled_sample_cache_chunk_size_multiplier,
            },
            project_builder);
        project_builder.build();
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

void handle_set_timeline_lane_sample_channel_type(
    SetTimelineLaneSampleChannelTypeRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_timeline_lane_sample_channel_type_requested_event,
            ProjectSetTimelineLaneSampleChannelTypeRequest{
                .lane_id = request.lane_id,
                .sample_channel_type = request.sample_channel_type,
            },
            project_builder);
        project_builder.build();
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

void handle_set_timeline_lane_ui_state(
    SetTimelineLaneUiStateRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_timeline_lane_ui_state_requested_event,
            ProjectSetTimelineLaneUiStateRequest{
                .lane_id = request.lane_id,
                .expected_revision = request.expected_revision,
                .serialized_state = request.serialized_state,
                .name = request.name,
            },
            project_builder);
        project_builder.build();
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

void handle_connect_timeline_lanes(
    ConnectTimelineLanesRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_connect_timeline_lanes_requested_event,
            ProjectConnectTimelineLanesRequest{
                .source_lane_id = request.source_lane_id,
                .target_lane_id = request.target_lane_id,
                .port_domain = request.port_domain,
                .port_kind = request.port_kind,
                .port_ordinal = request.port_ordinal,
                .authored = true,
            },
            project_builder);
        project_builder.build();
    } catch (std::exception const &error) {
        builder.fail(error.what());
    }
}

void handle_disconnect_timeline_lanes(
    DisconnectTimelineLanesRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_disconnect_timeline_lanes_requested_event,
            ProjectDisconnectTimelineLanesRequest{
                .source_lane_id = request.source_lane_id,
                .target_lane_id = request.target_lane_id,
                .port_domain = request.port_domain,
                .port_kind = request.port_kind,
                .port_ordinal = request.port_ordinal,
            },
            project_builder);
        project_builder.build();
    } catch (std::exception const &error) {
        builder.fail(error.what());
    }
}

void handle_get_timeline_lane_types(
    GetTimelineLaneTypesRequest const&, SocketRpcLaneTypesResultBuilder &builder)
{
    try {
        ProjectLaneTypesBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_get_timeline_lane_types_requested_event, project_builder);
        builder.succeed(project_builder.build());
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void handle_create_timeline_lane(
    CreateTimelineLaneRequest const& request, SocketRpcAckResponseBuilder &builder)
{
    try {
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_create_timeline_lane_requested_event,
            ProjectCreateTimelineLaneRequest{.type_id = request.type_id}, project_builder);
        project_builder.build();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void handle_pause(
    PauseRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_pause_event,
            request);
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

void handle_resume(
    ResumeRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_resume_event,
            request);
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

void handle_seek(
    SeekRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        IV_INVOKE_LINKER_EVENT(iv_runtime_seek_event, request);
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSeekEvent,
    iv_socket_rpc_seek_event,
    handle_seek);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetTimelineCompiledSampleCacheChunkSizeMultiplierEvent,
    iv_socket_rpc_set_timeline_compiled_sample_cache_chunk_size_multiplier_event,
    handle_set_timeline_compiled_sample_cache_chunk_size);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetTimelineLaneSampleChannelTypeEvent,
    iv_socket_rpc_set_timeline_lane_sample_channel_type_event,
    handle_set_timeline_lane_sample_channel_type);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetTimelineLaneUiStateEvent,
    iv_socket_rpc_set_timeline_lane_ui_state_event,
    handle_set_timeline_lane_ui_state);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcConnectTimelineLanesEvent,
    iv_socket_rpc_connect_timeline_lanes_event,
    handle_connect_timeline_lanes);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcDisconnectTimelineLanesEvent,
    iv_socket_rpc_disconnect_timeline_lanes_event,
    handle_disconnect_timeline_lanes);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcGetTimelineLaneTypesEvent,
    iv_socket_rpc_get_timeline_lane_types_event,
    handle_get_timeline_lane_types);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcCreateTimelineLaneEvent,
    iv_socket_rpc_create_timeline_lane_event,
    handle_create_timeline_lane);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcPauseEvent,
    iv_socket_rpc_pause_event,
    handle_pause);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcResumeEvent,
    iv_socket_rpc_resume_event,
    handle_resume);
} // namespace

void bind_socket_rpc_timeline_execution_bridge(
    Timeline &timeline,
    TimelineExecution &timeline_execution,
    std::filesystem::path workspace_root)
{
    (void)timeline;
    (void)timeline_execution;
    (void)workspace_root;
}

void unbind_socket_rpc_timeline_execution_bridge(
    TimelineExecution const &timeline_execution)
{
    (void)timeline_execution;
}
} // namespace iv
