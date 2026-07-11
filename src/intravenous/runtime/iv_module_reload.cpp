#include <intravenous/runtime/iv_module_reload.h>

#include <intravenous/devices/audio_device.h>
#include <intravenous/graph/builder.h>
#include <intravenous/juce/vst_runtime.h>
#include <intravenous/runtime/iv_module_reload_events.h>

#include <exception>
#include <iterator>

namespace iv {
namespace {
ModuleExecutorTarget module_executor_target(RenderConfig const &config)
{
    return ModuleExecutorTarget{
        .sample_rate = config.sample_rate,
        .num_channels = config.num_channels,
        .max_block_frames = config.max_block_frames,
    };
}

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

IvModuleReloadResults coalesce_results_by_definition(IvModuleReloadResults results)
{
    std::unordered_map<std::string, IvModuleReloadedDefinition> loaded_by_definition;
    std::unordered_map<std::string, IvModuleReloadFailure> failed_by_definition;
    std::vector<std::string> order;
    order.reserve(results.loaded.size() + results.failed.size());

    auto const remember_order = [&](std::string const &definition_id) {
        if (std::ranges::find(order, definition_id) == order.end()) {
            order.push_back(definition_id);
        }
    };

    for (auto &loaded : results.loaded) {
        remember_order(loaded.definition_id);
        failed_by_definition.erase(loaded.definition_id);
        loaded_by_definition[loaded.definition_id] = std::move(loaded);
    }
    for (auto &failed : results.failed) {
        remember_order(failed.definition_id);
        loaded_by_definition.erase(failed.definition_id);
        failed_by_definition[failed.definition_id] = std::move(failed);
    }

    IvModuleReloadResults coalesced;
    coalesced.loaded.reserve(loaded_by_definition.size());
    coalesced.failed.reserve(failed_by_definition.size());
    for (auto const &definition_id : order) {
        if (auto loaded = loaded_by_definition.find(definition_id);
            loaded != loaded_by_definition.end()) {
            coalesced.loaded.push_back(std::move(loaded->second));
            continue;
        }
        if (auto failed = failed_by_definition.find(definition_id);
            failed != failed_by_definition.end()) {
            coalesced.failed.push_back(std::move(failed->second));
        }
    }
    return coalesced;
}
} // namespace

IvModuleReload::IvModuleReload(StartupConfigState startup_config_)
    : startup_config(std::move(startup_config_)),
      watcher(make_dependency_watcher())
{
    device_sample_period_ = sample_period(RenderConfig{});
}

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

    RenderConfig const render_config{};

    for (auto const &declaration : declarations) {
        try {
            auto loaded_definition = loader.load_root_definition(
                declaration.module_root,
                module_executor_target(render_config),
                &device_sample_period_);

            IvModuleReloadedDefinition loaded{
                .definition_id = declaration.definition_id,
                .module_root = declaration.module_root,
                .module_id = loaded_definition.module_id,
                .introspection = loaded_definition.introspection,
                .dependencies = loaded_definition.dependencies,
                .module_refs = loaded_definition.module_refs,
                .canonical_builder = *loaded_definition.canonical_builder,
            };
            {
                std::scoped_lock lock(mutex);
                dependencies_by_definition_id[declaration.definition_id] = loaded.dependencies;
                refresh_watched_dependencies_locked();
            }
            results.loaded.push_back(std::move(loaded));
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
        for (auto const &definition_id : diff.deleted_definition_ids) {
            declarations_by_id.erase(definition_id);
            dependencies_by_definition_id.erase(definition_id);
            dirty_definition_ids.erase(definition_id);
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
        for (auto const &definition_id : dirty_definition_ids) {
            if (auto const it = declarations_by_id.find(definition_id);
                it != declarations_by_id.end()) {
                declarations.push_back(it->second);
            }
        }
        dirty_definition_ids.clear();
    }

    if (declarations.empty()) {
        return;
    }

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_reload_status_event,
        IvModuleReloadStatus{
            .code = "rebuildStarted",
            .message = declarations.size() == 1
                ? "Building module definition"
                : "Building " + std::to_string(declarations.size()) + " module definitions",
            .module_root = declarations.size() == 1 ? declarations.front().module_root : std::filesystem::path{},
        });

    auto results = reload_declarations(declarations);
    if (results.loaded.empty() && results.failed.empty()) {
        return;
    }

    if (!results.failed.empty()) {
        auto const &failure = results.failed.front();
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_reload_status_event,
            IvModuleReloadStatus{
                .level = "error",
                .code = "rebuildFailed",
                .message = failure.message,
                .module_root = failure.module_root,
            });
    } else {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_reload_status_event,
            IvModuleReloadStatus{
                .code = "rebuildFinished",
                .message = "Module build ready to apply",
                .module_root = results.loaded.size() == 1
                    ? results.loaded.front().module_root
                    : std::filesystem::path{},
            });
    }

    {
        std::scoped_lock lock(mutex);
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
    return !pending_results.loaded.empty() || !pending_results.failed.empty();
}

void IvModuleReload::apply_pending_results()
{
    IvModuleReloadResults results;
    {
        std::scoped_lock lock(mutex);
        if (pending_results.loaded.empty() && pending_results.failed.empty()) {
            return;
        }
        results = std::move(pending_results);
        pending_results = {};
    }
    results = coalesce_results_by_definition(std::move(results));
    if (results.loaded.empty() && results.failed.empty()) {
        return;
    }

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_reload_results_event,
        results);
}
} // namespace iv
