#pragma once

#include <intravenous/basic_nodes/weak_type_erased.h>
#include <intravenous/graph/build_types.h>
#include <intravenous/module/dependency.h>
#include <intravenous/node/compiler_record.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
using ModuleRef = std::shared_ptr<void>;
struct AuthoredGraph;

struct IvModuleDefinitionDeclaration {
    // A declaration schedules one IV source build. Its definition_id is the
    // source package key only at this private/reload boundary.
    std::string definition_id{};
    std::filesystem::path module_root{};
};

struct IvModuleDefinitionDeclarationsChanged {
    std::vector<IvModuleDefinitionDeclaration> created{};
    std::vector<IvModuleDefinitionDeclaration> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

struct IvModuleDefinition {
    // Published definitions are keyed by registered IV module ID.  The source
    // identity is retained separately so source ownership never has to be
    // inferred from the module ID or source path.
    std::string definition_id{};
    std::string source_id{};
    std::filesystem::path module_root{};
    std::string module_id{};
    GraphIntrospectionMetadata introspection{};
    std::vector<ModuleDependency> dependencies{};
    std::vector<ModuleRef> module_refs{};
    WeakTypeErasedNode root{};
    std::shared_ptr<AuthoredGraph const> authored_graph{};
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
    std::string source_id{};
    std::filesystem::path source_root{};
    details::NodeCompilerRecord compiler_record{};
    std::vector<ModuleRef> module_refs{};
    std::shared_ptr<AuthoredGraph const> authored_graph{};
};

struct IvNodeTypeDefinitionsChanged {
    std::vector<IvNodeTypeDefinition> created{};
    std::vector<IvNodeTypeDefinition> updated{};
    std::vector<std::string> deleted_node_type_ids{};
};

struct IvModuleDefinitionsMessage {
    std::string level = "info";
    std::string message{};
    std::filesystem::path module_root{};
};

using IvModuleDefinitionsNotification = IvModuleDefinitionsMessage;

// A successful source build contributes this complete candidate module set to
// the registry.  It is intentionally stored even while another source has a
// conflicting candidate for the same module ID.
struct IvModuleReloadedDefinition {
    std::string source_id{};
    std::string definition_id{};
    std::filesystem::path module_root{};
    std::string module_id{};
    GraphIntrospectionMetadata introspection{};
    std::vector<ModuleDependency> dependencies{};
    std::vector<ModuleRef> module_refs{};
    WeakTypeErasedNode root{};
    std::shared_ptr<AuthoredGraph const> authored_graph{};
};

struct IvModuleReloadedNodeType {
    std::string source_id{};
    std::string node_type_id{};
    std::filesystem::path source_root{};
    details::NodeCompilerRecord compiler_record{};
    std::vector<ModuleRef> module_refs{};
    std::shared_ptr<AuthoredGraph const> authored_graph{};
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
    struct SourceCandidate {
        std::vector<IvModuleReloadedDefinition> modules{};
        std::vector<IvModuleReloadedNodeType> node_types{};
    };

    mutable std::mutex mutex;
    std::unordered_map<std::string, IvModuleDefinitionDeclaration> declarations_by_source_id;
    std::unordered_map<std::string, std::unique_ptr<DefinitionState>> loaded_definitions_by_module_id;
    std::unordered_map<std::string, std::unique_ptr<NodeTypeState>> loaded_node_types_by_id;
    // One shared ownership map enforces the single stable-ID namespace across
    // primitive node types and iv modules.
    std::unordered_map<std::string, std::string> source_id_by_registered_id;
    std::unordered_map<std::string, std::vector<std::string>> module_ids_by_source_id;
    // Candidate sets are independent from publication.  A source move can
    // temporarily create a duplicate ID without discarding the destination
    // candidate, and the remaining candidate becomes live as soon as its
    // conflict disappears.
    std::unordered_map<std::string, SourceCandidate> candidates_by_source_id;

    void emit_notification(IvModuleDefinitionsNotification notification) const;
    void emit_message(std::string level, std::string message, std::filesystem::path module_root = {}) const;
    void rebuild_published_registry_locked(
        IvModuleDefinitionsChanged& diff,
        IvNodeTypeDefinitionsChanged& node_type_diff,
        std::vector<IvModuleDefinitionsMessage>& failures,
        std::unordered_set<std::string> const& changed_source_ids);
public:
    IvModuleDefinitions() = default;
    ~IvModuleDefinitions();

    std::string declare_definition(
        std::string definition_id,
        std::filesystem::path module_root);
    void remove_definition(std::string const &definition_id);

    void handle_required_definitions_changed(
        IvModuleRequiredDefinitionsChanged const &diff);
    void handle_reload_results(IvModuleReloadResults const &results);

    void seed_loaded_definition(IvModuleReloadedDefinition loaded_definition);

    [[nodiscard]] std::vector<IvModuleDefinition> loaded_definitions() const;
    [[nodiscard]] std::vector<IvNodeTypeDefinition> loaded_node_types() const;
};
} // namespace iv
