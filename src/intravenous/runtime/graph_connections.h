#pragma once

#include <intravenous/graph/builder.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/project_connection_types.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace iv {
struct GraphConnectionDiagnostic {
    std::string connection_id{};
    std::string message{};
    bool operator==(GraphConnectionDiagnostic const&) const = default;
};

struct GraphConnectionsBatchResult {
    std::vector<std::string> applied_connection_ids{};
    std::vector<GraphConnectionDiagnostic> diagnostics{};
};

struct GraphConnectionUpsertMutation {
    ProjectConnection connection{};
};

struct GraphConnectionDeleteMutation {
    std::string connection_id{};
};

// Used by normalized replay once ProjectPersistence batches project state.
struct GraphConnectionReplaceAllMutation {
    std::vector<ProjectConnection> connections{};
};

using GraphConnectionsMutation = std::variant<
    std::monostate,
    GraphConnectionUpsertMutation,
    GraphConnectionDeleteMutation,
    GraphConnectionReplaceAllMutation>;

// Synchronous ProjectGraph -> GraphConnections child request. ProjectGraph
// supplies the same fresh root builder NodeInstances just populated plus the
// complete external instance-id -> placement map for that transaction.
struct GraphConnectionsProjectGraphRequest {
    GraphBuilder* root_builder = nullptr;
    std::unordered_map<std::string, NodeInstancePlacement> const* placements = nullptr;
    GraphConnectionsMutation mutation{};
    GraphConnectionsBatchResult result{};
    bool handled = false;
};

// Canonical owner of desired whole-project cross-node connection intent.
// Matcher resolution and connection application are derived work performed
// once against a complete fresh root graph in ProjectGraph's transaction.
class GraphConnections {
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ProjectConnection> desired_connections_{};

public:
    GraphConnections() = default;
    ~GraphConnections() = default;
    GraphConnections(GraphConnections const&) = delete;
    GraphConnections& operator=(GraphConnections const&) = delete;

    [[nodiscard]] std::vector<ProjectConnection> desired_connections() const;
    void handle_project_graph_transaction(GraphConnectionsProjectGraphRequest& request);
};
} // namespace iv
