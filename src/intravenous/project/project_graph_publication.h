#pragma once

#include <intravenous/project/runtime_project_api_types.h>

#include <cstdint>
#include <optional>

namespace iv {
struct ExecutionPreparationProgress {
    std::uint64_t attempt = 0;
    bool pending = false;
};

// What one ProjectGraph root build changed in the read models that clients
// present. No module notifies SocketRpcServer partway through a cause: this
// record returns to the cause's root on the request records the cause already
// uses, and the root publishes it once (docs-dev/event_flows/README.md).
struct ProjectGraphPublication {
    bool instances_changed = false;
    // Source-introspection symbolic-node replacements, when there are any.
    std::optional<ProjectSymbolicNodesNotification> symbolic_nodes{};
    // The build changed what a graph editor scope shows: desired instances or
    // connections, or the generation their ports come from. Editors re-read
    // their scopes (graph.getScope).
    bool graph_changed = false;
    std::optional<ExecutionPreparationProgress> execution_preparation{};
    // Configuration can produce valid interfaces while GraphJit fails. Carry
    // that failure back to the cause's publication instead of hiding it in
    // ProjectGraphGeneration or reporting only a successful package build.
    std::optional<ProjectStatusNotification> execution_failure{};

    [[nodiscard]] bool empty() const noexcept
    {
        return !instances_changed && !symbolic_nodes.has_value() && !graph_changed
            && !execution_failure.has_value() && !execution_preparation.has_value();
    }
};
} // namespace iv
