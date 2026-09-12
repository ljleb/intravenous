#include <intravenous/runtime/iv_module_reload.h>

#include <intravenous/juce/vst_runtime.h>
#include <intravenous/runtime/iv_module_reload_events.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <algorithm>
#include <exception>
#include <chrono>
#include <iterator>
#include <ranges>

namespace iv {
namespace {
std::string describe_exception(std::exception_ptr exception)
{
    if (!exception) {
        return "unknown exception";
    }

    try {
        std::rethrow_exception(exception);
    } catch (std::exception const &e) {
        return e.what();
    } catch (...) {
        return "unknown exception";
    }
}

std::string format_rebuild_duration(std::chrono::steady_clock::duration duration)
{
    return std::to_string(
               std::chrono::duration_cast<std::chrono::milliseconds>(duration).count())
        + " ms";
}

IvModuleReloadResults coalesce_results_by_package(IvModuleReloadResults results)
{
    std::unordered_map<std::string, IvModuleReloadedPackage> packages_by_id;
    std::unordered_map<std::string, std::vector<IvModuleReloadedDefinition>>
        loaded_by_package_id;
    std::unordered_map<std::string, std::vector<IvModuleReloadedNodeType>>
        node_types_by_package_id;
    std::unordered_map<std::string, IvModuleReloadFailure> failed_by_package_id;
    std::vector<std::string> order;
    order.reserve(results.packages.size() + results.failed.size());

    auto const remember_order = [&](std::string const& package_id) {
        if (std::ranges::find(order, package_id) == order.end()) {
            order.push_back(package_id);
        }
    };

    for (auto& package : results.packages) {
        auto const package_id = package.package_id;
        remember_order(package_id);
        failed_by_package_id.erase(package_id);
        loaded_by_package_id[package_id].clear();
        node_types_by_package_id[package_id].clear();
        packages_by_id[package_id] = std::move(package);
    }
    for (auto& loaded : results.loaded) {
        if (!packages_by_id.contains(loaded.package_id)) continue;
        loaded_by_package_id[loaded.package_id].push_back(std::move(loaded));
    }
    for (auto& node_type : results.node_types) {
        if (!packages_by_id.contains(node_type.package_id)) continue;
        node_types_by_package_id[node_type.package_id].push_back(std::move(node_type));
    }
    for (auto& failed : results.failed) {
        auto const package_id = failed.package_id;
        remember_order(package_id);
        packages_by_id.erase(package_id);
        loaded_by_package_id.erase(package_id);
        node_types_by_package_id.erase(package_id);
        failed_by_package_id[package_id] = std::move(failed);
    }

    IvModuleReloadResults coalesced;
    coalesced.packages.reserve(packages_by_id.size());
    coalesced.loaded.reserve(results.loaded.size());
    coalesced.node_types.reserve(results.node_types.size());
    coalesced.failed.reserve(failed_by_package_id.size());
    for (auto const& package_id : order) {
        if (auto failed = failed_by_package_id.find(package_id);
            failed != failed_by_package_id.end()) {
            coalesced.failed.push_back(std::move(failed->second));
            continue;
        }
        auto package = packages_by_id.find(package_id);
        if (package == packages_by_id.end()) continue;
        coalesced.packages.push_back(std::move(package->second));
        if (auto loaded = loaded_by_package_id.find(package_id);
            loaded != loaded_by_package_id.end()) {
            coalesced.loaded.insert(
                coalesced.loaded.end(),
                std::make_move_iterator(loaded->second.begin()),
                std::make_move_iterator(loaded->second.end()));
        }
        if (auto node_types = node_types_by_package_id.find(package_id);
            node_types != node_types_by_package_id.end()) {
            coalesced.node_types.insert(
                coalesced.node_types.end(),
                std::make_move_iterator(node_types->second.begin()),
                std::make_move_iterator(node_types->second.end()));
        }
    }
    return coalesced;
}
} // namespace

IvModuleReload::IvModuleReload(StartupConfigState startup_config_)
    : startup_config(std::move(startup_config_)),
      watcher(make_dependency_watcher())
{}

ModuleLoader& IvModuleReload::ensure_loader()
{
    std::scoped_lock lock(mutex);
    if (!loader_) {
        loader_ = std::make_unique<ModuleLoader>(
            startup_config.discovery_start,
            startup_config.search_roots,
            startup_config.toolchain);
    }
    return *loader_;
}

void IvModuleReload::set_toolchain_config(ModuleLoaderToolchainConfig toolchain)
{
    std::scoped_lock lock(mutex);
    startup_config.toolchain = toolchain;
    if (loader_) {
        loader_->set_toolchain_config(std::move(toolchain));
    }
}

ModuleLoaderToolchainConfig IvModuleReload::toolchain_config() const
{
    std::scoped_lock lock(mutex);
    return startup_config.toolchain;
}

void IvModuleReload::handle_project_override_settings(
    ProjectOverrideSettingsRequest const &request)
{
    bool touched = false;
    auto toolchain = toolchain_config();
    auto const assign_path = [&](std::optional<std::filesystem::path> const &value,
                                 std::optional<std::filesystem::path>
                                     ModuleLoaderToolchainConfig::*field) {
        if (!value.has_value() || value == toolchain.*field) {
            return;
        }
        toolchain.*field = value;
        touched = true;
    };
    auto const assign_string = [&](std::optional<std::string> const &value,
                                   std::optional<std::string>
                                       ModuleLoaderToolchainConfig::*field) {
        if (!value.has_value() || value == toolchain.*field) {
            return;
        }
        toolchain.*field = value;
        touched = true;
    };

    assign_path(request.c_compiler, &ModuleLoaderToolchainConfig::c_compiler);
    assign_path(request.cxx_compiler, &ModuleLoaderToolchainConfig::cxx_compiler);
    assign_path(request.cmake_program, &ModuleLoaderToolchainConfig::cmake_program);
    assign_string(request.cmake_generator, &ModuleLoaderToolchainConfig::cmake_generator);
    assign_path(request.make_program, &ModuleLoaderToolchainConfig::make_program);
    assign_path(request.juce_dir, &ModuleLoaderToolchainConfig::juce_dir);
    if (!touched) {
        return;
    }

    set_toolchain_config(std::move(toolchain));
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void IvModuleReload::handle_project_persistence_collect_state(
    ProjectPersistenceBuilder &builder) const
{
    builder.add_project_toolchain_config(toolchain_config());
}

void IvModuleReload::emit_status(
    std::string level,
    std::string code,
    std::string message,
    std::filesystem::path package_root)
{
    IV_INVOKE_LINKER_EVENT_SOURCE(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectStatusNotification{
            .level = std::move(level),
            .code = std::move(code),
            .message = std::move(message),
            .package_root = std::move(package_root),
        }));
}

void IvModuleReload::refresh_watched_dependencies_locked()
{
    std::vector<ModuleDependency> dependencies;
    for (auto const &entry : dependencies_by_package_id) {
        dependencies.insert(
            dependencies.end(),
            entry.second.begin(),
            entry.second.end());
    }
    watcher.update(std::move(dependencies));
}

IvModuleReloadResults IvModuleReload::reload_packages(
    std::vector<IvPackageDeclaration> const &declarations)
{
#if IV_ENABLE_JUCE_VST
    warmup_juce_vst_scan_cache();
#endif

    IvModuleReloadResults results;
    auto& loader = ensure_loader();

    for (auto const &declaration : declarations) {
        try {
            auto loaded_package = loader.load_package(declaration.package_root);
            auto dependencies = std::move(loaded_package.dependencies);
            {
                std::scoped_lock lock(mutex);
                dependencies_by_package_id[declaration.package_id] = dependencies;
                refresh_watched_dependencies_locked();
            }
            results.packages.push_back({
                .package_id = declaration.package_id,
                .package_root = declaration.package_root,
                .dependencies = dependencies,
            });
            for (auto& loaded_definition : loaded_package.definitions) {
                results.loaded.push_back(IvModuleReloadedDefinition{
                    .package_id = declaration.package_id,
                    .definition_id = loaded_definition.module_id,
                    .package_root = declaration.package_root,
                    .module_id = std::move(loaded_definition.module_id),
                    .introspection = std::move(loaded_definition.introspection),
                    .dependencies = std::move(loaded_definition.dependencies),
                    .module_refs = std::move(loaded_definition.module_refs),
                    .root = std::move(loaded_definition.root),
                    .configured_graph = std::move(loaded_definition.configured_graph),
                });
            }
            for (auto& node_type : loaded_package.node_types) {
                results.node_types.push_back(IvModuleReloadedNodeType{
                    .package_id = declaration.package_id,
                    .node_type_id = std::move(node_type.node_type_id),
                    .package_root = declaration.package_root,
                    .compiler_record = node_type.compiler_record,
                    .module_refs = std::move(node_type.module_refs),
                    .configured_graph = std::move(node_type.configured_graph),
                });
            }
        } catch (...) {
            results.failed.push_back(IvModuleReloadFailure{
                .package_id = declaration.package_id,
                .package_root = declaration.package_root,
                .message = describe_exception(std::current_exception()),
            });
        }
    }

    return results;
}

void IvModuleReload::handle_package_declarations_changed(
    IvPackageDeclarationsChanged const &diff)
{
    {
        std::scoped_lock lock(mutex);
        for (auto const &declaration : diff.created) {
            package_declarations_by_id[declaration.package_id] = declaration;
            dirty_package_ids.insert(declaration.package_id);
            build_status_by_package_id.insert_or_assign(
                declaration.package_id,
                IvPackageBuildStatus{
                    .package_id = declaration.package_id,
                    .state = IvPackageBuildState::queued,
                });
        }
        for (auto const &declaration : diff.updated) {
            package_declarations_by_id[declaration.package_id] = declaration;
            dirty_package_ids.insert(declaration.package_id);
            build_status_by_package_id.insert_or_assign(
                declaration.package_id,
                IvPackageBuildStatus{
                    .package_id = declaration.package_id,
                    .state = IvPackageBuildState::queued,
                });
        }
        for (auto const &package_id : diff.deleted_package_ids) {
            package_declarations_by_id.erase(package_id);
            dependencies_by_package_id.erase(package_id);
            dirty_package_ids.erase(package_id);
            build_status_by_package_id.erase(package_id);
        }
        refresh_watched_dependencies_locked();
    }
}

bool IvModuleReload::has_dirty_packages() const
{
    std::scoped_lock lock(mutex);
    return !dirty_package_ids.empty();
}

std::vector<IvPackageBuildStatus> IvModuleReload::package_build_statuses() const
{
    std::vector<IvPackageBuildStatus> statuses;
    {
        std::scoped_lock lock(mutex);
        statuses.reserve(build_status_by_package_id.size());
        for (auto const& [_, status] : build_status_by_package_id) {
            statuses.push_back(status);
        }
    }
    std::ranges::sort(statuses, {}, &IvPackageBuildStatus::package_id);
    return statuses;
}

void IvModuleReload::compile_dirty_packages()
{
    std::vector<IvPackageDeclaration> declarations;
    {
        std::scoped_lock lock(mutex);
        if (dirty_package_ids.empty()) {
            return;
        }
        declarations.reserve(dirty_package_ids.size());
        for (auto const &package_id : dirty_package_ids) {
            if (auto const it = package_declarations_by_id.find(package_id);
                it != package_declarations_by_id.end()) {
                declarations.push_back(it->second);
                build_status_by_package_id.insert_or_assign(
                    package_id,
                    IvPackageBuildStatus{
                        .package_id = package_id,
                        .state = IvPackageBuildState::building,
                    });
            }
        }
        dirty_package_ids.clear();
    }

    if (declarations.empty()) {
        return;
    }

    emit_status(
        "info",
        "rebuildStarted",
        declarations.size() == 1
            ? "Building IV package"
            : "Building " + std::to_string(declarations.size()) + " IV packages",
        declarations.size() == 1 ? declarations.front().package_root : std::filesystem::path{});

    auto const rebuild_started_at = std::chrono::steady_clock::now();
    auto results = reload_packages(declarations);
    auto const rebuild_duration = format_rebuild_duration(
        std::chrono::steady_clock::now() - rebuild_started_at);
    if (results.packages.empty() && results.loaded.empty()
        && results.node_types.empty() && results.failed.empty()) {
        return;
    }

    // Publish the terminal build state before emitting the RPC status event.
    // The client refreshes its package card in response to a failure event;
    // publishing it afterwards leaves a timing window where that refresh can
    // still observe the prior "building" state forever (failed results do not
    // produce a definitions-changed event).
    {
        std::scoped_lock lock(mutex);
        for (auto const& package : results.packages) {
            build_status_by_package_id.insert_or_assign(
                package.package_id,
                IvPackageBuildStatus{
                    .package_id = package.package_id,
                    .state = IvPackageBuildState::built,
                });
        }
        for (auto const& failure : results.failed) {
            build_status_by_package_id.insert_or_assign(
                failure.package_id,
                IvPackageBuildStatus{
                    .package_id = failure.package_id,
                    .state = IvPackageBuildState::failed,
                    .message = failure.message,
                });
        }
    }

    if (!results.failed.empty()) {
        auto const &failure = results.failed.front();
        emit_status(
            "error",
            "rebuildFailed",
            "Module build failed after " + rebuild_duration + ": " + failure.message,
            failure.package_root);
    } else {
        auto const& package_root = results.packages.size() == 1
            ? results.packages.front().package_root
            : std::filesystem::path{};
        emit_status(
            "info",
            "rebuildFinished",
            "IV package build ready to apply in " + rebuild_duration,
            package_root);
    }

    {
        std::scoped_lock lock(mutex);
        // A package result is an atomic candidate set: it can add, update, or
        // remove any number of IV modules. Do not leave an older successful
        // result for the same package in the queue, otherwise its old module
        // IDs would be unioned with the new candidate set at apply time.
        std::unordered_set<std::string> replaced_package_ids;
        for (auto const& package : results.packages) {
            replaced_package_ids.insert(package.package_id);
        }
        for (auto const& failure : results.failed) {
            replaced_package_ids.insert(failure.package_id);
        }
        if (!replaced_package_ids.empty()) {
            std::erase_if(pending_results.packages, [&](auto const& package) {
                return replaced_package_ids.contains(package.package_id);
            });
            std::erase_if(pending_results.loaded, [&](auto const& loaded) {
                return replaced_package_ids.contains(loaded.package_id);
            });
            std::erase_if(pending_results.node_types, [&](auto const& node_type) {
                return replaced_package_ids.contains(node_type.package_id);
            });
            std::erase_if(pending_results.failed, [&](auto const& failure) {
                return replaced_package_ids.contains(failure.package_id);
            });
        }
        pending_results.packages.insert(
            pending_results.packages.end(),
            std::make_move_iterator(results.packages.begin()),
            std::make_move_iterator(results.packages.end()));
        pending_results.loaded.insert(
            pending_results.loaded.end(),
            std::make_move_iterator(results.loaded.begin()),
            std::make_move_iterator(results.loaded.end()));
        pending_results.node_types.insert(
            pending_results.node_types.end(),
            std::make_move_iterator(results.node_types.begin()),
            std::make_move_iterator(results.node_types.end()));
        pending_results.failed.insert(
            pending_results.failed.end(),
            std::make_move_iterator(results.failed.begin()),
            std::make_move_iterator(results.failed.end()));
    }
}

void IvModuleReload::reload_changed_packages()
{
    {
        std::scoped_lock lock(mutex);
        if (!watcher.has_changes()) {
            return;
        }
        for (auto const &entry : package_declarations_by_id) {
            dirty_package_ids.insert(entry.first);
            build_status_by_package_id.insert_or_assign(
                entry.first,
                IvPackageBuildStatus{
                    .package_id = entry.first,
                    .state = IvPackageBuildState::queued,
                });
        }
    }
    compile_dirty_packages();
}

bool IvModuleReload::has_pending_results() const
{
    std::scoped_lock lock(mutex);
    return !pending_results.packages.empty() || !pending_results.loaded.empty()
        || !pending_results.node_types.empty() || !pending_results.failed.empty();
}

bool IvModuleReload::apply_pending_results()
{
    IvModuleReloadResults results;
    {
        std::scoped_lock lock(mutex);
        if (pending_results.packages.empty() && pending_results.loaded.empty()
            && pending_results.node_types.empty() && pending_results.failed.empty()) {
            return false;
        }
        results = std::move(pending_results);
        pending_results = {};
    }
    results = coalesce_results_by_package(std::move(results));
    if (results.packages.empty() && results.loaded.empty()
        && results.node_types.empty() && results.failed.empty()) {
        return false;
    }

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_reload_results_event,
        results);
    return true;
}

} // namespace iv
