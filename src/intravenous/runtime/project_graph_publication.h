#pragma once

#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/runtime_project_api_types.h>

#include <optional>
#include <vector>

namespace iv {
// What one ProjectGraph root build changed in the read models that clients
// present. No module notifies SocketRpcServer partway through a cause: this
// record returns to the cause's root on the request records the cause already
// uses, and the root publishes it once (docs/event_flows/README.md).
struct ProjectGraphPublication {
    // The published node-instance list, when it changed.
    std::optional<std::vector<IvModuleInstanceInfo>> instances{};
    // Source-introspection virtual-node replacements, when there are any.
    std::optional<ProjectVirtualNodesNotification> virtual_nodes{};
    // The build changed what a graph editor scope shows: desired instances or
    // connections, or the generation their ports come from. Editors re-read
    // their scopes (graph.getScope).
    bool graph_changed = false;
    // Configuration can produce valid interfaces while GraphJit fails. Carry
    // that failure back to the cause's publication instead of hiding it in
    // ProjectGraphGeneration or reporting only a successful package build.
    std::optional<ProjectStatusNotification> execution_failure{};

    [[nodiscard]] bool empty() const noexcept
    {
        return !instances.has_value() && !virtual_nodes.has_value() && !graph_changed
            && !execution_failure.has_value();
    }
};
} // namespace iv
