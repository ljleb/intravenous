#pragma once

#include <intravenous/basic_nodes/weak_type_erased.h>
#include <intravenous/graph/build_types.h>
#include <intravenous/module/dependency.h>
#include <intravenous/node/compiler_record.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iv {
using ModuleRef = std::shared_ptr<void>;
struct ConfiguredGraph;

struct IvPackageDeclaration {
    // Stable identity of one independently compiled IV package.
    std::string package_id{};
    std::filesystem::path package_root{};
};

struct IvPackageDeclarationsChanged {
    std::vector<IvPackageDeclaration> created{};
    std::vector<IvPackageDeclaration> updated{};
    std::vector<std::string> deleted_package_ids{};
};

struct IvModuleDefinition {
    // Published definitions are keyed by stable IV module ID. Package ownership
    // is retained separately and never inferred from the module ID or source path.
    std::string definition_id{};
    std::string package_id{};
    std::filesystem::path package_root{};
    std::string module_id{};
    GraphIntrospectionMetadata introspection{};
    std::vector<ModuleDependency> dependencies{};
    std::vector<ModuleRef> module_refs{};
    WeakTypeErasedNode root{};
    std::shared_ptr<ConfiguredGraph const> configured_graph{};
};

struct IvModuleDefinitionsChanged {
    std::vector<IvModuleDefinition> created{};
    std::vector<IvModuleDefinition> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

// Primitive node types are first-class registry definitions.  The stable ID
// is the server identity; NodeCodeKey and the compiler callbacks are specific
// to this loaded artifact and stay valid through module_refs.
struct IvNodeTypeDefinition {
    std::string node_type_id{};
    std::string package_id{};
    std::filesystem::path package_root{};
    details::NodeCompilerRecord compiler_record{};
    std::vector<ModuleRef> module_refs{};
    std::shared_ptr<ConfiguredGraph const> configured_graph{};
};

struct IvNodeTypeDefinitionsChanged {
    std::vector<IvNodeTypeDefinition> created{};
    std::vector<IvNodeTypeDefinition> updated{};
    std::vector<std::string> deleted_node_type_ids{};
};

struct IvModuleDefinitionsMessage {
    std::string level = "info";
    std::string message{};
    std::filesystem::path package_root{};
};

using IvModuleDefinitionsNotification = IvModuleDefinitionsMessage;

// A successful package build contributes this complete candidate definition set.
// It remains stored while another package temporarily conflicts with one of its IDs.
struct IvModuleReloadedDefinition {
    std::string package_id{};
    std::string definition_id{};
    std::filesystem::path package_root{};
    std::string module_id{};
    GraphIntrospectionMetadata introspection{};
    std::vector<ModuleDependency> dependencies{};
    std::vector<ModuleRef> module_refs{};
    WeakTypeErasedNode root{};
    std::shared_ptr<ConfiguredGraph const> configured_graph{};
};

struct IvModuleReloadedNodeType {
    std::string package_id{};
    std::string node_type_id{};
    std::filesystem::path package_root{};
    details::NodeCompilerRecord compiler_record{};
    std::vector<ModuleRef> module_refs{};
    std::shared_ptr<ConfiguredGraph const> configured_graph{};
};

struct IvModuleRequiredDefinitionsChanged;
struct IvModuleReloadResults;
class IvModuleDefinitionLookupBuilder;

class IvModuleDefinitions {
public:
    struct DefinitionState {
        std::vector<ModuleRef> module_refs{};
        IvModuleDefinition snapshot{};
    };
    struct NodeTypeState {
        std::vector<ModuleRef> module_refs{};
        IvNodeTypeDefinition snapshot{};
    };

private:
    struct PackageCandidate {
        std::vector<IvModuleReloadedDefinition> modules{};
        std::vector<IvModuleReloadedNodeType> node_types{};
    };

    mutable std::mutex mutex;
    std::unordered_map<std::string, IvPackageDeclaration> declarations_by_package_id;
    std::unordered_map<std::string, std::unique_ptr<DefinitionState>> loaded_definitions_by_module_id;
    std::unordered_map<std::string, std::unique_ptr<NodeTypeState>> loaded_node_types_by_id;
    // One shared ownership map enforces the single stable-ID namespace across
    // primitive node types and iv modules.
    std::unordered_map<std::string, std::string> package_id_by_definition_id;
    std::unordered_map<std::string, std::vector<std::string>> module_ids_by_package_id;
    // Candidate sets are independent from publication. Moving a definition between
    // packages can temporarily create a duplicate ID without discarding either
    // package candidate; the remaining candidate becomes live when the conflict ends.
    std::unordered_map<std::string, PackageCandidate> candidates_by_package_id;

    void emit_notification(IvModuleDefinitionsNotification notification) const;
    void emit_message(std::string level, std::string message, std::filesystem::path package_root = {}) const;
    void rebuild_published_registry_locked(
        IvModuleDefinitionsChanged& diff,
        IvNodeTypeDefinitionsChanged& node_type_diff,
        std::vector<IvModuleDefinitionsMessage>& failures,
        std::unordered_set<std::string> const& changed_package_ids);
public:
    IvModuleDefinitions() = default;
    ~IvModuleDefinitions();

    std::string declare_package(
        std::string package_id,
        std::filesystem::path package_root);
    // Replace the manifest-discovered IV package set as one snapshot. Definition
    // IDs are supplied only by a successful package compile/finalize result.
    void sync_package_declarations(
        std::vector<std::pair<std::string, std::filesystem::path>> declarations);
    void remove_package(std::string const &package_id);

    void handle_required_definitions_changed(
        IvModuleRequiredDefinitionsChanged const &diff);
    void handle_iv_module_definition_lookup(
        std::string const& definition_id,
        IvModuleDefinitionLookupBuilder& builder) const;
    void handle_reload_results(IvModuleReloadResults const &results);

    void seed_loaded_definition(IvModuleReloadedDefinition loaded_definition);

    [[nodiscard]] std::vector<IvModuleDefinition> loaded_definitions() const;
    [[nodiscard]] std::vector<IvNodeTypeDefinition> loaded_node_types() const;
    [[nodiscard]] std::optional<std::filesystem::path> package_root_for_module(
        std::string const& module_id) const;
    [[nodiscard]] std::vector<std::string> module_ids_for_package(
        std::string const& package_id) const;
    [[nodiscard]] std::vector<std::string> node_type_ids_for_package(
        std::string const& package_id) const;
};
} // namespace iv
