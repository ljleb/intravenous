#include <intravenous/runtime/iv_module_reload.h>

#include <intravenous/runtime/task_runner_events.h>

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

IvModuleReloadResults coalesce_results_by_source(IvModuleReloadResults results)
{
    std::unordered_map<std::string, IvModuleReloadedSource> sources_by_id;
    std::unordered_map<std::string, std::vector<IvModuleReloadedDefinition>>
        loaded_by_source_id;
    std::unordered_map<std::string, IvModuleReloadFailure> failed_by_source_id;
    std::vector<std::string> order;
    order.reserve(results.sources.size() + results.failed.size());

    auto const remember_order = [&](std::string const& source_id) {
        if (std::ranges::find(order, source_id) == order.end()) {
            order.push_back(source_id);
        }
    };

    for (auto& source : results.sources) {
        auto const source_id = source.definition_id;
        remember_order(source_id);
        failed_by_source_id.erase(source_id);
        loaded_by_source_id[source_id].clear();
        sources_by_id[source_id] = std::move(source);
    }
    for (auto& loaded : results.loaded) {
        if (!sources_by_id.contains(loaded.source_id)) continue;
        loaded_by_source_id[loaded.source_id].push_back(std::move(loaded));
    }
    for (auto& failed : results.failed) {
        auto const source_id = failed.definition_id;
        remember_order(source_id);
        sources_by_id.erase(source_id);
        loaded_by_source_id.erase(source_id);
        failed_by_source_id[source_id] = std::move(failed);
    }

    IvModuleReloadResults coalesced;
    coalesced.sources.reserve(sources_by_id.size());
    coalesced.loaded.reserve(results.loaded.size());
    coalesced.failed.reserve(failed_by_source_id.size());
    for (auto const& source_id : order) {
        if (auto failed = failed_by_source_id.find(source_id);
            failed != failed_by_source_id.end()) {
            coalesced.failed.push_back(std::move(failed->second));
            continue;
        }
        auto source = sources_by_id.find(source_id);
        if (source == sources_by_id.end()) continue;
        coalesced.sources.push_back(std::move(source->second));
        if (auto loaded = loaded_by_source_id.find(source_id);
            loaded != loaded_by_source_id.end()) {
            coalesced.loaded.insert(
                coalesced.loaded.end(),
                std::make_move_iterator(loaded->second.begin()),
                std::make_move_iterator(loaded->second.end()));
        }
    }
    return coalesced;
}
} // namespace

IvModuleReload::IvModuleReload(StartupConfigState startup_config_)
    : startup_config(std::move(startup_config_)),
      watcher(make_dependency_watcher())
{}

void IvModuleReload::set_toolchain_config(ModuleLoaderToolchainConfig toolchain)
{
    std::scoped_lock lock(mutex);
    startup_config.toolchain = std::move(toolchain);
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
    std::filesystem::path module_root)
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectStatusNotification{
            .level = std::move(level),
            .code = std::move(code),
            .message = std::move(message),
            .module_root = std::move(module_root),
        }));
}

void IvModuleReload::refresh_watched_dependencies_locked()
{
    std::vector<ModuleDependency> dependencies;
    for (auto const &entry : dependencies_by_definition_id) {
        dependencies.insert(
            dependencies.end(),
            entry.second.begin(),
            entry.second.end());
    }
    watcher.update(std::move(dependencies));
}

IvModuleReloadResults IvModuleReload::reload_declarations(
    std::vector<IvModuleDefinitionDeclaration> const &declarations)
{
#if IV_ENABLE_JUCE_VST
    warmup_juce_vst_scan_cache();
#endif

    IvModuleReloadResults results;
    ModuleLoader loader(
        startup_config.discovery_start,
        startup_config.search_roots,
        startup_config.toolchain);

    for (auto const &declaration : declarations) {
        try {
            auto loaded_source = loader.load_source(declaration.module_root);
            auto dependencies = std::move(loaded_source.dependencies);
            {
                std::scoped_lock lock(mutex);
                dependencies_by_definition_id[declaration.definition_id] = dependencies;
                refresh_watched_dependencies_locked();
            }
            results.sources.push_back({
                .definition_id = declaration.definition_id,
                .module_root = declaration.module_root,
                .dependencies = dependencies,
            });
            for (auto& loaded_definition : loaded_source.definitions) {
                results.loaded.push_back(IvModuleReloadedDefinition{
                    .source_id = declaration.definition_id,
                    .definition_id = loaded_definition.module_id,
                    .module_root = declaration.module_root,
                    .module_id = std::move(loaded_definition.module_id),
                    .introspection = std::move(loaded_definition.introspection),
                    .dependencies = std::move(loaded_definition.dependencies),
                    .module_refs = std::move(loaded_definition.module_refs),
                    .root = std::move(loaded_definition.root),
                });
            }
        } catch (...) {
            results.failed.push_back(IvModuleReloadFailure{
                .definition_id = declaration.definition_id,
                .module_root = declaration.module_root,
                .message = describe_exception(std::current_exception()),
            });
        }
    }

    return results;
}

