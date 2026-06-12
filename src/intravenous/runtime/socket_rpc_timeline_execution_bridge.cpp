#include <intravenous/runtime/socket_rpc_timeline_execution_bridge.h>

#include <intravenous/runtime/config.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/timeline_execution.h>

namespace iv {
namespace {
TimelineExecution *bound_timeline_execution = nullptr;
std::filesystem::path bound_workspace_root {};

void handle_set_timeline_compiled_sample_cache_chunk_size(
    SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    if (bound_timeline_execution == nullptr) {
        return;
    }
    try {
        bound_timeline_execution->set_compiled_sample_cache_chunk_size_multiplier(
            request.compiled_sample_cache_chunk_size_multiplier);
        set_runtime_project_compiled_sample_cache_chunk_size_multiplier(
            bound_workspace_root,
            request.compiled_sample_cache_chunk_size_multiplier);
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetTimelineCompiledSampleCacheChunkSizeMultiplierEvent,
    iv_socket_rpc_set_timeline_compiled_sample_cache_chunk_size_multiplier_event,
    handle_set_timeline_compiled_sample_cache_chunk_size);
} // namespace

void bind_socket_rpc_timeline_execution_bridge(
    TimelineExecution &timeline_execution,
    std::filesystem::path workspace_root)
{
    bound_timeline_execution = &timeline_execution;
    bound_workspace_root = std::move(workspace_root);
}

void unbind_socket_rpc_timeline_execution_bridge(
    TimelineExecution const &timeline_execution)
{
    if (bound_timeline_execution == &timeline_execution) {
        bound_timeline_execution = nullptr;
        bound_workspace_root.clear();
    }
}
} // namespace iv
