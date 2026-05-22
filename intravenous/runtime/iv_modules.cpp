#include "runtime/iv_modules.h"

#include "devices/miniaudio_device.h"
#include "juce/vst_runtime.h"
#include "module/loader.h"
#include "module/search_paths.h"
#include "module/watcher.h"
#include "node/executor.h"
#include "orchestrator/orchestrator_builder.h"
#include "runtime/handlers.h"
#include "runtime/iv_modules_events.h"
#include "runtime/reload_worker.h"

#include <stdexcept>
#include <utility>

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

DeviceOrchestrator make_audio_device_provider(LogicalAudioDevice audio_device)
{
    std::vector<OutputDeviceMixer> mixers;
    mixers.emplace_back(
        std::move(audio_device),
        [](OrchestratorBuilder &builder, OutputDeviceMixer &&mixer) {
            builder.add_audio_mixer(0, std::move(mixer));
        });
    return DeviceOrchestrator(std::move(mixers));
}

ResourceContext make_resource_context(RenderConfig const &render_config)
{
    ResourceContext resources{};
#if IV_ENABLE_JUCE_VST
    static JuceVstRuntimeManager juce_vst_runtime_manager;
    static std::unique_ptr<JuceVstRuntimeSupport> juce_vst_runtime_support;
    juce_vst_runtime_support = std::make_unique<JuceVstRuntimeSupport>(
        juce_vst_runtime_manager,
        static_cast<double>(render_config.sample_rate));
    resources = juce_vst_runtime_support->resources();
#endif
    return resources;
}

NodeExecutor make_executor(
    RenderConfig const &render_config,
    DeviceOrchestrator &device_orchestrator,
    ModuleLoader::LoadedGraph loaded_graph)
{
    return NodeExecutor::create(
        std::move(loaded_graph.root),
        make_resource_context(render_config),
        std::move(device_orchestrator).to_builder(),
        std::move(loaded_graph.module_refs));
}

std::filesystem::path normalize_path(std::filesystem::path const &path)
{
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical.lexically_normal();
    }
    return std::filesystem::absolute(path).lexically_normal();
}

std::string format_exception_context(
    std::string_view context,
    std::exception_ptr exception)
{
    if (!exception) {
        return std::string(context);
    }

    try {
        std::rethrow_exception(exception);
    } catch (std::exception const &e) {
        return std::string(context) + ": " + e.what();
    } catch (...) {
        return std::string(context) + ": unknown exception";
    }
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

RuntimeIvModuleDefinition make_definition_snapshot(
    std::filesystem::path const &module_root,
    ModuleLoader::LoadedGraph const &loaded_graph)
{
    return RuntimeIvModuleDefinition{
        .definition_id = loaded_graph.module_id,
        .module_root = normalize_path(module_root),
        .module_id = loaded_graph.module_id,
        .introspection = loaded_graph.introspection,
        .dependencies = loaded_graph.dependencies,
    };
}
} // namespace

void RuntimeIvModules::emit_notification(
    RuntimeIvModulesNotification notification) const
{
    IV_INVOKE_LINKER_EVENT(iv_runtime_iv_modules_notification_event, notification);
}

void RuntimeIvModules::emit_message(std::string level, std::string message) const
{
    emit_notification(RuntimeIvModulesMessage{
        .level = std::move(level),
        .message = std::move(message),
        .module_root = config.has_value() ? config->module_root : std::filesystem::path{},
    });
}

void RuntimeIvModules::emit_status(
    std::string code,
    std::string level,
    std::string message,
    std::filesystem::path module_root,
    std::vector<std::string> created_definition_ids,
    std::vector<std::string> deleted_definition_ids) const
{
    emit_notification(RuntimeIvModulesStatus{
        .level = std::move(level),
        .code = std::move(code),
        .message = std::move(message),
        .module_root = std::move(module_root),
        .created_definition_ids = std::move(created_definition_ids),
        .deleted_definition_ids = std::move(deleted_definition_ids),
    });
}

