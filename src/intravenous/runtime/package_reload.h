#pragma once

#include <intravenous/module/loader.h>
#include <intravenous/module/watcher.h>
#include <intravenous/runtime/package_reload_types.h>
#include <intravenous/runtime/startup_config.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
struct ProjectOverrideSettingsRequest;
class ProjectPersistenceBuilder;

class PackageReload {
    StartupConfigState startup_config;
    ModuleLoader::OptimizationLevel loader_optimization_level_;
    std::unique_ptr<ModuleLoader> loader_;
    mutable std::mutex mutex;
    std::unordered_map<std::string, IvPackageDeclaration> package_declarations_by_id;
    std::unordered_map<std::string, std::vector<ModuleDependency>> dependencies_by_package_id;
    std::unordered_set<std::string> dirty_package_ids;
    std::unordered_map<std::string, PackageBuildStatus> build_status_by_package_id;
    PackageReloadResults pending_results;
    DependencyWatcher watcher;

    // Bridge-only owners may intentionally have no startup configuration.
    // Construct the persistent loader only when there is a package to load.
    // Once created it remains the same loader, preserving its shared ORC JIT
    // and the lifetime of previously configured package revisions.
    [[nodiscard]] ModuleLoader& ensure_loader();
    [[nodiscard]] PackageReloadResults reload_packages(
        std::vector<IvPackageDeclaration> const &declarations);
    void refresh_watched_dependencies_locked();
    void publish_build_statuses() const;
    void emit_status(
        std::string level,
        std::string code,
        std::string message,
        std::filesystem::path package_root = {});

public:
    explicit PackageReload(
        StartupConfigState startup_config_,
        ModuleLoader::OptimizationLevel loader_optimization_level =
            ModuleLoader::OptimizationLevel::O3);

    void set_toolchain_config(ModuleLoaderToolchainConfig toolchain);
    [[nodiscard]] ModuleLoaderToolchainConfig toolchain_config() const;
    void handle_project_override_settings(ProjectOverrideSettingsRequest const &request);
    void handle_project_persistence_collect_state(ProjectPersistenceBuilder &builder) const;
    void handle_package_declarations_changed(
        IvPackageDeclarationsChanged const &diff);

    [[nodiscard]] bool has_dirty_packages() const;
    [[nodiscard]] std::vector<PackageBuildStatus> package_build_statuses() const;
    void compile_dirty_packages();
    void reload_changed_packages();
    [[nodiscard]] bool has_pending_results() const;
    bool apply_pending_results();
};
} // namespace iv