void IvModuleReload::handle_definition_declarations_changed(
    IvModuleDefinitionDeclarationsChanged const &diff)
{
    {
        std::scoped_lock lock(mutex);
        for (auto const &declaration : diff.created) {
            declarations_by_id[declaration.definition_id] = declaration;
            dirty_definition_ids.insert(declaration.definition_id);
        }
        for (auto const &declaration : diff.updated) {
            declarations_by_id[declaration.definition_id] = declaration;
            dirty_definition_ids.insert(declaration.definition_id);
        }
        for (auto const &source_id : diff.deleted_definition_ids) {
            declarations_by_id.erase(source_id);
            dependencies_by_definition_id.erase(source_id);
            dirty_definition_ids.erase(source_id);
        }
        refresh_watched_dependencies_locked();
    }
}

bool IvModuleReload::has_dirty_definitions() const
{
    std::scoped_lock lock(mutex);
    return !dirty_definition_ids.empty();
}

void IvModuleReload::compile_dirty_definitions()
{
    std::vector<IvModuleDefinitionDeclaration> declarations;
    {
        std::scoped_lock lock(mutex);
        if (dirty_definition_ids.empty()) {
            return;
        }
        declarations.reserve(dirty_definition_ids.size());
        for (auto const &source_id : dirty_definition_ids) {
            if (auto const it = declarations_by_id.find(source_id);
                it != declarations_by_id.end()) {
                declarations.push_back(it->second);
            }
        }
        dirty_definition_ids.clear();
    }

    if (declarations.empty()) {
        return;
    }

    emit_status(
        "info",
        "rebuildStarted",
        declarations.size() == 1
            ? "Building IV source"
            : "Building " + std::to_string(declarations.size()) + " IV sources",
        declarations.size() == 1 ? declarations.front().module_root : std::filesystem::path{});

    auto const rebuild_started_at = std::chrono::steady_clock::now();
    auto results = reload_declarations(declarations);
    auto const rebuild_duration = format_rebuild_duration(
        std::chrono::steady_clock::now() - rebuild_started_at);
    if (results.sources.empty() && results.loaded.empty() && results.failed.empty()) {
        return;
    }

    if (!results.failed.empty()) {
        auto const &failure = results.failed.front();
        emit_status(
            "error",
            "rebuildFailed",
            "Module build failed after " + rebuild_duration + ": " + failure.message,
            failure.module_root);
    } else {
        emit_status(
            "info",
            "rebuildFinished",
            "Module build ready to apply in " + rebuild_duration,
            results.loaded.size() == 1
                ? results.loaded.front().module_root
                : std::filesystem::path{});
    }

    {
        std::scoped_lock lock(mutex);
        // A source result is an atomic candidate set: it can add, update, or
        // remove any number of IV modules.  Do not leave an older successful
        // result for the same source in the queue, otherwise its old module
        // IDs would be unioned with the new candidate set at apply time.
        std::unordered_set<std::string> replaced_source_ids;
        for (auto const& source : results.sources) {
            replaced_source_ids.insert(source.definition_id);
        }
        for (auto const& failure : results.failed) {
            replaced_source_ids.insert(failure.definition_id);
        }
        if (!replaced_source_ids.empty()) {
            std::erase_if(pending_results.sources, [&](auto const& source) {
                return replaced_source_ids.contains(source.definition_id);
            });
            std::erase_if(pending_results.loaded, [&](auto const& loaded) {
                return replaced_source_ids.contains(loaded.source_id);
            });
            std::erase_if(pending_results.failed, [&](auto const& failure) {
                return replaced_source_ids.contains(failure.definition_id);
            });
        }
        pending_results.sources.insert(
            pending_results.sources.end(),
            std::make_move_iterator(results.sources.begin()),
            std::make_move_iterator(results.sources.end()));
        pending_results.loaded.insert(
            pending_results.loaded.end(),
            std::make_move_iterator(results.loaded.begin()),
            std::make_move_iterator(results.loaded.end()));
        pending_results.failed.insert(
            pending_results.failed.end(),
            std::make_move_iterator(results.failed.begin()),
            std::make_move_iterator(results.failed.end()));
    }
}

bool IvModuleReload::has_changes()
{
    std::scoped_lock lock(mutex);
    return watcher.has_changes();
}

void IvModuleReload::reload_changed_definitions()
{
    {
        std::scoped_lock lock(mutex);
        if (!watcher.has_changes()) {
            return;
        }
        for (auto const &entry : declarations_by_id) {
            dirty_definition_ids.insert(entry.first);
        }
    }
    compile_dirty_definitions();
}

bool IvModuleReload::has_pending_results() const
{
    std::scoped_lock lock(mutex);
    return !pending_results.sources.empty() || !pending_results.loaded.empty()
        || !pending_results.failed.empty();
}

void IvModuleReload::apply_pending_results()
{
    IvModuleReloadResults results;
    {
        std::scoped_lock lock(mutex);
        if (pending_results.sources.empty() && pending_results.loaded.empty()
            && pending_results.failed.empty()) {
            return;
        }
        results = std::move(pending_results);
        pending_results = {};
    }
    results = coalesce_results_by_source(std::move(results));
    if (results.sources.empty() && results.loaded.empty() && results.failed.empty()) {
        return;
    }

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_reload_results_event,
        results);
}

void IvModuleReload::handle_task_runner_before_pass(TasksRunnerBeforePass const &)
{
    if (has_pending_results()) {
        apply_pending_results();
    }
}
} // namespace iv