void RuntimeIvModules::run_runtime()
{
    try {
        config = load_runtime_project_config(workspace_root);

        auto audio_device = audio_device_factory
            ? audio_device_factory()
            : make_miniaudio_device({});
        if (!audio_device) {
            throw std::runtime_error(
                "no production audio backend is currently configured");
        }

#if IV_ENABLE_JUCE_VST
        warmup_juce_vst_scan_cache();
#endif

        auto search_roots = parse_search_path_env();
        search_roots.insert(
            search_roots.end(),
            extra_search_roots.begin(),
            extra_search_roots.end());

        ModuleLoader loader(
            this->timeline,
            discovery_start,
            std::move(search_roots),
            config->toolchain,
            [this](std::string const &message) { emit_message("info", message); });
        auto watcher = make_dependency_watcher();

        RenderConfig const render_config = audio_device->config();
        Sample device_sample_period = sample_period(render_config);

        auto loaded_graph = loader.load_root(
            config->module_root,
            module_executor_target(render_config),
            &device_sample_period);
        watcher.update(loaded_graph.dependencies);

        auto definition = make_definition_snapshot(config->module_root, loaded_graph);
        {
            std::scoped_lock lock(mutex);
            root_definition = definition;
            initialized = true;
        }
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_modules_definitions_changed_event,
            RuntimeIvModuleDefinitionsChanged{
                .added = {definition},
            });
        initialized_cv.notify_all();

        DeviceOrchestrator output_devices(
            make_audio_device_provider(std::move(*audio_device)));
        auto executor_storage =
            make_executor(render_config, output_devices, std::move(loaded_graph));
        {
            std::scoped_lock lock(mutex);
            executor_state = &executor_storage;
            if (shutdown_requested) {
                executor_state->request_shutdown();
            }
        }

        ReloadWorker reload_worker(
            watcher,
            config->module_root,
            [&]() {
                auto reload = loader.load_root(
                    config->module_root,
                    module_executor_target(render_config),
                    &device_sample_period);

                auto updated_definition =
                    make_definition_snapshot(config->module_root, reload);
                {
                    std::scoped_lock lock(mutex);
                    root_definition = updated_definition;
                }

                IV_INVOKE_LINKER_EVENT(
                    iv_runtime_iv_modules_definitions_changed_event,
                    RuntimeIvModuleDefinitionsChanged{
                        .updated = {updated_definition},
                    });
                emit_status(
                    "rebuildFinished",
                    "info",
                    "rebuild complete " + config->module_root.string(),
                    config->module_root);
                return reload;
            },
            [this]() {
                emit_status(
                    "rebuildStarted",
                    "info",
                    "rebuilding " + config->module_root.string(),
                    config->module_root);
            },
            []() {},
            [this](std::exception_ptr exception) {
                emit_status(
                    "rebuildFailed",
                    "error",
                    describe_exception(exception),
                    config->module_root);
            });
        reload_worker.start();

        executor_storage.execute([&]() -> std::optional<ModuleLoader::LoadedGraph> {
            if (!reload_worker.has_pending_reload()) {
                if (reload_worker.has_pending_exception()) {
                    if (auto exception = reload_worker.take_exception()) {
                        emit_message(
                            "error",
                            format_exception_context(
                                "runtime project reload failed",
                                exception));
                    }
                }
                return std::nullopt;
            }

            if (auto exception = reload_worker.take_exception()) {
                emit_message(
                    "error",
                    format_exception_context(
                        "runtime project reload failed",
                        exception));
            }

            std::vector<ModuleDependency> dependencies;
            auto reload = reload_worker.take_completed_reload(&dependencies);
            if (reload) {
                watcher.update(std::move(dependencies));
            }
            return reload;
        });

        reload_worker.request_shutdown();
    } catch (...) {
        auto exception = std::current_exception();
        emit_status("startupFailed", "error", describe_exception(exception));
        {
            std::scoped_lock lock(mutex);
            pending_exception = exception;
            initialized = true;
        }
        initialized_cv.notify_all();
    }
}

RuntimeIvModules::RuntimeIvModules(
    Timeline &timeline_,
    std::filesystem::path workspace_root_,
    std::filesystem::path discovery_start_,
    std::vector<std::filesystem::path> extra_search_roots_,
    AudioDeviceFactory audio_device_factory_)
    : timeline(timeline_),
      workspace_root(std::filesystem::weakly_canonical(workspace_root_).lexically_normal()),
      discovery_start(std::move(discovery_start_)),
      extra_search_roots(std::move(extra_search_roots_)),
      audio_device_factory(std::move(audio_device_factory_))
{
}

RuntimeIvModules::~RuntimeIvModules()
{
    request_shutdown();
}

RuntimeIvModuleDefinition RuntimeIvModules::initialize()
{
    if (!runtime_thread.has_value()) {
        runtime_thread.emplace([this](std::stop_token) { run_runtime(); });
    }

    std::unique_lock lock(mutex);
    initialized_cv.wait(
        lock,
        [&] { return initialized || pending_exception != nullptr; });
    if (pending_exception) {
        std::rethrow_exception(pending_exception);
    }
    if (!root_definition.has_value()) {
        throw std::runtime_error(
            "runtime iv-modules failed to produce an initial root definition");
    }
    return *root_definition;
}

void RuntimeIvModules::request_shutdown()
{
    std::scoped_lock lock(mutex);
    shutdown_requested = true;
    if (executor_state) {
        executor_state->request_shutdown();
    }
}
} // namespace iv
