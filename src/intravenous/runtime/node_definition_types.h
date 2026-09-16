#pragma once

#include <intravenous/basic_nodes/weak_type_erased.h>
#include <intravenous/graph/build_types.h>
#include <intravenous/module/dependency.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/node/compiler_record.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
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

// Temporary compatibility projection for legacy runtime consumers. Package
// catalog/publication diagnostics return synchronously to PackageDefinitions
// and are deliberately not carried on this downstream-only event.
struct IvPackageDefinitionsChanged {
    ModuleNodeDefinitionsChanged module_definitions{};
    LeafNodeDefinitionsChanged leaf_definitions{};
};
} // namespace iv
