#include <intravenous/runtime/package_reload.h>

#include <intravenous/juce/vst_runtime.h>
#include <intravenous/runtime/package_reload_events.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <algorithm>
#include <exception>
#include <chrono>
#include <iterator>
#include <ranges>
#include <system_error>

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

std::string dependency_root_key(std::filesystem::path const& path)
{
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error) {
        error.clear();
        canonical = std::filesystem::absolute(path, error);
    }
    if (error) canonical = path;
    return canonical.lexically_normal().generic_string();
}

PackageReloadResults coalesce_results_by_package(PackageReloadResults results)
{
    std::unordered_map<std::string, PackageReloadedPackage> packages_by_id;
    std::unordered_map<std::string, std::vector<PackageReloadedModuleDefinition>>
        module_definitions_by_package_id;
    std::unordered_map<std::string, std::vector<PackageReloadedLeafDefinition>>
        leaf_definitions_by_package_id;
    std::unordered_map<std::string, PackageReloadFailure> failed_by_package_id;
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
        module_definitions_by_package_id[package_id].clear();
        leaf_definitions_by_package_id[package_id].clear();
        packages_by_id[package_id] = std::move(package);
    }
    for (auto& definition : results.module_definitions) {
        if (!packages_by_id.contains(definition.package_id)) continue;
        module_definitions_by_package_id[definition.package_id].push_back(std::move(definition));
    }
    for (auto& definition : results.leaf_definitions) {
        if (!packages_by_id.contains(definition.package_id)) continue;
        leaf_definitions_by_package_id[definition.package_id].push_back(std::move(definition));
    }
    for (auto& failed : results.failed) {
        auto const package_id = failed.package_id;
        remember_order(package_id);
        packages_by_id.erase(package_id);
        module_definitions_by_package_id.erase(package_id);
        leaf_definitions_by_package_id.erase(package_id);
        failed_by_package_id[package_id] = std::move(failed);
    }

    PackageReloadResults coalesced;
    coalesced.packages.reserve(packages_by_id.size());
    coalesced.module_definitions.reserve(results.module_definitions.size());
    coalesced.leaf_definitions.reserve(results.leaf_definitions.size());
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
        if (auto definitions = module_definitions_by_package_id.find(package_id);
            definitions != module_definitions_by_package_id.end()) {
            coalesced.module_definitions.insert(
                coalesced.module_definitions.end(),
                std::make_move_iterator(definitions->second.begin()),
                std::make_move_iterator(definitions->second.end()));
        }
        if (auto definitions = leaf_definitions_by_package_id.find(package_id);
            definitions != leaf_definitions_by_package_id.end()) {
            coalesced.leaf_definitions.insert(
                coalesced.leaf_definitions.end(),
                std::make_move_iterator(definitions->second.begin()),
                std::make_move_iterator(definitions->second.end()));
        }
    }
    return coalesced;
}
} // namespace

PackageReload::PackageReload(
    StartupConfigState startup_config_,
    ModuleLoader::OptimizationLevel loader_optimization_level)
    : startup_config(std::move(startup_config_)),
      loader_optimization_level_(loader_optimization_level),
      watcher(make_dependency_watcher())
{}

ModuleLoader& PackageReload::ensure_loader()
{
    std::scoped_lock lock(mutex);
    if (!loader_) {
        loader_ = std::make_unique<ModuleLoader>(
            startup_config.discovery_start,
            startup_config.search_roots,
            startup_config.toolchain,
            ModuleLoader::LogSink{},
            loader_optimization_level_);
    }
    return *loader_;
}

void PackageReload::set_toolchain_config(ModuleLoaderToolchainConfig toolchain)
{
    std::scoped_lock lock(mutex);
    startup_config.toolchain = toolchain;
    if (loader_) {
        loader_->set_toolchain_config(std::move(toolchain));
    }
}

ModuleLoaderToolchainConfig PackageReload::toolchain_config() const
{
    std::scoped_lock lock(mutex);
    return startup_config.toolchain;
}

