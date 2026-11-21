#include <intravenous/app/bridges/socket_rpc_package_watcher_bridge.h>

#include <intravenous/packages/package_pipeline_events.h>
#include <intravenous/packages/package_watcher.h>
#include <intravenous/rpc/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_package_watcher_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_package_watcher_bridge,
    iv_runtime_package_refresh_published_event,
    &SocketRpcServer::handle_package_refresh_published)
} // namespace iv
