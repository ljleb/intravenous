#pragma once

#include <intravenous/runtime/socket_rpc_requests.h>

#include <functional>
#include <memory>
#include <string>

namespace iv {
struct SocketRpcRouteResult {
    std::string response{};
    bool mutates_session = false;
    bool shutdown_after_response = false;
};

struct SocketRpcRouteContext {
    std::function<void(std::string const&)> authorize{};
    std::function<void(bool)> before_invoke{};
    std::function<std::string(int, SessionHelloRequest const&)> hello{};
    std::function<std::string(int, SessionSubscribeRequest const&)> subscribe{};
};

class SocketRpcRouteRegistry {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    SocketRpcRouteRegistry();
    ~SocketRpcRouteRegistry();
    SocketRpcRouteRegistry(SocketRpcRouteRegistry const&) = delete;
    SocketRpcRouteRegistry& operator=(SocketRpcRouteRegistry const&) = delete;

    [[nodiscard]] SocketRpcRouteResult dispatch(
        ParsedSocketRpcRequest const& request,
        SocketRpcRouteContext const& context) const;
};
} // namespace iv
