#pragma once

#include <intravenous/module/watcher.h>
#include <intravenous/runtime/node_definition_types.h>
#include <intravenous/runtime/package_pipeline_types.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iv {
struct IvModuleRequiredDefinitionsChanged;

[[nodiscard]] std::vector<IvPackageDeclaration>
discover_iv_package_declarations(
    std::filesystem::path const& project_root,
    std::vector<PackageDiscoveryRoot> const& package_roots);

class PackageWatcher {
    mutable std::mutex mutex_;
    std::unordered_map<std::string, IvPackageDeclaration>
        retained_package_declarations_by_id_;
    std::unordered_map<std::string, IvPackageDeclaration>
        discovered_package_declarations_by_id_;
    std::unordered_map<std::string, IvPackageDeclaration> package_declarations_by_id_;
    std::unordered_map<std::string, IvPackageDeclaration> published_declarations_by_id_;
    // Source-root watches exist even before a package has ever built successfully,
    // so an edit after an initial compiler failure can dirty the package again.
    std::unordered_map<std::string, ModuleDependency> source_dependencies_by_package_id_;
    std::unordered_map<std::string, std::vector<ModuleDependency>> dependencies_by_package_id_;
    std::unordered_set<std::string> dirty_package_ids_;
    DependencyWatcher dependency_watcher_;

    // The changes and build batch of the next refresh, taken from the dirty
    // and declaration state by prepare_refresh() so the service can announce
    // the build before the refresh cause runs.
    struct PreparedRefresh {
        PackageRefreshTransaction transaction{};
        PackageJitBatchRequest jit_request{};
        std::unordered_map<std::string, IvPackageDeclaration> next_published{};
    };
    std::optional<PreparedRefresh> prepared_refresh_;

    [[nodiscard]] std::unordered_map<std::string, IvPackageDeclaration>
    merge_declaration_sources_locked() const;
    void install_effective_declarations_locked(
        std::unordered_map<std::string, IvPackageDeclaration> next);
    void refresh_watched_dependencies_locked();
    [[nodiscard]] std::optional<PreparedRefresh> prepare_refresh_locked();
    [[nodiscard]] bool refresh_pending_packages(
        PackageWatcherRefreshRequest& request);

public:
    PackageWatcher();
    ~PackageWatcher() = default;

    void handle_required_definitions_changed(
        IvModuleRequiredDefinitionsChanged const& diff);
    void synchronize_discovered_packages(
        std::vector<IvPackageDeclaration> declarations);
    void poll_dependency_changes();
    [[nodiscard]] int dependency_watch_descriptor() const noexcept;
    [[nodiscard]] bool has_pending_refresh() const;
    // Fixes the next refresh's batch and reports it; nullopt when nothing
    // needs refreshing. A refresh without a prepared batch prepares its own.
    [[nodiscard]] std::optional<PackageWatcherRefreshPlan> prepare_refresh();
    void handle_refresh_requested(PackageWatcherRefreshRequest& request);
};
} // namespace iv
