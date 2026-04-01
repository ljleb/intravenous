#include "devices/audio_device.h"
#include "devices/miniaudio_device.h"
#include "module/loader.h"
#include "module/search_paths.h"
#include "module/watcher.h"
#include "runtime/handlers.h"
#include "runtime/juce_vst_runtime.h"
#include "runtime/reload_worker.h"
#include "node_executor.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <condition_variable>
#include <vector>

namespace {
    iv::NodeExecutor* executor_state = nullptr;

    void request_shutdown()
    {
        if (executor_state) {
            executor_state->request_shutdown();
        }
    }

    std::optional<iv::LogicalAudioDevice> try_create_audio_device(iv::RequestNotification request_notification)
    {
        return iv::make_miniaudio_device({}, std::move(request_notification));
    }

    iv::ModuleRenderConfig module_render_config(iv::LogicalAudioDevice const& audio_device)
    {
        return {
            .sample_rate = audio_device.config().sample_rate,
            .num_channels = audio_device.config().num_channels,
            .max_block_frames = audio_device.config().max_block_frames,
        };
    }

    iv::ExecutionTargetRegistry::AudioDeviceProvider make_audio_device_provider(iv::LogicalAudioDevice& audio_device)
    {
        return {
            .owner = &audio_device,
            .device_fn = [](void* owner, size_t device_id) -> iv::LogicalAudioDevice* {
                if (device_id != 0) {
                    return nullptr;
                }
                return static_cast<iv::LogicalAudioDevice*>(owner);
            },
            .preferred_block_size_fn = [](void* owner) -> size_t {
                return static_cast<iv::LogicalAudioDevice*>(owner)->config().preferred_block_size;
            },
        };
    }

    iv::NodeExecutor make_executor(
        iv::LogicalAudioDevice& audio_device,
        iv::ExecutionTargetRegistry& execution_target_registry,
        size_t executor_id,
        iv::ModuleLoader::LoadedGraph loaded_graph
    )
    {
        iv::ResourceContext resources {};
#if IV_ENABLE_JUCE_VST
        static iv::JuceVstRuntimeManager juce_vst_runtime_manager;
        static std::unique_ptr<iv::JuceVstRuntimeSupport> juce_vst_runtime_support;
        juce_vst_runtime_support = std::make_unique<iv::JuceVstRuntimeSupport>(
            juce_vst_runtime_manager,
            static_cast<double>(audio_device.config().sample_rate)
        );
        resources = juce_vst_runtime_support->resources();
#endif
        return iv::NodeExecutor::create(
            std::move(loaded_graph.root),
            std::move(resources),
            execution_target_registry,
            executor_id,
            std::move(loaded_graph.module_refs)
        );
    }
}

int main(int argc, char** argv)
{
#ifndef NDEBUG
    iv::install_crash_handlers();
#endif

    if (argc < 2) {
        auto const sep = iv::module_search_path_separator();
        std::cerr << "usage: intravenous <module-path> [search-root...]\n";
        std::cerr << "env:   IV_MODULE_SEARCH_PATH=root1"<<sep<<"root2\n";
        return 2;
    }

    iv::install_shutdown_handlers(request_shutdown);

    std::filesystem::path module_path = argv[1];
    auto search_roots = iv::parse_search_path_env();
    for (int i = 2; i < argc; ++i) {
        search_roots.emplace_back(argv[i]);
    }

    auto request_notification = std::make_shared<std::condition_variable>();
    auto audio_device = try_create_audio_device(request_notification);
    if (!audio_device) {
        std::cerr << "No production audio backend is currently configured.\n";
        return 1;
    }

    std::cout << "Audio running. Press CTRL+C to quit.\n";

    iv::ModuleLoader loader(std::filesystem::current_path(), std::move(search_roots));
    auto watcher = iv::make_dependency_watcher();

    iv::Sample device_sample_period = iv::sample_period(audio_device->config());

    auto loaded_graph = loader.load_root(
        module_path,
        module_render_config(*audio_device),
        &device_sample_period
    );
    watcher->update(loaded_graph.dependencies);

    iv::ExecutionTargetRegistry execution_targets(make_audio_device_provider(*audio_device), 48000, request_notification);
    auto executor_storage = make_executor(*audio_device, execution_targets, 1, std::move(loaded_graph));
    executor_state = &executor_storage;

    iv::ReloadWorker reload_worker(
        loader,
        *watcher,
        module_path,
        [&]() {
            return loader.load_root(
                module_path,
                module_render_config(*audio_device),
                &device_sample_period
            );
        }
    );
    reload_worker.start();

    executor_state->execute([&]() -> std::optional<iv::ModuleLoader::LoadedGraph> {
        if (auto exception = reload_worker.take_exception()) {
            std::rethrow_exception(exception);
        }

        std::vector<iv::ModuleDependency> dependencies;
        auto reload = reload_worker.take_completed_reload(&dependencies);
        if (reload) {
            watcher->update(std::move(dependencies));
            std::cout << "reloaded " << module_path.string() << '\n';
        }
        return reload;
    });

    reload_worker.request_shutdown();
    return 0;
}
