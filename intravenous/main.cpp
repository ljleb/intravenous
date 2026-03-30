#include "devices/audio_device.h"
#include "module/loader.h"
#include "module/search_paths.h"
#include "module/watcher.h"
#include "runtime/handlers.h"
#include "runtime/juce_vst_runtime.h"
#include "runtime/reload_worker.h"
#include "node_executor.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <vector>

namespace {
    iv::NodeExecutor* executor = nullptr;

    void request_shutdown()
    {
        if (executor) {
            executor->request_shutdown();
        }
    }

    iv::ModuleRenderConfig module_render_config(iv::AudioDevice const& audio_device)
    {
        // TODO: Stop deriving module render config from one bootstrap audio device.
        // Graph/runtime config should eventually be negotiated from executor targets.
        return {
            .sample_rate = audio_device.config().sample_rate,
            .num_channels = audio_device.config().num_channels,
            .max_block_frames = audio_device.config().max_block_frames,
        };
    }

    iv::ExecutionTargets::AudioDeviceProvider make_audio_device_provider(iv::AudioDevice& audio_device)
    {
        // TODO: Replace this single-device bootstrap provider with a real device registry.
        // The executor should discover the active output devices from its targets, not from main().
        return {
            .owner = &audio_device,
            .device_fn = [](void* owner, size_t device_id) -> iv::AudioDevice* {
                if (device_id != 0) {
                    return nullptr;
                }
                return static_cast<iv::AudioDevice*>(owner);
            },
            .preferred_block_size_fn = [](void* owner) -> size_t {
                return static_cast<iv::AudioDevice*>(owner)->scheduling_block_size();
            },
        };
    }

    iv::NodeExecutor make_executor(iv::AudioDevice& audio_device, iv::ModuleLoader::LoadedGraph loaded_graph)
    {
        // TODO: Bootstrap currently assumes one output-device provider up front.
        // Move target/device negotiation out of main() once executor/runtime bootstrap is cleaned up.
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
            iv::ExecutionTargets(make_audio_device_provider(audio_device)),
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
    std::cout << "Audio running. Press CTRL+C to quit.\n";

    std::filesystem::path module_path = argv[1];
    auto search_roots = iv::parse_search_path_env();
    for (int i = 2; i < argc; ++i) {
        search_roots.emplace_back(argv[i]);
    }

    iv::AudioDevice audio_device;
    iv::ModuleLoader loader(std::filesystem::current_path(), std::move(search_roots));
    auto watcher = iv::make_dependency_watcher();

    auto loaded_graph = loader.load_root(
        module_path,
        module_render_config(audio_device),
        &audio_device.sample_period()
    );
    watcher->update(loaded_graph.dependencies);

    auto executor_storage = make_executor(audio_device, std::move(loaded_graph));
    executor = &executor_storage;

    iv::ReloadWorker reload_worker(
        loader,
        *watcher,
        module_path,
        [&]() {
            return loader.load_root(
                module_path,
                module_render_config(audio_device),
                &audio_device.sample_period()
            );
        }
    );
    reload_worker.start();

    executor->execute([&]() -> std::optional<iv::ModuleLoader::LoadedGraph> {
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
