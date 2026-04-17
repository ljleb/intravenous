#define IV_INTERNAL_TRANSLATION_UNIT

#include "devices/audio_device.h"
#include "devices/miniaudio_device.h"
#include "module/loader.h"
#include "module/search_paths.h"
#include "module/watcher.h"
#include "runtime/handlers.h"
#include "juce/vst_runtime.h"
#include "runtime/reload_worker.h"
#include "node/executor.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

namespace {
    iv::NodeExecutor* executor_state = nullptr;

    void request_shutdown()
    {
        if (executor_state) {
            executor_state->request_shutdown();
        }
    }

    std::optional<iv::LogicalAudioDevice> try_create_audio_device()
    {
        return iv::make_miniaudio_device({});
    }

    iv::ModuleRenderConfig module_render_config(iv::RenderConfig const& config)
    {
        return {
            .sample_rate = config.sample_rate,
            .num_channels = config.num_channels,
            .max_block_frames = config.max_block_frames,
        };
    }

    iv::DeviceOrchestrator make_audio_device_provider(iv::LogicalAudioDevice audio_device)
    {
        std::vector<iv::OutputDeviceMixer> mixers;
        mixers.emplace_back(
            std::move(audio_device),
            [](iv::OrchestratorBuilder& builder, iv::OutputDeviceMixer&& mixer) {
                builder.add_audio_mixer(0, std::move(mixer));
            }
        );
        return iv::DeviceOrchestrator(std::move(mixers));
    }

    iv::NodeExecutor make_executor(
        iv::RenderConfig const& render_config,
        iv::DeviceOrchestrator& device_orchestrator,
        iv::ModuleLoader::LoadedGraph loaded_graph
    )
    {
        iv::ResourceContext resources {};
#if IV_ENABLE_JUCE_VST
        static iv::JuceVstRuntimeManager juce_vst_runtime_manager;
        static std::unique_ptr<iv::JuceVstRuntimeSupport> juce_vst_runtime_support;
        juce_vst_runtime_support = std::make_unique<iv::JuceVstRuntimeSupport>(
            juce_vst_runtime_manager,
            static_cast<double>(render_config.sample_rate)
        );
        resources = juce_vst_runtime_support->resources();
#endif
        return iv::NodeExecutor::create(
            std::move(loaded_graph.root),
            std::move(resources),
            std::move(device_orchestrator).to_builder(),
            std::move(loaded_graph.module_refs)
        );
    }
}

int main(int argc, char** argv)
{
    iv::install_crash_handlers();
#if IV_ENABLE_JUCE_VST
    iv::warmup_juce_vst_scan_cache();
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

    auto audio_device = try_create_audio_device();
    if (!audio_device) {
        std::cerr << "No production audio backend is currently configured.\n";
        return 1;
    }

    std::cout << "Audio running. Press CTRL+C to quit.\n";

    iv::ModuleLoader loader(std::filesystem::current_path(), std::move(search_roots));
    auto watcher = iv::make_dependency_watcher();

    auto const render_config = audio_device->config();
    iv::Sample device_sample_period = iv::sample_period(render_config);

    auto loaded_graph = loader.load_root(
        module_path,
        module_render_config(render_config),
        &device_sample_period
    );
    watcher->update(loaded_graph.dependencies);

    iv::DeviceOrchestrator output_devices(make_audio_device_provider(std::move(*audio_device)));
    auto executor_storage = make_executor(render_config, output_devices, std::move(loaded_graph));
    executor_state = &executor_storage;

    iv::ReloadWorker reload_worker(
        *watcher,
        module_path,
        [&]() {
            return loader.load_root(
                module_path,
                module_render_config(render_config),
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
