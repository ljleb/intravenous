#include "runtime/iv_module_definitions.h"

#include "devices/miniaudio_device.h"
#include "graph/builder.h"
#include "juce/vst_runtime.h"
#include "module/loader.h"
#include "module/search_paths.h"
#include "module/watcher.h"
#include "node/executor.h"
#include "orchestrator/orchestrator_builder.h"
#include "runtime/config.h"
#include "runtime/handlers.h"
#include "runtime/iv_module_definitions_events.h"
#include "runtime/reload_worker.h"

#include <stdexcept>
#include <utility>

namespace iv {
struct RuntimeIvModuleDefinitionState {
    std::vector<ModuleRef> module_refs{};
    RuntimeIvModuleDefinition snapshot{};
    GraphBuilder canonical_builder{};
};

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

std::unique_ptr<RuntimeIvModuleDefinitionState> make_definition_state(
    std::filesystem::path const &module_root,
    ModuleLoader::LoadedGraph loaded_graph)
{
    auto state =
        std::make_unique<RuntimeIvModuleDefinitionState>();
    state->module_refs = std::move(loaded_graph.module_refs);
    state->canonical_builder = *loaded_graph.canonical_builder;
    state->snapshot = RuntimeIvModuleDefinition{
        .definition_id = loaded_graph.module_id,
        .module_root = normalize_path(module_root),
        .module_id = loaded_graph.module_id,
        .introspection = loaded_graph.introspection,
        .dependencies = loaded_graph.dependencies,
        .canonical_builder = &state->canonical_builder,
    };
    return state;
}
} // namespace

void RuntimeIvModuleDefinitions::emit_notification(
    RuntimeIvModuleDefinitionsNotification notification) const
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_definitions_notification_event,
        notification);
}

void RuntimeIvModuleDefinitions::emit_message(
    std::string level,
    std::string message) const
{
    emit_notification(RuntimeIvModuleDefinitionsMessage{
        .level = std::move(level),
        .message = std::move(message),
        .module_root = config != nullptr ? config->module_root : std::filesystem::path{},
    });
}

void RuntimeIvModuleDefinitions::emit_status(
    std::string code,
    std::string level,
    std::string message,
    std::filesystem::path module_root,
    std::vector<std::string> created_definition_ids,
    std::vector<std::string> deleted_definition_ids) const
{
    emit_notification(RuntimeIvModuleDefinitionsStatus{
        .level = std::move(level),
        .code = std::move(code),
        .message = std::move(message),
        .module_root = std::move(module_root),
        .created_definition_ids = std::move(created_definition_ids),
        .deleted_definition_ids = std::move(deleted_definition_ids),
    });
}

void RuntimeIvModuleDefinitions::run_runtime()
{
    try {
        config = std::make_unique<RuntimeProjectConfig>(
            load_runtime_project_config(workspace_root));

#if IV_ENABLE_JUCE_VST
        warmup_juce_vst_scan_cache();
#endif

        auto search_roots = parse_search_path_env();
        search_roots.insert(
            search_roots.end(),
            extra_search_roots.begin(),
            extra_search_roots.end());

        ModuleLoader loader(
            discovery_start,
            std::move(search_roots),
            config->toolchain,
            [this](std::string const &message) { emit_message("info", message); });
        auto watcher = make_dependency_watcher();

        RenderConfig const render_config{};
        Sample device_sample_period = sample_period(render_config);

        auto loaded_graph = loader.load_root(
            config->module_root,
            module_executor_target(render_config),
            &device_sample_period);
        watcher.update(loaded_graph.dependencies);

        auto definition_state =
            make_definition_state(config->module_root, std::move(loaded_graph));
        auto definition_snapshot = definition_state->snapshot;
        {
            std::scoped_lock lock(mutex);
            root_definition = std::move(definition_state);
            initialized = true;
            root_definition->snapshot.canonical_builder =
                &root_definition->canonical_builder;
            definition_snapshot = root_definition->snapshot;
        }
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_definitions_changed_event,
            RuntimeIvModuleDefinitionsChanged{
                .created = {definition_snapshot},
            });
        initialized_cv.notify_all();
        return;
    } catch (...) {
        auto exception = std::current_exception();
        emit_status("startupFailed", "error", describe_exception(exception));
        {
            std::scoped_lock lock(mutex);
            executor_state = nullptr;
            pending_exception = exception;
            initialized = true;
        }
        initialized_cv.notify_all();
    }
}

RuntimeIvModuleDefinitions::RuntimeIvModuleDefinitions(
    std::filesystem::path workspace_root_,
    std::filesystem::path discovery_start_,
    std::vector<std::filesystem::path> extra_search_roots_,
    AudioDeviceFactory audio_device_factory_)
    : workspace_root(std::filesystem::weakly_canonical(workspace_root_).lexically_normal()),
      discovery_start(std::move(discovery_start_)),
      extra_search_roots(std::move(extra_search_roots_)),
      audio_device_factory(std::move(audio_device_factory_))
{
}

RuntimeIvModuleDefinitions::~RuntimeIvModuleDefinitions()
{
    request_shutdown();
}

RuntimeIvModuleDefinition RuntimeIvModuleDefinitions::initialize()
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
    if (root_definition == nullptr) {
        throw std::runtime_error(
            "runtime iv-module-definitions failed to produce an initial root definition");
    }
    root_definition->snapshot.canonical_builder = &root_definition->canonical_builder;
    return root_definition->snapshot;
}

void RuntimeIvModuleDefinitions::request_shutdown()
{
    std::scoped_lock lock(mutex);
    shutdown_requested = true;
    if (executor_state) {
        executor_state->request_shutdown();
    }
}
} // namespace iv
