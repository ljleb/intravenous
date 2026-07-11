#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <exception>

namespace iv {
namespace {
void set_autosave_enabled(
    bool enabled,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_autosave_enabled_requested_event,
            ProjectSetAutosaveEnabledRequest{.enabled = enabled},
            project_builder);
        project_builder.build();
    } catch (std::exception const& exception) {
        builder.fail(exception.what());
    }
}

IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcEnableProjectAutosaveEvent,
    iv_socket_rpc_enable_project_autosave_event,
    [](EnableProjectAutosaveRequest const&, SocketRpcAckResponseBuilder& builder) {
        set_autosave_enabled(true, builder);
    });
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcDisableProjectAutosaveEvent,
    iv_socket_rpc_disable_project_autosave_event,
    [](DisableProjectAutosaveRequest const&, SocketRpcAckResponseBuilder& builder) {
        set_autosave_enabled(false, builder);
    });
} // namespace
} // namespace iv
