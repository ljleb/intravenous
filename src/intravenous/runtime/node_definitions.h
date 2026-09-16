#pragma once

#include <intravenous/basic_nodes/weak_type_erased.h>
#include <intravenous/graph/build_types.h>
#include <intravenous/module/dependency.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/node/compiler_record.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
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

// Invocation surface retained from one loaded provider revision. Function and
// signature pointers are valid while the owning definition's module_refs live.
// Typed value operations for cached configuration arguments will extend this
// record when NodeInstances is generalized.
struct NodeDefinitionProvider {
    details::IvModuleConfigureFunction module_build = nullptr;
    details::NodeTypeConfigureFunction leaf_build = nullptr;
    details::RegisteredSignatureFunction signature = nullptr;
};

struct ModuleNodeDefinition {
    // Published definitions are keyed by stable IV module ID. Package ownership
    // is retained separately and never inferred from the module ID or source path.
    std::string definition_id{};
    std::string package_id{};
    std::filesystem::path package_root{};
    std::string module_id{};
    NodeDefinitionProvider provider{};
    GraphIntrospectionMetadata introspection{};
    std::vector<ModuleDependency> dependencies{};
    std::vector<ModuleRef> module_refs{};
    WeakTypeErasedNode root{};
    std::shared_ptr<ConfiguredGraph const> configured_graph{};
};

struct ModuleNodeDefinitionsChanged {
    std::vector<ModuleNodeDefinition> created{};
    std::vector<ModuleNodeDefinition> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

// Leaf nodes are first-class registry definitions. The stable ID is the
// application identity; NodeCodeKey and compiler callbacks are specific to this
// loaded artifact and stay valid through module_refs.
struct LeafNodeDefinition {
    std::string definition_id{};
    std::string package_id{};
    std::filesystem::path package_root{};
    NodeDefinitionProvider provider{};
    details::NodeCompilerRecord compiler_record{};
    std::vector<ModuleRef> module_refs{};
};

struct LeafNodeDefinitionsChanged {
    std::vector<LeafNodeDefinition> created{};
    std::vector<LeafNodeDefinition> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

enum class NodeDefinitionKind {
    leaf,
    module,
};

// One immutable published definition entry. `version` changes whenever the
// provider for this stable ID is republished, even when the ID and package
// ownership stay the same. The concrete provider payload remains specialized
// while callers that only need registry identity can use the common fields.
struct NodeDefinitionEntry {
    std::string definition_id{};
    NodeDefinitionKind kind = NodeDefinitionKind::leaf;
    std::uint64_t version = 0;
    std::variant<LeafNodeDefinition, ModuleNodeDefinition> definition{};
};

// Complete coherent registry generation. A caller keeps this object for an
// entire configuration transaction so nested definition resolution can never
// observe a different provider generation halfway through the batch.
struct NodeDefinitionsSnapshot {
    std::uint64_t generation = 0;
    std::unordered_map<std::string, NodeDefinitionEntry> by_id{};
};

struct IvPackageDefinitionsChanged {
    ModuleNodeDefinitionsChanged module_definitions{};
    LeafNodeDefinitionsChanged leaf_definitions{};
    // Package-level publication diagnostics are registry state, not reload
    // state. Publish them with the definition diff so package tooling does
    // not need a direct reference back to NodeDefinitions.
    std::unordered_map<std::string, std::string> publication_messages_by_package_id{};
};

// One coherent read of the package registry. Published IDs are the only IDs
// that can be instantiated or resolved by a graph. A non-empty publication
// message means the package produced a candidate that deliberately remains
// unavailable (for example, an invalid local registration or an ID conflict).
struct IvPackageDefinitionSnapshot {
    IvPackageDeclaration declaration{};
    std::vector<std::string> published_module_definition_ids{};
    std::vector<std::string> published_leaf_definition_ids{};
    std::string publication_message{};
};

// A successful package build contributes this complete candidate definition set.
// It remains stored while another package temporarily conflicts with one of its IDs.
struct IvPackageReloadedDefinition {
    std::string package_id{};
    std::string definition_id{};
    std::filesystem::path package_root{};
    std::string module_id{};
    NodeDefinitionProvider provider{};
    GraphIntrospectionMetadata introspection{};
    std::vector<ModuleDependency> dependencies{};
    std::vector<ModuleRef> module_refs{};
    WeakTypeErasedNode root{};
    std::shared_ptr<ConfiguredGraph const> configured_graph{};
};

struct IvPackageReloadedNodeType {
    std::string package_id{};
    std::string node_type_id{};
    std::filesystem::path package_root{};
    NodeDefinitionProvider provider{};
    details::NodeCompilerRecord compiler_record{};
    std::vector<ModuleRef> module_refs{};
};

struct IvModuleRequiredDefinitionsChanged;
struct IvPackageReloadResults;

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
        std::vector<IvPackageReloadedDefinition> module_definitions{};
        std::vector<IvPackageReloadedNodeType> leaf_definitions{};
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
    void handle_reload_results(IvPackageReloadResults const &results);

    void seed_loaded_definition(IvPackageReloadedDefinition loaded_definition);

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
