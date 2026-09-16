#include <intravenous/runtime/package_watcher.h>

#include <intravenous/module/package_manifest.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/package_pipeline_events.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace iv {
namespace {
std::filesystem::path normalize_path(std::filesystem::path const& path)
{
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error) {
        error.clear();
        canonical = std::filesystem::absolute(path, error);
    }
    return (error ? path : canonical).lexically_normal();
}

std::string dependency_root_key(std::filesystem::path const& path)
{
    return normalize_path(path).generic_string();
}

std::filesystem::file_time_type package_source_stamp(std::filesystem::path const& dir)
{
    std::error_code error;
    if (!std::filesystem::exists(dir, error) || error) return {};

    std::filesystem::file_time_type latest{};
    bool saw_file = false;
    auto const options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(dir, options, error), end;
         !error && it != end;
         it.increment(error)) {
        auto const& entry = *it;
        if (entry.is_directory()) {
            if (is_module_dependency_ignored_directory(entry.path())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!entry.is_regular_file() || !is_module_dependency_package_path(entry.path())) {
            continue;
        }
        std::error_code stamp_error;
        auto const stamp = std::filesystem::last_write_time(entry.path(), stamp_error);
        if (stamp_error) continue;
        latest = saw_file ? std::max(latest, stamp) : stamp;
        saw_file = true;
    }
    if (error) return {};
    if (saw_file) return latest;
    return std::filesystem::last_write_time(dir, error);
}

ModuleDependency source_dependency(IvPackageDeclaration const& declaration)
{
    return ModuleDependency{
        .id = declaration.package_id,
        .module_dir = declaration.package_root,
        .entry_file = declaration.package_root / std::string(IV_PACKAGE_MANIFEST_FILE),
        .package_stamp = package_source_stamp(declaration.package_root),
    };
}

std::string format_rebuild_duration(std::chrono::steady_clock::duration duration)
{
    return std::to_string(
               std::chrono::duration_cast<std::chrono::milliseconds>(duration).count())
        + " ms";
}

struct PackageManifest {
    std::filesystem::path entry;
};

std::optional<PackageManifest> read_manifest(std::filesystem::path const& path)
{
    std::ifstream in(path);
    if (!in) return std::nullopt;
    try {
        auto json = nlohmann::json::parse(in);
        if (json.value("schema", 0) != 2 || !json.contains("entry")) return std::nullopt;
        auto entry = std::filesystem::path(json.at("entry").get<std::string>());
        if (entry.empty() || entry.is_absolute()) return std::nullopt;
        return PackageManifest{std::move(entry)};
    } catch (nlohmann::json::exception const&) {
        return std::nullopt;
    }
}
} // namespace

std::vector<std::pair<std::string, std::filesystem::path>>
discover_iv_package_declarations(
    std::filesystem::path const& project_root,
    std::vector<std::filesystem::path> const& shared_roots)
{
    std::unordered_map<std::string, std::filesystem::path> by_id;
    auto scan = [&](std::filesystem::path const& root) {
        std::error_code error;
        if (!std::filesystem::exists(root, error)) return;
        auto const options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator it(root, options, error), end;
             !error && it != end;
             it.increment(error)) {
            auto const& entry = *it;
            if (entry.is_directory()) {
                auto const name = entry.path().filename();
                if (name == ".git" || name == "build" || name == ".cache") {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!entry.is_regular_file()
                || !is_iv_package_manifest_file(entry.path().filename().string())) {
                continue;
            }
            auto const directory = entry.path().parent_path();
            auto manifest = read_manifest(entry.path());
            if (!manifest || !std::filesystem::is_regular_file(directory / manifest->entry)) {
                continue;
            }
            auto const normalized = normalize_path(directory);
            by_id.insert_or_assign(normalized.generic_string(), normalized);
            it.disable_recursion_pending();
        }
    };
    scan(project_root);
    for (auto const& root : shared_roots) scan(root);

    std::vector<std::pair<std::string, std::filesystem::path>> result;
    result.reserve(by_id.size());
    for (auto& [package_id, package_root] : by_id) {
        result.emplace_back(package_id, std::move(package_root));
    }
    std::ranges::sort(result, {}, &std::pair<std::string, std::filesystem::path>::first);
    return result;
}

PackageWatcher::PackageWatcher()
    : dependency_watcher_(make_dependency_watcher())
{}

std::unordered_map<std::string, IvPackageDeclaration>
PackageWatcher::merge_declaration_sources_locked() const
{
    auto merged = retained_package_declarations_by_id_;
    for (auto const& [package_id, declaration] : discovered_package_declarations_by_id_) {
        auto const [position, inserted] = merged.emplace(package_id, declaration);
        if (!inserted && position->second.package_root != declaration.package_root) {
            throw std::runtime_error(
                "IV package declaration sources disagree about package ID '"
                + package_id + "'");
        }
    }
    return merged;
}

