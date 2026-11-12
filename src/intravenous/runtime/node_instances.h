#pragma once

#include <intravenous/graph/builder.h>
#include <intravenous/linker_event.h>
#include <intravenous/module/configuration_argument.h>
#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/node_definition_types.h>
#include <intravenous/runtime/project_node_instance_types.h>
#include <intravenous/runtime/project_node_replacement_types.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace iv {
class ProjectPersistenceBuilder;
class SocketRpcNodeInstancesResultBuilder;
struct GetNodeInstancesRequest;

struct IvModuleRequiredDefinition {
    std::string definition_id{};
    std::filesystem::path package_root{};
};

struct IvModuleRequiredDefinitionsChanged {
    std::vector<IvModuleRequiredDefinition> created{};
    std::vector<IvModuleRequiredDefinition> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

// Compatibility read model retained for the current source-introspection
// projection. NodeInstances itself and its project mutation surface are
// definition-kind agnostic.
struct IvModuleInstance {
    std::string instance_id{};
    std::string definition_id{};
    std::string display_name{};
    std::filesystem::path package_root{};
    std::string configuration_source{};
    std::string module_id{};

    bool operator==(IvModuleInstance const&) const = default;
};

struct NodeInstanceConfigurationRequest {
    std::string instance_id{};
    std::string definition_id{};
    std::span<details::ConfigurationArgument> arguments{};
    std::optional<std::string> configuration_source{};
    std::optional<std::string> configuration_arguments_json{};
    std::optional<ChannelLayout> tiled_layout{};
};

// The record of one configured invocation: its definition provenance, the
// lifetimes its live graph state depends on, and its message handlers. A
// reusable invocation additionally carries a closed copy of its contents
// (`graph`, rooted at `local_root`) that later identical calls import.
struct ConfiguredNodeInstance {
    std::string definition_id{};
    std::uint64_t definitions_generation = 0;
    std::uint64_t definition_version = 0;
    NodeBundleHandle local_root = 0;

