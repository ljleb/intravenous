#pragma once

#include <intravenous/module/watcher.h>
#include <intravenous/runtime/node_definition_types.h>
#include <intravenous/runtime/package_pipeline_types.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iv {
struct IvModuleRequiredDefinitionsChanged;

[[nodiscard]] std::vector<std::pair<std::string, std::filesystem::path>>
discover_iv_package_declarations(
    std::filesystem::path const& project_root,
    std::vector<std::filesystem::path> const& shared_roots);

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

    [[nodiscard]] std::unordered_map<std::string, IvPackageDeclaration>
    merge_declaration_sources_locked() const;
    void install_effective_declarations_locked(
        std::unordered_map<std::string, IvPackageDeclaration> next);
    void refresh_watched_dependencies_locked();
    [[nodiscard]] bool refresh_pending_packages();
    void emit_status(
        std::string level,
        std::string code,
        std::string message,
        std::filesystem::path package_root = {});

public:
    PackageWatcher();

    void handle_required_definitions_changed(
        IvModuleRequiredDefinitionsChanged const& diff);
    void synchronize_discovered_packages(
        std::vector<std::pair<std::string, std::filesystem::path>> declarations);
    void poll_dependency_changes();
    [[nodiscard]] bool has_pending_refresh() const;
    void handle_refresh_requested(PackageWatcherRefreshRequest& request);
};
} // namespace iv
