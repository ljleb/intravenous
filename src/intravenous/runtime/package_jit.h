#pragma once

#include <intravenous/runtime/project_settings.h>

#include <intravenous/module/loader.h>
#include <intravenous/runtime/package_pipeline_types.h>
#include <intravenous/runtime/startup_config.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace iv {
struct ProjectOverrideSettingsRequest;
class ProjectPersistenceBuilder;

class PackageJit {
    StartupConfigState startup_config_;
    std::unique_ptr<ModuleLoader> loader_;
    bool loader_toolchain_dirty_ = false;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::uint64_t> next_revision_by_package_id_;

    friend class ProjectToolchainTransaction;
    void set_toolchain_config_locked(ModuleLoaderToolchainConfig toolchain);
    [[nodiscard]] ModuleLoader& ensure_loader();
    [[nodiscard]] PackageJitBatchResult build(
        std::vector<IvPackageDeclaration> const& declarations);

public:
    explicit PackageJit(StartupConfigState startup_config);

    void set_toolchain_config(ModuleLoaderToolchainConfig toolchain);
    [[nodiscard]] ModuleLoaderToolchainConfig toolchain_config() const;

    void handle_project_graph_toolchain(ProjectToolchainTransactionRequest& request);
    void handle_build_request(PackageJitBatchRequest& request);
    void handle_project_override_settings(ProjectOverrideSettingsRequest const& request);
    void handle_project_persistence_collect_state(ProjectPersistenceBuilder& builder) const;
};
} // namespace iv