    // Ownership roots are declared before the objects that depend on them so
    // reverse member destruction keeps provider code and typed values alive
    // while the configured graph and nested configured results are destroyed.
    // A later optimization pass may narrow this conservative snapshot pin to
    // only revisions actually used.
    std::shared_ptr<NodeDefinitionsSnapshot const> definitions{};
    // Type-erased owner of the exact typed argument tuple invoked to produce
    // this graph. The graph may contain configuration objects that retain
    // references/pointers into those values. Its provider-generated destructor
    // callbacks are valid while `definitions` remains pinned.
    std::shared_ptr<void const> configuration_lifetime{};
    // Nested configured results that were embedded while this graph was
    // evaluated. This is required even when a nested invocation is deliberately
    // not reusable because its value type has no safe equality/hash operation.
    std::vector<std::shared_ptr<ConfiguredNodeInstance const>> dependencies{};
    // Paths are relative to the invocation root, so a cached configured
    // result can be placed at any durable project NodeAddress without
    // capturing its first placement.
    std::vector<ConfiguredNodeMessageHandler> message_handlers{};
    // Null when the invocation was not reusable: uncacheable arguments, or a
    // construction that refers to graph state outside itself.
    std::shared_ptr<ConfiguredGraph const> graph{};
};

struct NodeInstancePlacement {
    std::shared_ptr<ConfiguredNodeInstance const> configured{};
    // The invocation's result in the root graph.
    NodeBundleHandle root = 0;
};

struct NodeInstanceDiagnostic {
    std::string instance_id{};
    std::string definition_id{};
    // A construction failure's message is followed by one line per library
    // boundary it crossed, innermost first.
    std::string message{};
    // Where a construction failure was first recorded; empty for problems
    // found before construction.
    std::string stacktrace{};
};

// Any diagnostic means the batch failed: it then holds every problem found
// before construction, or the first construction failure, and no placements.
struct NodeInstancesBatchResult {
    std::unordered_map<std::string, NodeInstancePlacement> placements{};
    std::vector<NodeInstanceDiagnostic> diagnostics{};
};

struct NodeInstanceCreateMutation {
    std::optional<std::string> instance_id{};
    std::string definition_id{};
    std::optional<std::filesystem::path> package_root{};
    std::optional<std::string> display_name{};
    std::string configuration_source{};
};

struct NodeInstanceDeleteMutation {
    std::string instance_id{};
};

struct NodeInstanceDisplayNameUpdate {
    std::string instance_id{};
    std::optional<std::string> display_name{};
    std::optional<std::string> configuration_source{};
};

struct NodeInstanceUpdateMutation {
    std::vector<NodeInstanceDisplayNameUpdate> updates{};
};

struct NodeInstanceReplaceAllMutation {
    std::vector<ProjectNodeInstanceDeclaration> instances{};
    std::vector<ProjectNodeReplacement> node_replacements{};
};

// Transaction-local semantic realization. This never changes the canonical
// desired replacement set owned by NodeInstances.
struct NodeReplacementPreviewMutation {
    std::vector<ProjectNodeReplacement> replacements{};
};

struct NodeReplacementsReplaceMutation {
    std::vector<ProjectNodeReplacement> replacements{};
};

using NodeInstancesMutation = std::variant<
    std::monostate,
    NodeInstanceCreateMutation,
    NodeInstanceDeleteMutation,
    NodeInstanceUpdateMutation,
    NodeInstanceReplaceAllMutation,
    NodeReplacementPreviewMutation,
    NodeReplacementsReplaceMutation>;

// Synchronous ProjectGraph -> NodeInstances transaction request. ProjectGraph
// supplies one pinned definition world and one fresh root builder. NodeInstances
// applies at most one logical mutation, realizes its complete desired set once,
// and returns placements/diagnostics through this same control-flow edge.
struct NodeInstancesProjectGraphRequest {
    std::shared_ptr<NodeDefinitionsSnapshot const> snapshot{};
    GraphBuilder* root_builder = nullptr;
    NodeInstancesMutation mutation{};
    NodeInstancesBatchResult result{};
    std::vector<std::string> created_instance_ids{};
    std::vector<ProjectNodeReplacement> realized_node_replacements{};
    // What this transaction changed in the instance read models. NodeInstances
    // publishes nothing itself; ProjectGraph returns these to its cause's root.
    // `instances` is the published list after the transaction, filled when
    // either flag is set. Declarations are the project-owned instances
    // (source-introspection input); the published list also changes when a
    // definition's realization does.
    bool instances_list_changed = false;
    bool instance_declarations_changed = false;
    std::vector<IvModuleInstanceInfo> instances{};
    bool handled = false;
};

class NodeInstances;

// The NodeInstances transaction of one node-message interaction. ProjectGraph
// enters NodeInstances once, through
// iv_runtime_project_graph_node_instances_interaction_requested_event, to open
// it. Each later run takes one NodeInstancesProjectGraphRequest carrying a
// NodeReplacementPreviewMutation, or the interaction's final
// NodeReplacementsReplaceMutation, and handles it exactly as that request's
// event would, but as part of the opening entry instead of entering
// NodeInstances again. A preview changes no desired state and raises nothing;
// the final replacement set is committed once, and is the only run that can
// raise NodeInstances' own events.
class NodeInstancesInteraction {
    NodeInstances* owner_ = nullptr;
    EventModuleVisit visit_{};
    bool committed_ = false;

public:
    NodeInstancesInteraction() = default;
    NodeInstancesInteraction(NodeInstances& owner, EventModuleVisit visit);
    NodeInstancesInteraction(NodeInstancesInteraction&& other) noexcept;
    NodeInstancesInteraction& operator=(NodeInstancesInteraction&& other) noexcept;
    NodeInstancesInteraction(NodeInstancesInteraction const&) = delete;
    NodeInstancesInteraction& operator=(NodeInstancesInteraction const&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }

    void run(NodeInstancesProjectGraphRequest& request);
};

struct NodeInstancesInteractionRequest {
    NodeInstancesInteraction interaction{};
    bool handled = false;
};

// Canonical desired-instance owner plus reusable typed configured-graph cache.
// ProjectGraph owns the write-side transaction surface and invokes this module
// synchronously with one pinned definition snapshot and one fresh root builder.
// The remaining IV-module handlers are read/persistence projection surfaces.
class NodeInstances {
public:
    struct Update {
        std::string instance_id{};
        std::optional<std::string> display_name{};
        std::optional<std::string> configuration_source{};
    };

private:
    struct DesiredInstance {
        std::string instance_id{};
        std::string definition_id{};
        std::string display_name{};
        std::filesystem::path package_root{};
        std::string configuration_source{};
    };
    struct CacheEntry;
    struct ConfigurationContext;
    struct ExpressionCacheEntry;
    // Read-model changes accumulated over one transaction.
    struct Changes {
        bool instances_list_changed = false;
        bool instance_declarations_changed = false;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, DesiredInstance> desired_instances_by_id_;
    std::vector<ProjectNodeReplacement> desired_node_replacements_{};
    std::unordered_map<std::string, IvModuleRequiredDefinition> required_definitions_by_id_;
    std::unordered_map<std::string, IvModuleInstance> published_instances_by_id_;
    std::shared_ptr<NodeDefinitionsSnapshot const> definitions_snapshot_{};
    std::vector<std::shared_ptr<CacheEntry>> configuration_cache_{};
    std::vector<std::shared_ptr<ExpressionCacheEntry const>> expression_cache_{};
    std::uint64_t minimum_cache_generation_ = 0;

