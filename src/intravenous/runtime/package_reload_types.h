#pragma once

#include <intravenous/runtime/node_definition_types.h>

#include <filesystem>
#include <string>
#include <vector>

namespace iv {
// A successful package build contributes one complete candidate definition set.
// NodeDefinitions decides which candidates become published in its immutable
// registry snapshot.
struct PackageReloadedModuleDefinition {
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

struct PackageReloadedLeafDefinition {
    std::string package_id{};
    std::string definition_id{};
    std::filesystem::path package_root{};
    NodeDefinitionProvider provider{};
    details::NodeCompilerRecord compiler_record{};
    std::vector<ModuleRef> module_refs{};
};

struct PackageReloadFailure {
    std::string package_id{};
    std::filesystem::path package_root{};
    std::string message{};
};

struct PackageReloadedPackage {
    std::string package_id{};
    std::filesystem::path package_root{};
    std::vector<ModuleDependency> dependencies{};
};

struct PackageReloadResults {
    // A successful source result is present even when the source currently
    // publishes zero node definitions, allowing transactional removal of its
    // prior definitions.
    std::vector<PackageReloadedPackage> packages{};
    std::vector<PackageReloadedModuleDefinition> module_definitions{};
    std::vector<PackageReloadedLeafDefinition> leaf_definitions{};
    std::vector<PackageReloadFailure> failed{};
};

// Compilation is deliberately separate from definition publication. A package
// can be discovered before it has a usable artifact, and a failed rebuild must
// not be misrepresented to the UI as a source with no definitions.
enum class PackageBuildState {
    queued,
    building,
    built,
    failed,
};

struct PackageBuildStatus {
    std::string package_id{};
    PackageBuildState state = PackageBuildState::queued;
    std::string message{};
};
} // namespace iv