void PackageReload::handle_project_override_settings(
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
    assign_path(request.iv_package_pch, &ModuleLoaderToolchainConfig::iv_package_pch);
    if (!touched) {
        return;
    }

    set_toolchain_config(std::move(toolchain));
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void PackageReload::handle_project_persistence_collect_state(
    ProjectPersistenceBuilder &builder) const
{
    builder.add_project_toolchain_config(toolchain_config());
}

void PackageReload::emit_status(
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

void PackageReload::refresh_watched_dependencies_locked()
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

PackageReloadResults PackageReload::reload_packages(
    std::vector<IvPackageDeclaration> const &declarations)
{
#if IV_ENABLE_JUCE_VST
    warmup_juce_vst_scan_cache();
#endif

    PackageReloadResults results;
    auto& loader = ensure_loader();
    std::vector<std::filesystem::path> package_paths;
    package_paths.reserve(declarations.size());
    for (auto const& declaration : declarations) {
        package_paths.push_back(declaration.package_root);
    }
    auto batch = loader.load_packages(package_paths);

    for (std::size_t index = 0; index < declarations.size(); ++index) {
        auto const& declaration = declarations[index];
        try {
            if (index >= batch.size()) {
                throw std::logic_error("IV package reload batch omitted a requested package");
            }
            auto& result = batch[index];
            if (!result) {
                throw std::runtime_error(result.error.empty()
                    ? "IV package reload failed without an error"
                    : result.error);
            }
            auto loaded_package = std::move(*result.package);
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
                results.module_definitions.push_back(PackageReloadedModuleDefinition{
                    .package_id = declaration.package_id,
                    .definition_id = loaded_definition.module_id,
                    .package_root = declaration.package_root,
                    .module_id = std::move(loaded_definition.module_id),
                    .provider = NodeDefinitionProvider{
                        .module_build = loaded_definition.provider.module_build,
                        .signature = loaded_definition.provider.signature,
                    },
                    .introspection = std::move(loaded_definition.introspection),
                    .dependencies = std::move(loaded_definition.dependencies),
                    .module_refs = std::move(loaded_definition.module_refs),
                    .root = std::move(loaded_definition.root),
                    .configured_graph = std::move(loaded_definition.configured_graph),
                });
            }
            for (auto& node_type : loaded_package.node_types) {
                results.leaf_definitions.push_back(PackageReloadedLeafDefinition{
                    .package_id = declaration.package_id,
                    .definition_id = std::move(node_type.node_type_id),
                    .package_root = declaration.package_root,
                    .provider = NodeDefinitionProvider{
                        .leaf_build = node_type.provider.node_build,
                        .signature = node_type.provider.signature,
                    },
                    .compiler_record = node_type.compiler_record,
                    .module_refs = std::move(node_type.module_refs),
                });
            }
        } catch (...) {
            results.failed.push_back(PackageReloadFailure{
                .package_id = declaration.package_id,
                .package_root = declaration.package_root,
                .message = describe_exception(std::current_exception()),
            });
        }
    }

    return results;
}

void PackageReload::publish_build_statuses() const
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_package_build_statuses_changed_event,
        package_build_statuses());
}

void PackageReload::handle_package_declarations_changed(
    IvPackageDeclarationsChanged const &diff)
{
    std::vector<std::filesystem::path> removed_package_roots;
    ModuleLoader* loader = nullptr;
    {
        std::scoped_lock lock(mutex);
        for (auto const &declaration : diff.created) {
            package_declarations_by_id[declaration.package_id] = declaration;
            dirty_package_ids.insert(declaration.package_id);
            build_status_by_package_id.insert_or_assign(
                declaration.package_id,
                PackageBuildStatus{
                    .package_id = declaration.package_id,
                    .state = PackageBuildState::queued,
                });
        }
        for (auto const &declaration : diff.updated) {
            package_declarations_by_id[declaration.package_id] = declaration;
            dirty_package_ids.insert(declaration.package_id);
            build_status_by_package_id.insert_or_assign(
                declaration.package_id,
                PackageBuildStatus{
                    .package_id = declaration.package_id,
                    .state = PackageBuildState::queued,
                });
        }
        for (auto const &package_id : diff.deleted_package_ids) {
            if (auto const declaration = package_declarations_by_id.find(package_id);
                declaration != package_declarations_by_id.end()) {
                removed_package_roots.push_back(declaration->second.package_root);
            }
            package_declarations_by_id.erase(package_id);
            dependencies_by_package_id.erase(package_id);
            dirty_package_ids.erase(package_id);
            build_status_by_package_id.erase(package_id);
        }
        refresh_watched_dependencies_locked();
        loader = loader_.get();
    }
    // Do not let a removed provider remain available just because an older
    // graph still owns its LLVM revision. Old graphs retain that revision via
    // ModuleRef, but a newly compiled caller must report the now-missing ID.
    if (loader) {
        for (auto const& package_root : removed_package_roots) {
            loader->remove_package(package_root);
        }
    }
    publish_build_statuses();
}

