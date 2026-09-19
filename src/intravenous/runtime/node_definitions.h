#pragma once

#include <intravenous/runtime/node_definition_types.h>
#include <intravenous/runtime/package_pipeline_types.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
class NodeDefinitions {
public:
    struct ModuleDefinitionState {
        std::vector<ModuleRef> module_refs{};
        std::uint64_t package_revision = 0;
        std::uint64_t version = 0;
        ModuleNodeDefinition snapshot{};
    };
    struct LeafDefinitionState {
        std::vector<ModuleRef> module_refs{};
        std::uint64_t package_revision = 0;
        std::uint64_t version = 0;
        LeafNodeDefinition snapshot{};
    };

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<ModuleDefinitionState>>
        loaded_module_definitions_by_id_;
    std::unordered_map<std::string, std::unique_ptr<LeafDefinitionState>>
        loaded_leaf_definitions_by_id_;
    std::uint64_t next_definition_version_ = 1;
    std::uint64_t snapshot_generation_ = 0;
    std::shared_ptr<NodeDefinitionsSnapshot const> definitions_snapshot_;

    void fill_publication_projection_locked(
        PackageDefinitionsPublicationRequest& request) const;

public:
    NodeDefinitions();
    ~NodeDefinitions();

    void handle_package_definitions_publication(
        PackageDefinitionsPublicationRequest& request);

    // Convenience entry point retained for focused module/bridge tests. It
    // exercises the same accepted-revision publication path as PackageDefinitions.
    void seed_loaded_definition(PackageModuleDefinition loaded_definition);

    [[nodiscard]] std::shared_ptr<NodeDefinitionsSnapshot const> snapshot() const;
    [[nodiscard]] std::vector<ModuleNodeDefinition> loaded_module_definitions() const;
    [[nodiscard]] std::vector<LeafNodeDefinition> loaded_leaf_definitions() const;
};
} // namespace iv
