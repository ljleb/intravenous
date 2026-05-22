#pragma once

namespace iv {
    class RuntimeProjectService;

    void bind_socket_rpc_runtime_project_bridge(RuntimeProjectService &service);
    void unbind_socket_rpc_runtime_project_bridge(RuntimeProjectService const &service);
} // namespace iv
