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
