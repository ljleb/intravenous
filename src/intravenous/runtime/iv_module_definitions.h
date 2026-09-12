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

struct IvPackageDefinitionsChanged {
    IvModuleDefinitionsChanged modules{};
    IvNodeTypeDefinitionsChanged node_types{};
};

// One coherent read of the package registry. Published IDs are the only IDs
// that can be instantiated or resolved by a graph. A non-empty publication
// message means the package produced a candidate that deliberately remains
// unavailable (for example, an invalid local registration or an ID conflict).
struct IvPackageDefinitionSnapshot {
    IvPackageDeclaration declaration{};
    std::vector<std::string> published_module_ids{};
    std::vector<std::string> published_node_type_ids{};
    std::string publication_message{};
};

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
    // Discovery and persisted/project requests have different lifetimes.
    // A fresh filesystem scan must never erase a declaration retained for an
    // existing instance merely because that package lies outside the current
    // search roots.
    std::unordered_map<std::string, IvPackageDeclaration>
        retained_package_declarations_by_id;
    std::unordered_map<std::string, IvPackageDeclaration>
        discovered_package_declarations_by_id;
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
    // A finalizer can return a malformed candidate even when the package build
    // itself succeeded. Keep that diagnostic in the registry so the package
    // catalog does not mislabel it as an empty package.
    std::unordered_map<std::string, std::string>
        candidate_validation_messages_by_package_id;

    [[nodiscard]] std::unordered_map<std::string, IvPackageDeclaration>
    merge_declaration_sources_locked(
        std::unordered_map<std::string, IvPackageDeclaration> const& retained,
        std::unordered_map<std::string, IvPackageDeclaration> const& discovered) const;
    void declare_packages(std::vector<IvPackageDeclaration> declarations);
    void rebuild_published_registry_locked(
        IvModuleDefinitionsChanged& diff,
        IvNodeTypeDefinitionsChanged& node_type_diff,
        std::unordered_set<std::string> const& changed_package_ids);
    void publish_package_definitions_changed(
        IvModuleDefinitionsChanged modules,
        IvNodeTypeDefinitionsChanged node_types) const;
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
    void handle_reload_results(IvModuleReloadResults const &results);

    void seed_loaded_definition(IvModuleReloadedDefinition loaded_definition);

    // The declaration registry is the authoritative package catalog. UI and
    // project services must query this coherent snapshot rather than scanning
    // the filesystem or combining independently-read change caches.
    [[nodiscard]] std::vector<IvPackageDefinitionSnapshot>
    package_definition_snapshots() const;
    [[nodiscard]] std::vector<IvModuleDefinition> loaded_definitions() const;
    [[nodiscard]] std::vector<IvNodeTypeDefinition> loaded_node_types() const;
};
} // namespace iv
