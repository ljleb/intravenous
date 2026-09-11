#pragma once

#include <intravenous/basic_nodes/weak_type_erased.h>
#include <intravenous/graph/build_types.h>
#include <intravenous/module/dependency.h>
#include <intravenous/module/loader.h>
#include <intravenous/module/watcher.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/startup_config.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
struct TasksRunnerBeforePass;
struct IvModuleReloadFailure {
    std::string package_id{};
    std::filesystem::path package_root{};
    std::string message{};
};

struct IvModuleReloadedPackage {
    std::string package_id{};
    std::filesystem::path package_root{};
    std::vector<ModuleDependency> dependencies{};
};

struct IvModuleReloadResults {
    // A successful source result is present even when the source currently
    // publishes zero IV modules, allowing transactional removal of its prior
    // definitions.
    std::vector<IvModuleReloadedPackage> packages{};
    std::vector<IvModuleReloadedDefinition> loaded{};
    std::vector<IvModuleReloadedNodeType> node_types{};
    std::vector<IvModuleReloadFailure> failed{};
};

struct ProjectOverrideSettingsRequest;
class ProjectPersistenceBuilder;

class IvModuleReload {
    StartupConfigState startup_config;
    std::unique_ptr<ModuleLoader> loader_;
    mutable std::mutex mutex;
    std::unordered_map<std::string, IvPackageDeclaration> package_declarations_by_id;
    std::unordered_map<std::string, std::vector<ModuleDependency>> dependencies_by_package_id;
    std::unordered_set<std::string> dirty_package_ids;
    IvModuleReloadResults pending_results;
    DependencyWatcher watcher;

    // Bridge-only owners may intentionally have no startup configuration.
    // Construct the persistent loader only when there is a package to load.
    // Once created it remains the same loader, preserving its shared ORC JIT
    // and the lifetime of previously configured package revisions.
    [[nodiscard]] ModuleLoader& ensure_loader();
    [[nodiscard]] IvModuleReloadResults reload_packages(
        std::vector<IvPackageDeclaration> const &declarations);
    void refresh_watched_dependencies_locked();
    void emit_status(
        std::string level,
        std::string code,
        std::string message,
        std::filesystem::path package_root = {});

public:
    explicit IvModuleReload(StartupConfigState startup_config_);

    void set_toolchain_config(ModuleLoaderToolchainConfig toolchain);
    [[nodiscard]] ModuleLoaderToolchainConfig toolchain_config() const;
    void handle_project_override_settings(ProjectOverrideSettingsRequest const &request);
    void handle_project_persistence_collect_state(ProjectPersistenceBuilder &builder) const;
    void handle_package_declarations_changed(
        IvPackageDeclarationsChanged const &diff);

    [[nodiscard]] bool has_dirty_packages() const;
    bool has_changes();
    void compile_dirty_packages();
    void reload_changed_packages();
    [[nodiscard]] bool has_pending_results() const;
    void apply_pending_results();
    void handle_task_runner_before_pass(TasksRunnerBeforePass const &pass);
};
} // namespace iv