    // Updates the published record of one realized instance; true when it
    // changed.
    bool publish_instance_locked(
        std::string const& instance_id,
        NodeDefinitionEntry const& definition);
    IvModuleRequiredDefinitionsChanged rebuild_required_definitions_locked();
    std::string create_instance(
        Changes& changes,
        std::string_view definition_id,
        std::filesystem::path package_root,
        std::optional<std::string> instance_id,
        std::optional<std::string> display_name,
        std::string configuration_source);
    void remove_instance(Changes& changes, std::string const& instance_id);
    void update_instances(Changes& changes, std::vector<Update> updates);
    void install_definitions_snapshot(
        Changes& changes,
        std::shared_ptr<NodeDefinitionsSnapshot const> const& snapshot);
    struct RealizedInvocation {
        NodeBundleHandle root = 0;
        std::shared_ptr<ConfiguredNodeInstance const> configured{};
    };
    // Realizes one registered call in `builder`'s session: a cache hit
    // imports the cached contents, a miss constructs in place.
    RealizedInvocation configure_cached(
        ConfigurationContext& context,
        GraphBuilder& builder,
        NodeAddress const& address,
        std::string_view qualifier,
        std::string_view definition_id,
        std::optional<ChannelLayout> tiled_layout,
        std::span<details::ConfigurationArgument> arguments);
    std::shared_ptr<ExpressionCacheEntry const> parse_expression_cached(
        std::string_view definition_id,
        std::uint64_t definition_version,
        std::string source);
    static NodeRef resolve_builder_definition(
        void* context,
        GraphBuilder& builder,
        std::string_view definition_id,
        std::span<ProjectNodePathSelector const> relative_path,
        std::optional<ChannelLayout> tiled_layout,
        std::span<details::ConfigurationArgument> arguments);
    static std::optional<NodeBundleHandle> resolve_builder_concrete_node_replacement(
        void* context,
        GraphBuilder& builder,
        std::span<ProjectNodePathSelector const> relative_path);

    friend class NodeInstancesInteraction;
    // The body of one ProjectGraph transaction, shared by the transaction
    // event and NodeInstancesInteraction::run.
    void apply_project_graph_transaction(NodeInstancesProjectGraphRequest& request);

public:
    NodeInstances();
    ~NodeInstances();
    NodeInstances(NodeInstances const&) = delete;
    NodeInstances& operator=(NodeInstances const&) = delete;

    [[nodiscard]] std::vector<IvModuleInstanceInfo> list_instances() const;
    [[nodiscard]] std::vector<ProjectNodeInstanceDeclaration>
    desired_instances() const;
    [[nodiscard]] std::vector<ProjectNodeReplacement>
    desired_node_replacements() const;

    // Synchronous configuration primitive intended for ProjectGraph. Every
    // request is constructed directly in `root_builder`'s session and resolves
    // recursively through exactly `snapshot`. Any failure fails the whole
    // batch: problems found before construction are reported for every
    // request; otherwise construction stops at the first failure, which leaves
    // the session failed. A failed batch must not be finished or used.
    // Cache reuse is currently conservative across snapshot generations; cache
    // preservation across unrelated definition reloads is intentionally deferred.
    NodeInstancesBatchResult configure_and_embed(
        std::shared_ptr<NodeDefinitionsSnapshot const> snapshot,
        GraphBuilder& root_builder,
        std::span<NodeInstanceConfigurationRequest const> requests,
        std::vector<ProjectNodeReplacement> const* replacement_override = nullptr);
    [[nodiscard]] std::size_t configuration_cache_size() const;

    void handle_project_graph_transaction(NodeInstancesProjectGraphRequest& request);
    void handle_project_graph_interaction(NodeInstancesInteractionRequest& request);
    void handle_project_persistence_collect_state(ProjectPersistenceBuilder& builder) const;
    void handle_socket_rpc_get_node_instances(
        GetNodeInstancesRequest const& request,
        SocketRpcNodeInstancesResultBuilder& builder) const;
};
} // namespace iv