void PackageWatcher::install_effective_declarations_locked(
    std::unordered_map<std::string, IvPackageDeclaration> next)
{
    for (auto const& [package_id, declaration] : next) {
        auto const current = package_declarations_by_id_.find(package_id);
        if (current == package_declarations_by_id_.end()
            || current->second.package_root != declaration.package_root) {
            dirty_package_ids_.insert(package_id);
            source_dependencies_by_package_id_.insert_or_assign(
                package_id, source_dependency(declaration));
        }
    }
    for (auto const& [package_id, _] : package_declarations_by_id_) {
        if (!next.contains(package_id)) {
            dirty_package_ids_.erase(package_id);
            source_dependencies_by_package_id_.erase(package_id);
            dependencies_by_package_id_.erase(package_id);
        }
    }
    package_declarations_by_id_ = std::move(next);
    refresh_watched_dependencies_locked();
}

void PackageWatcher::refresh_watched_dependencies_locked()
{
    std::vector<ModuleDependency> dependencies;
    dependencies.reserve(source_dependencies_by_package_id_.size());
    for (auto const& [_, source] : source_dependencies_by_package_id_) {
        dependencies.push_back(source);
    }
    for (auto const& [_, package_dependencies] : dependencies_by_package_id_) {
        dependencies.insert(
            dependencies.end(),
            package_dependencies.begin(),
            package_dependencies.end());
    }
    dependency_watcher_.update(std::move(dependencies));
}

void PackageWatcher::handle_required_definitions_changed(
    IvModuleRequiredDefinitionsChanged const& diff)
{
    std::scoped_lock lock(mutex_);
    auto retain = [&](IvModuleRequiredDefinition const& required) {
        auto package_root = normalize_path(required.package_root);
        auto package_id = package_root.generic_string();
        retained_package_declarations_by_id_.insert_or_assign(
            package_id,
            IvPackageDeclaration{
                .package_id = package_id,
                .package_root = std::move(package_root),
            });
    };
    for (auto const& required : diff.created) retain(required);
    for (auto const& required : diff.updated) retain(required);
    install_effective_declarations_locked(merge_declaration_sources_locked());
}

void PackageWatcher::synchronize_discovered_packages(
    std::vector<std::pair<std::string, std::filesystem::path>> declarations)
{
    std::unordered_map<std::string, IvPackageDeclaration> next_discovered;
    next_discovered.reserve(declarations.size());
    for (auto& [package_id, package_root] : declarations) {
        if (package_id.empty()) {
            throw std::runtime_error("discovered IV package has an empty package ID");
        }
        auto declaration = IvPackageDeclaration{
            .package_id = std::move(package_id),
            .package_root = normalize_path(package_root),
        };
        if (!next_discovered.emplace(declaration.package_id, declaration).second) {
            throw std::runtime_error(
                "discovered IV package snapshot contains duplicate package ID '"
                + declaration.package_id + "'");
        }
    }

    std::scoped_lock lock(mutex_);
    discovered_package_declarations_by_id_ = std::move(next_discovered);
    install_effective_declarations_locked(merge_declaration_sources_locked());
}

void PackageWatcher::poll_dependency_changes()
{
    std::scoped_lock lock(mutex_);
    auto const changed_dependencies = dependency_watcher_.changed_dependencies();
    if (changed_dependencies.empty()) return;

    std::unordered_set<std::string> changed_roots;
    for (auto const& dependency : changed_dependencies) {
        changed_roots.insert(dependency_root_key(dependency.module_dir));
    }
    for (auto const& [package_id, source] : source_dependencies_by_package_id_) {
        if (changed_roots.contains(dependency_root_key(source.module_dir))) {
            dirty_package_ids_.insert(package_id);
        }
    }
    for (auto const& [package_id, dependencies] : dependencies_by_package_id_) {
        if (std::ranges::any_of(dependencies, [&](ModuleDependency const& dependency) {
                return changed_roots.contains(dependency_root_key(dependency.module_dir));
            })) {
            dirty_package_ids_.insert(package_id);
        }
    }
}

bool PackageWatcher::has_pending_refresh() const
{
    std::scoped_lock lock(mutex_);
    if (!dirty_package_ids_.empty()) return true;
    if (package_declarations_by_id_.size() != published_declarations_by_id_.size()) return true;
    for (auto const& [package_id, declaration] : package_declarations_by_id_) {
        auto const published = published_declarations_by_id_.find(package_id);
        if (published == published_declarations_by_id_.end()
            || published->second.package_root != declaration.package_root) {
            return true;
        }
    }
    return false;
}

