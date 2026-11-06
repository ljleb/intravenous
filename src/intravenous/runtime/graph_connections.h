#pragma once

#include <intravenous/graph/builder.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/project_connection_types.h>

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace iv {
class ProjectPersistenceBuilder;
struct GraphConnectionDiagnostic {
    std::string connection_id{};
    std::string message{};
    bool operator==(GraphConnectionDiagnostic const&) const = default;
};

// `diagnostics` describe retained intent that is not applied: a connection
// whose matchers resolve to no ports or to incompatible ports. They do not fail
// the configuration pass. `failure` is set when applying a resolved, compatible
// connection failed; the pass fails and the root builder must not be used.
struct GraphConnectionsBatchResult {
    std::vector<std::string> applied_connection_ids{};
    std::vector<GraphConnectionDiagnostic> diagnostics{};
    std::optional<GraphConnectionDiagnostic> failure{};
};

void validate_project_connection(ProjectConnection const& connection);

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
// Without a root builder, the request only commits `mutation` to the desired
// connection set: the configuration pass failed before connections could be
// realized, but the desired revision still includes this mutation.
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
    void handle_project_persistence_collect_state(
        ProjectPersistenceBuilder& builder) const;
};
} // namespace iv