bool PackageReload::has_dirty_packages() const
{
    std::scoped_lock lock(mutex);
    return !dirty_package_ids.empty();
}

std::vector<PackageBuildStatus> PackageReload::package_build_statuses() const
{
    std::vector<PackageBuildStatus> statuses;
    {
        std::scoped_lock lock(mutex);
        statuses.reserve(build_status_by_package_id.size());
        for (auto const& [_, status] : build_status_by_package_id) {
            statuses.push_back(status);
        }
    }
    std::ranges::sort(statuses, {}, &PackageBuildStatus::package_id);
    return statuses;
}

void PackageReload::compile_dirty_packages()
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
                    PackageBuildStatus{
                        .package_id = package_id,
                        .state = PackageBuildState::building,
                    });
            }
        }
        dirty_package_ids.clear();
    }

    if (declarations.empty()) {
        return;
    }

    publish_build_statuses();
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
    if (results.packages.empty() && results.module_definitions.empty()
        && results.leaf_definitions.empty() && results.failed.empty()) {
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
                PackageBuildStatus{
                    .package_id = package.package_id,
                    .state = PackageBuildState::built,
                });
        }
        for (auto const& failure : results.failed) {
            build_status_by_package_id.insert_or_assign(
                failure.package_id,
                PackageBuildStatus{
                    .package_id = failure.package_id,
                    .state = PackageBuildState::failed,
                    .message = failure.message,
                });
        }
    }
    publish_build_statuses();

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
        // remove any number of node definitions. Do not leave an older successful
        // result for the same package in the queue, otherwise its old definition
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
            std::erase_if(pending_results.module_definitions, [&](auto const& loaded) {
                return replaced_package_ids.contains(loaded.package_id);
            });
            std::erase_if(pending_results.leaf_definitions, [&](auto const& node_type) {
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
        pending_results.module_definitions.insert(
            pending_results.module_definitions.end(),
            std::make_move_iterator(results.module_definitions.begin()),
            std::make_move_iterator(results.module_definitions.end()));
        pending_results.leaf_definitions.insert(
            pending_results.leaf_definitions.end(),
            std::make_move_iterator(results.leaf_definitions.begin()),
            std::make_move_iterator(results.leaf_definitions.end()));
        pending_results.failed.insert(
            pending_results.failed.end(),
            std::make_move_iterator(results.failed.begin()),
            std::make_move_iterator(results.failed.end()));
    }
}

void PackageReload::reload_changed_packages()
{
    {
        std::scoped_lock lock(mutex);
        auto const changed_dependencies = watcher.changed_dependencies();
        if (changed_dependencies.empty()) {
            return;
        }

        std::unordered_set<std::string> changed_roots;
        for (auto const& dependency : changed_dependencies) {
            changed_roots.insert(dependency_root_key(dependency.module_dir));
        }
        for (auto const& [package_id, dependencies] : dependencies_by_package_id) {
            auto const depends_on_changed_package = std::ranges::any_of(
                dependencies, [&](ModuleDependency const& dependency) {
                    return changed_roots.contains(dependency_root_key(dependency.module_dir));
                });
            if (!depends_on_changed_package) continue;
            dirty_package_ids.insert(package_id);
            build_status_by_package_id.insert_or_assign(
                package_id,
                PackageBuildStatus{
                    .package_id = package_id,
                    .state = PackageBuildState::queued,
                });
        }
    }
    compile_dirty_packages();
}

bool PackageReload::has_pending_results() const
{
    std::scoped_lock lock(mutex);
    return !pending_results.packages.empty() || !pending_results.module_definitions.empty()
        || !pending_results.leaf_definitions.empty() || !pending_results.failed.empty();
}

bool PackageReload::apply_pending_results()
{
    PackageReloadResults results;
    {
        std::scoped_lock lock(mutex);
        if (pending_results.packages.empty() && pending_results.module_definitions.empty()
            && pending_results.leaf_definitions.empty() && pending_results.failed.empty()) {
            return false;
        }
        results = std::move(pending_results);
        pending_results = {};
    }
    results = coalesce_results_by_package(std::move(results));
    if (results.packages.empty() && results.module_definitions.empty()
        && results.leaf_definitions.empty() && results.failed.empty()) {
        return false;
    }

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_package_reload_results_event,
        results);
    return true;
}

} // namespace iv
