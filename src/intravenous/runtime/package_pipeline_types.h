#pragma once

#include <intravenous/runtime/node_definition_types.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
struct PackageModuleDefinition {
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

struct PackageLeafDefinition {
    std::string package_id{};
    std::string definition_id{};
    std::filesystem::path package_root{};
    NodeDefinitionProvider provider{};
    details::NodeCompilerRecord compiler_record{};
    std::vector<ModuleRef> module_refs{};
};

// One successful, self-contained package build generation. The retained module
// references pin every provider/code object needed by the definitions in this
// revision even after a later revision is accepted.
struct PackageRevision {
    std::string package_id{};
    std::filesystem::path package_root{};
    std::uint64_t revision = 0;
    // Pins this package JIT generation, including retained LLVM/code/data.
    ModuleRef package_code{};
    std::vector<ModuleDependency> dependencies{};
    std::vector<PackageModuleDefinition> module_definitions{};
    std::vector<PackageLeafDefinition> leaf_definitions{};
};

struct PackageJitFailure {
    std::string package_id{};
    std::filesystem::path package_root{};
    std::string message{};
};

struct PackageJitBatchResult {
    std::vector<PackageRevision> revisions{};
    std::vector<PackageJitFailure> failed{};
};

struct PackageWatcherRefreshRequest {
    bool changed = false;
};

struct PackageJitBatchRequest {
    std::vector<IvPackageDeclaration> declarations{};
    std::vector<std::filesystem::path> removed_package_roots{};
    PackageJitBatchResult result{};
};

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

// One complete application-level package update emitted by PackageWatcher after
// its synchronous PackageJit request has completed and dependency watches have
// been updated. PackageDefinitions is entered exactly once per refresh cause.
struct PackageRefreshTransaction {
    IvPackageDeclarationsChanged declarations{};
    std::vector<PackageRevision> successful_revisions{};
    std::vector<PackageJitFailure> failed_builds{};
};

struct PackageDefinitionsSnapshot {
    std::uint64_t generation = 0;
    std::unordered_map<std::string, std::shared_ptr<PackageRevision const>> by_package_id{};
};

// Synchronous request/response payload for the PackageDefinitions ->
// NodeDefinitions edge. NodeDefinitions derives the global namespace and writes
// the package-facing publication projection into this object before returning.
struct PackageDefinitionsPublicationRequest {
    std::shared_ptr<PackageDefinitionsSnapshot const> snapshot{};
    std::unordered_map<std::string, std::vector<std::string>>
        published_module_ids_by_package_id{};
    std::unordered_map<std::string, std::vector<std::string>>
        published_leaf_ids_by_package_id{};
    std::unordered_map<std::string, std::string> publication_messages_by_package_id{};
};
} // namespace iv
