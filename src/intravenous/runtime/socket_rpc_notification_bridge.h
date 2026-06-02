#pragma once

namespace iv {
    class SocketRpcServer;

    void bind_socket_rpc_notification_bridge(SocketRpcServer &server);
    void unbind_socket_rpc_notification_bridge(SocketRpcServer const &server);
} // namespace iv