void PackageWatcher::emit_status(
    std::string level,
    std::string code,
    std::string message,
    std::filesystem::path package_root)
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectStatusNotification{
            .level = std::move(level),
            .code = std::move(code),
            .message = std::move(message),
            .package_root = std::move(package_root),
        }));
}

bool PackageWatcher::refresh_pending_packages()
{
    PackageRefreshTransaction transaction;
    PackageJitBatchRequest jit_request;
    std::unordered_map<std::string, IvPackageDeclaration> next_published;
    {
        std::scoped_lock lock(mutex_);
        next_published = package_declarations_by_id_;

        for (auto const& [package_id, declaration] : package_declarations_by_id_) {
            auto const published = published_declarations_by_id_.find(package_id);
            if (published == published_declarations_by_id_.end()) {
                transaction.declarations.created.push_back(declaration);
            } else if (published->second.package_root != declaration.package_root) {
                transaction.declarations.updated.push_back(declaration);
                jit_request.removed_package_roots.push_back(published->second.package_root);
            }
        }
        for (auto const& [package_id, declaration] : published_declarations_by_id_) {
            if (!package_declarations_by_id_.contains(package_id)) {
                transaction.declarations.deleted_package_ids.push_back(package_id);
                jit_request.removed_package_roots.push_back(declaration.package_root);
            }
        }

        jit_request.declarations.reserve(dirty_package_ids_.size());
        for (auto const& package_id : dirty_package_ids_) {
            if (auto const declaration = package_declarations_by_id_.find(package_id);
                declaration != package_declarations_by_id_.end()) {
                jit_request.declarations.push_back(declaration->second);
                // Baseline immediately before the build. If the source changes
                // while PackageJit is running, the next watcher poll observes it.
                source_dependencies_by_package_id_.insert_or_assign(
                    package_id, source_dependency(declaration->second));
                if (auto dependencies = dependencies_by_package_id_.find(package_id);
                    dependencies != dependencies_by_package_id_.end()) {
                    for (auto& dependency : dependencies->second) {
                        dependency.package_stamp = package_source_stamp(dependency.module_dir);
                    }
                }
            }
        }
        dirty_package_ids_.clear();
    }

    auto const has_declaration_changes = !transaction.declarations.created.empty()
        || !transaction.declarations.updated.empty()
        || !transaction.declarations.deleted_package_ids.empty();
    if (!has_declaration_changes && jit_request.declarations.empty()
        && jit_request.removed_package_roots.empty()) {
        return false;
    }

    if (!jit_request.declarations.empty()) {
        emit_status(
            "info",
            "rebuildStarted",
            jit_request.declarations.size() == 1
                ? "Building IV package"
                : "Building " + std::to_string(jit_request.declarations.size())
                    + " IV packages",
            jit_request.declarations.size() == 1
                ? jit_request.declarations.front().package_root
                : std::filesystem::path{});
    }

    auto const rebuild_started_at = std::chrono::steady_clock::now();
    if (!jit_request.declarations.empty() || !jit_request.removed_package_roots.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_package_jit_batch_requested_event,
            jit_request);
    }
    auto const rebuild_duration = format_rebuild_duration(
        std::chrono::steady_clock::now() - rebuild_started_at);

    {
        std::scoped_lock lock(mutex_);
        for (auto const& package_id : transaction.declarations.deleted_package_ids) {
            dependencies_by_package_id_.erase(package_id);
        }
        for (auto const& revision : jit_request.result.revisions) {
            dependencies_by_package_id_[revision.package_id] = revision.dependencies;
        }
        refresh_watched_dependencies_locked();
    }

    if (!jit_request.result.failed.empty()) {
        auto const& failure = jit_request.result.failed.front();
        emit_status(
            "error",
            "rebuildFailed",
            "Module build failed after " + rebuild_duration + ": " + failure.message,
            failure.package_root);
    } else if (!jit_request.declarations.empty()) {
        emit_status(
            "info",
            "rebuildFinished",
            "IV package build ready to apply in " + rebuild_duration,
            jit_request.declarations.size() == 1
                ? jit_request.declarations.front().package_root
                : std::filesystem::path{});
    }

    transaction.successful_revisions = std::move(jit_request.result.revisions);
    transaction.failed_builds = std::move(jit_request.result.failed);
    IV_INVOKE_LINKER_EVENT(iv_runtime_package_refresh_event, transaction);

    {
        std::scoped_lock lock(mutex_);
        published_declarations_by_id_ = std::move(next_published);
    }
    return true;
}

void PackageWatcher::handle_refresh_requested(PackageWatcherRefreshRequest& request)
{
    request.changed = refresh_pending_packages();
}
} // namespace iv
