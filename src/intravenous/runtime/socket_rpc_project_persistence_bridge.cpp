#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
namespace {
void handle_save_project(
    SaveProjectRequest const &,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_persistence_save_requested_event,
            project_builder);
        project_builder.build();
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSaveProjectEvent,
    iv_socket_rpc_save_project_event,
    handle_save_project);
} // namespace
} // namespace iv
