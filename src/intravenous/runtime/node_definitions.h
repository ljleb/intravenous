#pragma once

#include <intravenous/runtime/node_definition_types.h>
#include <intravenous/runtime/package_reload_types.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iv {
struct IvModuleRequiredDefinitionsChanged;

class NodeDefinitions {
public:
    struct ModuleDefinitionState {
        std::vector<ModuleRef> module_refs{};
        std::uint64_t version = 0;
        ModuleNodeDefinition snapshot{};
    };
    struct LeafDefinitionState {
        std::vector<ModuleRef> module_refs{};
        std::uint64_t version = 0;
        LeafNodeDefinition snapshot{};
    };

private:
    struct PackageCandidate {
        std::vector<PackageReloadedModuleDefinition> module_definitions{};
        std::vector<PackageReloadedLeafDefinition> leaf_definitions{};
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
    std::unordered_map<std::string, std::unique_ptr<ModuleDefinitionState>> loaded_module_definitions_by_id;
    std::unordered_map<std::string, std::unique_ptr<LeafDefinitionState>> loaded_leaf_definitions_by_id;
    // One shared ownership map enforces the single stable-ID namespace across
    // primitive node types and iv modules.
    std::unordered_map<std::string, std::string> package_id_by_definition_id;
    std::unordered_map<std::string, std::vector<std::string>> module_definition_ids_by_package_id;
    // Candidate sets are independent from publication. Moving a definition between
    // packages can temporarily create a duplicate ID without discarding either
    // package candidate; the remaining candidate becomes live when the conflict ends.
    std::unordered_map<std::string, PackageCandidate> candidates_by_package_id;
    // A finalizer can return a malformed candidate even when the package build
    // itself succeeded. Keep that diagnostic in the registry so the package
    // catalog does not mislabel it as an empty package.
    std::unordered_map<std::string, std::string>
        candidate_validation_messages_by_package_id;
    std::uint64_t next_definition_version_ = 1;
    std::uint64_t snapshot_generation_ = 0;
    std::shared_ptr<NodeDefinitionsSnapshot const> definitions_snapshot_;

    [[nodiscard]] std::unordered_map<std::string, IvPackageDeclaration>
    merge_declaration_sources_locked(
        std::unordered_map<std::string, IvPackageDeclaration> const& retained,
        std::unordered_map<std::string, IvPackageDeclaration> const& discovered) const;
    void declare_packages(std::vector<IvPackageDeclaration> declarations);
    void rebuild_published_registry_locked(
        ModuleNodeDefinitionsChanged& diff,
        LeafNodeDefinitionsChanged& leaf_diff,
        std::unordered_set<std::string> const& changed_package_ids);
    void publish_package_definitions_changed(
        ModuleNodeDefinitionsChanged modules,
        LeafNodeDefinitionsChanged leaf_definitions,
        bool force = false) const;
public:
    NodeDefinitions();
    ~NodeDefinitions();

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
    void handle_reload_results(PackageReloadResults const &results);

    void seed_loaded_definition(PackageReloadedModuleDefinition loaded_definition);

    // The declaration registry is the authoritative package catalog. UI and
    // project services must query this coherent snapshot rather than scanning
    // the filesystem or combining independently-read change caches.
    [[nodiscard]] std::vector<IvPackageDefinitionSnapshot>
    package_definition_snapshots() const;
    [[nodiscard]] std::shared_ptr<NodeDefinitionsSnapshot const> snapshot() const;
    [[nodiscard]] std::vector<ModuleNodeDefinition> loaded_module_definitions() const;
    [[nodiscard]] std::vector<LeafNodeDefinition> loaded_leaf_definitions() const;
};
} // namespace iv
