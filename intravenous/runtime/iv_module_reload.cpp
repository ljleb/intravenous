#include "runtime/iv_module_reload.h"

#include "graph/builder.h"
#include "juce/vst_runtime.h"
#include "runtime/iv_module_reload_events.h"

#include <exception>

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
} // namespace

IvModuleReload::IvModuleReload(StartupConfigState startup_config_)
    : startup_config(std::move(startup_config_)),
      watcher(make_dependency_watcher())
{
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

void IvModuleReload::reload_declarations(
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
    Sample device_sample_period = sample_period(render_config);

    for (auto const &declaration : declarations) {
        try {
            auto loaded_definition = loader.load_root_definition(
                declaration.module_root,
                module_executor_target(render_config),
                &device_sample_period);

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

    if (!results.loaded.empty() || !results.failed.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_reload_results_event,
            results);
    }
}

void IvModuleReload::handle_definition_declarations_changed(
    IvModuleDefinitionDeclarationsChanged const &diff)
{
    std::vector<IvModuleDefinitionDeclaration> to_reload;
    {
        std::scoped_lock lock(mutex);
        for (auto const &declaration : diff.created) {
            declarations_by_id[declaration.definition_id] = declaration;
            to_reload.push_back(declaration);
        }
        for (auto const &declaration : diff.updated) {
            declarations_by_id[declaration.definition_id] = declaration;
            to_reload.push_back(declaration);
        }
        for (auto const &definition_id : diff.deleted_definition_ids) {
            declarations_by_id.erase(definition_id);
            dependencies_by_definition_id.erase(definition_id);
        }
        refresh_watched_dependencies_locked();
    }

    if (!to_reload.empty()) {
        reload_declarations(to_reload);
    }
}

bool IvModuleReload::has_changes()
{
    std::scoped_lock lock(mutex);
    return watcher.has_changes();
}

void IvModuleReload::reload_changed_definitions()
{
    std::vector<IvModuleDefinitionDeclaration> declarations;
    {
        std::scoped_lock lock(mutex);
        if (!watcher.has_changes()) {
            return;
        }
        declarations.reserve(declarations_by_id.size());
        for (auto const &entry : declarations_by_id) {
            declarations.push_back(entry.second);
        }
    }
    if (!declarations.empty()) {
        reload_declarations(declarations);
    }
}
} // namespace iv
