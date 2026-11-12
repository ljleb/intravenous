#pragma once

#include <intravenous/runtime/project_graph_publication.h>
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
    // Server-assigned identity of this attached client. Domain routes use
    // this value instead of trusting a client-supplied identity parameter.
    std::string client_id{};
    std::function<void(std::string const&)> authorize{};
    std::function<void(bool)> before_invoke{};
    std::function<std::string(int, SessionHelloRequest const&)> hello{};
    std::function<std::string(int, SessionSubscribeRequest const&)> subscribe{};
    // Publishes the read-model changes a project-graph request committed,
    // after its response. They return on the request's result builder.
    std::function<void(ProjectGraphPublication const&)> publish_project_graph{};
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
