#include "runtime/app.h"

#include "devices/miniaudio_device.h"
#include "juce/vst_runtime.h"
#include "module/loader.h"
#include "module/search_paths.h"
#include "module/watcher.h"
#include "node/executor.h"
#include "runtime/handlers.h"
#include "runtime/reload_worker.h"
#include "runtime/socket_rpc_server.h"

#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

namespace iv {
    namespace {
        std::function<void()>* shutdown_callback = nullptr;

        void request_shutdown()
        {
            if (shutdown_callback) {
                (*shutdown_callback)();
            }
        }

        std::optional<LogicalAudioDevice> try_create_audio_device()
        {
            return make_miniaudio_device({});
        }

        ModuleRenderConfig module_render_config(RenderConfig const& config)
        {
            return {
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
                [](OrchestratorBuilder& builder, OutputDeviceMixer&& mixer) {
                    builder.add_audio_mixer(0, std::move(mixer));
                }
            );
            return DeviceOrchestrator(std::move(mixers));
        }

        NodeExecutor make_executor(
            RenderConfig const& render_config,
            DeviceOrchestrator& device_orchestrator,
            ModuleLoader::LoadedGraph loaded_graph
        )
        {
            ResourceContext resources {};
#if IV_ENABLE_JUCE_VST
            static JuceVstRuntimeManager juce_vst_runtime_manager;
            static std::unique_ptr<JuceVstRuntimeSupport> juce_vst_runtime_support;
            juce_vst_runtime_support = std::make_unique<JuceVstRuntimeSupport>(
                juce_vst_runtime_manager,
                static_cast<double>(render_config.sample_rate)
            );
            resources = juce_vst_runtime_support->resources();
#endif
            return NodeExecutor::create(
                std::move(loaded_graph.root),
                std::move(resources),
                std::move(device_orchestrator).to_builder(),
                std::move(loaded_graph.module_refs)
            );
        }

        int run_audio_mode(int argc, char** argv)
        {
            if (argc < 2) {
                auto const sep = module_search_path_separator();
                std::cerr << "usage: intravenous <module-path> [search-root...]\n";
                std::cerr << "       intravenous --server --workspace-root <path> [--socket-path <path>]\n";
                std::cerr << "env:   IV_MODULE_SEARCH_PATH=root1" << sep << "root2\n";
                return 2;
            }

            std::filesystem::path module_path = argv[1];
            auto search_roots = parse_search_path_env();
            for (int i = 2; i < argc; ++i) {
                search_roots.emplace_back(argv[i]);
            }

            auto audio_device = try_create_audio_device();
            if (!audio_device) {
                std::cerr << "No production audio backend is currently configured.\n";
                return 1;
            }

            std::cout << "Audio running. Press CTRL+C to quit.\n";

            ModuleLoader loader(std::filesystem::current_path(), std::move(search_roots));
            auto watcher = make_dependency_watcher();

            auto const render_config = audio_device->config();
            Sample device_sample_period = sample_period(render_config);

            auto loaded_graph = loader.load_root(
                module_path,
                module_render_config(render_config),
                &device_sample_period
            );
            watcher->update(loaded_graph.dependencies);

            DeviceOrchestrator output_devices(make_audio_device_provider(std::move(*audio_device)));
            auto executor_storage = make_executor(render_config, output_devices, std::move(loaded_graph));

            std::function<void()> shutdown = [&]() {
                executor_storage.request_shutdown();
            };
            shutdown_callback = &shutdown;
            install_shutdown_handlers(request_shutdown);

            ReloadWorker reload_worker(
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

            executor_storage.execute([&]() -> std::optional<ModuleLoader::LoadedGraph> {
                if (auto exception = reload_worker.take_exception()) {
                    std::rethrow_exception(exception);
                }

                std::vector<ModuleDependency> dependencies;
                auto reload = reload_worker.take_completed_reload(&dependencies);
                if (reload) {
                    watcher->update(std::move(dependencies));
                    std::cout << "reloaded " << module_path.string() << '\n';
                }
                return reload;
            });

            reload_worker.request_shutdown();
            shutdown_callback = nullptr;
            return 0;
        }

        int run_server_mode(int argc, char** argv)
        {
            std::filesystem::path workspace_root;
            std::filesystem::path socket_path;

            for (int i = 2; i < argc; ++i) {
                std::string_view const arg = argv[i];
                if (arg == "--workspace-root" && i + 1 < argc) {
                    workspace_root = argv[++i];
                } else if (arg == "--socket-path" && i + 1 < argc) {
                    socket_path = argv[++i];
                } else {
                    throw std::runtime_error("unknown server argument: " + std::string(arg));
                }
            }

            if (workspace_root.empty()) {
                throw std::runtime_error("--server requires --workspace-root <path>");
            }

            SocketRpcServer server(workspace_root, std::filesystem::current_path(), socket_path);
            std::function<void()> shutdown = [&]() {
                server.request_shutdown();
            };
            shutdown_callback = &shutdown;
            install_shutdown_handlers(request_shutdown);

            server.start();
            std::cout << "Intravenous server listening on " << server.socket_path().string() << '\n';
            server.wait();
            shutdown_callback = nullptr;
            return 0;
        }
    }

    int run_intravenous_cli(int argc, char** argv)
    {
        install_crash_handlers();
#if IV_ENABLE_JUCE_VST
        warmup_juce_vst_scan_cache();
#endif

        if (argc >= 2 && std::string_view(argv[1]) == "--server") {
            return run_server_mode(argc, argv);
        }
        return run_audio_mode(argc, argv);
    }
}
