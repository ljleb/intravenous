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

class GraphConnections;

// The GraphConnections transaction of one node-message interaction. ProjectGraph
// enters GraphConnections once, through
// iv_runtime_project_graph_connections_interaction_requested_event, to open
// it. Each later run applies the unchanged desired connection set to one
// preview or final root builder, exactly as a GraphConnectionsProjectGraphRequest
// without a mutation would, but as part of the opening entry instead of
// entering GraphConnections again. An interaction never changes desired
// connections.
class GraphConnectionsInteraction {
    GraphConnections* owner_ = nullptr;
    EventModuleVisit visit_{};

public:
    GraphConnectionsInteraction() = default;
    GraphConnectionsInteraction(GraphConnections& owner, EventModuleVisit visit)
        : owner_(&owner)
        , visit_(visit)
    {}

    [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }

    void run(GraphConnectionsProjectGraphRequest& request) const;
};

struct GraphConnectionsInteractionRequest {
    GraphConnectionsInteraction interaction{};
    bool handled = false;
};

// Canonical owner of desired whole-project cross-node connection intent.
// Matcher resolution and connection application are derived work performed
// once against a complete fresh root graph in ProjectGraph's transaction.
class GraphConnections {
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ProjectConnection> desired_connections_{};

    friend class GraphConnectionsInteraction;
    // The body of one ProjectGraph transaction, shared by the transaction
    // event and GraphConnectionsInteraction::run.
    void apply_project_graph_transaction(GraphConnectionsProjectGraphRequest& request);

public:
    GraphConnections() = default;
    ~GraphConnections() = default;
    GraphConnections(GraphConnections const&) = delete;
    GraphConnections& operator=(GraphConnections const&) = delete;

    [[nodiscard]] std::vector<ProjectConnection> desired_connections() const;
    void handle_project_graph_transaction(GraphConnectionsProjectGraphRequest& request);
    void handle_project_graph_interaction(GraphConnectionsInteractionRequest& request);
    void handle_project_persistence_collect_state(
        ProjectPersistenceBuilder& builder) const;
};
} // namespace iv
