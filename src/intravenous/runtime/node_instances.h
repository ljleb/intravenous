#pragma once

#include <intravenous/graph/builder.h>
#include <intravenous/module/configuration_argument.h>
#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/node_definition_types.h>

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
class SocketRpcIvModuleInstancesResultBuilder;
struct NodeDefinitionsSnapshotChanged;
struct GetIvModuleInstancesRequest;

struct IvModuleRequiredDefinition {
    std::string definition_id{};
    std::filesystem::path package_root{};
};

struct IvModuleRequiredDefinitionsChanged {
    std::vector<IvModuleRequiredDefinition> created{};
    std::vector<IvModuleRequiredDefinition> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

// Compatibility read model retained for the current IV-module RPC/persistence
// surface. NodeInstances itself is definition-kind agnostic.
struct IvModuleInstance {
    std::string instance_id{};
    std::string definition_id{};
    std::string display_name{};
    std::filesystem::path package_root{};
    std::string module_id{};

    bool operator==(IvModuleInstance const&) const = default;
};

struct IvModuleInstancesChanged {
    std::vector<IvModuleInstance> created{};
    std::vector<IvModuleInstance> updated{};
    std::vector<std::string> deleted_instance_ids{};
};

struct NodeInstanceConfigurationRequest {
    std::string instance_id{};
    std::string definition_id{};
    std::span<details::ConfigurationArgument> arguments{};
    std::optional<ChannelLayout> tiled_layout{};
};

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
    std::shared_ptr<ConfiguredGraph const> graph{};
};

struct NodeInstancePlacement {
    std::shared_ptr<ConfiguredNodeInstance const> configured{};
    ConfiguredGraphEmbedding embedding{};
    NodeBundleHandle root = 0;
};

struct NodeInstanceDiagnostic {
    std::string instance_id{};
    std::string definition_id{};
    std::string message{};
};

struct NodeInstancesBatchResult {
    std::unordered_map<std::string, NodeInstancePlacement> placements{};
    std::vector<NodeInstanceDiagnostic> diagnostics{};
};

struct NodeInstanceCreateMutation {
    std::optional<std::string> instance_id{};
    std::string definition_id{};
    std::optional<std::filesystem::path> package_root{};
    std::optional<std::string> display_name{};
};

struct NodeInstanceDeleteMutation {
    std::string instance_id{};
};

struct NodeInstanceDisplayNameUpdate {
    std::string instance_id{};
    std::optional<std::string> display_name{};
};

struct NodeInstanceUpdateMutation {
    std::vector<NodeInstanceDisplayNameUpdate> updates{};
};

using NodeInstancesMutation = std::variant<
    std::monostate,
    NodeInstanceCreateMutation,
    NodeInstanceDeleteMutation,
    NodeInstanceUpdateMutation>;

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
    };

private:
    struct DesiredInstance {
        std::string instance_id{};
        std::string definition_id{};
        std::string display_name{};
        std::filesystem::path package_root{};
    };
    struct CacheEntry;
    struct ConfigurationContext;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, DesiredInstance> desired_instances_by_id_;
    std::unordered_map<std::string, IvModuleRequiredDefinition> required_definitions_by_id_;
    std::unordered_map<std::string, IvModuleInstance> published_instances_by_id_;
    std::shared_ptr<NodeDefinitionsSnapshot const> definitions_snapshot_{};
    std::vector<std::shared_ptr<CacheEntry>> configuration_cache_{};
    std::uint64_t minimum_cache_generation_ = 0;

    bool publish_instance_locked(
        std::string const& instance_id,
        NodeDefinitionEntry const& definition,
        IvModuleInstancesChanged& diff);
    void publish_instance_changes(IvModuleInstancesChanged diff, bool list_changed) const;
    void publish_instance_declarations_changed() const;
    std::shared_ptr<CacheEntry> configure_cached(
        ConfigurationContext& context,
        std::string_view definition_id,
        std::optional<ChannelLayout> tiled_layout,
        std::span<details::ConfigurationArgument> arguments);
    static NodeRef resolve_builder_definition(
        void* context,
        GraphBuilder& builder,
        std::string_view definition_id,
        std::optional<ChannelLayout> tiled_layout,
        std::span<details::ConfigurationArgument> arguments);

public:
    NodeInstances();
    ~NodeInstances();
    NodeInstances(NodeInstances const&) = delete;
    NodeInstances& operator=(NodeInstances const&) = delete;

    std::string create_instance(
        std::string_view definition_id,
        std::filesystem::path package_root,
        std::optional<std::string> instance_id = std::nullopt,
        std::optional<std::string> display_name = std::nullopt);
    void remove_instance(std::string const& instance_id);
    void update_instances(std::vector<Update> updates);
    [[nodiscard]] std::vector<IvModuleInstanceInfo> list_instances() const;

    // Synchronous configuration/embedding primitive intended for ProjectGraph.
    // Every request in the batch resolves recursively through exactly `snapshot`.
    // Cache reuse is currently conservative across snapshot generations; cache
    // preservation across unrelated definition reloads is intentionally deferred.
    NodeInstancesBatchResult configure_and_embed(
        std::shared_ptr<NodeDefinitionsSnapshot const> snapshot,
        GraphBuilder& root_builder,
        std::span<NodeInstanceConfigurationRequest const> requests);
    [[nodiscard]] std::size_t configuration_cache_size() const;

    void handle_node_definitions_snapshot_changed(
        NodeDefinitionsSnapshotChanged const& change);
    void handle_project_graph_transaction(NodeInstancesProjectGraphRequest& request);
    void handle_project_persistence_collect_state(ProjectPersistenceBuilder& builder) const;
    void handle_socket_rpc_get_iv_module_instances(
        GetIvModuleInstancesRequest const& request,
        SocketRpcIvModuleInstancesResultBuilder& builder) const;
};
} // namespace iv
