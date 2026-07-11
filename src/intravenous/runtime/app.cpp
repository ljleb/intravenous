#include <intravenous/runtime/app.h>

#include <intravenous/compat.h>
#include <intravenous/devices/miniaudio_device.h>
#include <intravenous/juce/vst_runtime.h>
#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/audio_device_lanes_timeline_bridge.h>
#include <intravenous/runtime/audio_device_lanes_timeline_execution_bridge.h>
#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes_iv_module_instances_bridge.h>
#include <intravenous/runtime/graph_input_lanes_timeline_bridge.h>
#include <intravenous/runtime/handlers.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_definitions_builder_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_instances_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_reload_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_execution.h>
#include <intravenous/runtime/iv_module_instances_execution_task_runner_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_definitions_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_instances_execution_bridge.h>
#include <intravenous/runtime/iv_module_instances_graph_input_lanes_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/iv_module_reload_iv_module_definitions_bridge.h>
#include <intravenous/runtime/lane_filters.h>
#include <intravenous/runtime/lane_filters_lane_views_bridge.h>
#include <intravenous/runtime/lane_views.h>
#include <intravenous/runtime/lane_views_lanes_visualization_bridge.h>
#include <intravenous/runtime/lanes_visualization.h>
#include <intravenous/runtime/lanes_visualization_timeline_bridge.h>
#include <intravenous/runtime/task_runner_lanes_visualization_bridge.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/iv_module_source_introspection_graph_input_lanes_bridge.h>
#include <intravenous/runtime/project_persistence.h>
#include <intravenous/runtime/runtime_project_audio_device_lanes_bridge.h>
#include <intravenous/runtime/runtime_project_graph_input_lanes_bridge.h>
#include <intravenous/runtime/runtime_project_iv_module_instances_bridge.h>
#include <intravenous/runtime/runtime_project_iv_module_reload_bridge.h>
#include <intravenous/runtime/runtime_project_lane_views_bridge.h>
#include <intravenous/runtime/runtime_project_timeline_execution_bridge.h>
#include <intravenous/runtime/server_options.h>
#include <intravenous/runtime/socket_rpc_lane_views_bridge.h>
#include <intravenous/runtime/socket_rpc_audio_device_lanes_bridge.h>
#include <intravenous/runtime/socket_rpc_iv_module_instances_bridge.h>
#include <intravenous/runtime/socket_rpc_notification_bridge.h>
#include <intravenous/runtime/socket_rpc_timeline_execution_bridge.h>
#include <intravenous/runtime/socket_rpc_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/startup_config.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/task_runner_audio_device_lanes_bridge.h>
#include <intravenous/runtime/task_runner_graph_input_lanes_bridge.h>
#include <intravenous/runtime/task_runner_iv_module_reload_bridge.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_execution.h>
#include <intravenous/runtime/timeline_execution_lanes_visualization_bridge.h>
#include <intravenous/runtime/timeline_execution_task_runner_bridge.h>
#include <intravenous/runtime/timeline_lane_filters_bridge.h>
#include <intravenous/runtime/timeline_timeline_execution_bridge.h>

#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace iv {
    namespace {
        std::function<void()>* shutdown_callback = nullptr;

        class IvModuleReloadWatcherService {
            IvModuleReload* reload_ = nullptr;
            std::optional<std::jthread> thread_ {};

        public:
            explicit IvModuleReloadWatcherService(
                IvModuleReload& reload)
                : reload_(&reload)
            {
            }

            void start()
            {
                if (thread_.has_value()) {
                    return;
                }

                thread_.emplace([this](std::stop_token stop_token) {
                    while (!stop_token.stop_requested()) {
                        if (reload_->has_dirty_definitions()) {
                            reload_->compile_dirty_definitions();
                        } else {
                            reload_->reload_changed_definitions();
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                });
            }

            void request_shutdown()
            {
                if (thread_.has_value()) {
                    thread_->request_stop();
                }
            }
        };

        void startup_log(std::string const &message)
        {
            std::cerr << "[intravenous startup] " << message << '\n';
        }

        void request_shutdown()
        {
            if (shutdown_callback) {
                (*shutdown_callback)();
            }
        }

        int run_server_mode(Timeline& timeline, int argc, char** argv)
        {
            startup_log("parsing server options");
            auto const options = ServerOptions::parse(argc, argv);
            startup_log("initializing startup config");
            StartupConfig startup_config(
                options.workspace_root,
                std::filesystem::current_path(),
                std::vector<std::filesystem::path>{});
            auto const startup = startup_config.initialize();
            startup_log("startup config initialized");
            IvModuleInstances iv_module_instances;
            IvModuleDefinitions iv_module_definitions;
            IvModuleReload iv_module_reload(startup);
            GraphInputLanes graph_input_lanes;
            TasksRunner task_runner;
            startup_log("constructing runtime modules");
            TimelineExecution timeline_execution(
                startup.execution.block_size,
                startup.execution.compiled_sample_cache_chunk_size_multiplier);
            IvModuleInstancesExecution iv_module_instances_execution(startup.execution.block_size);
            AudioDeviceLanes audio_device_lanes(
                startup.execution.sample_rate,
                startup.execution.block_size,
                AudioDeviceLanesBackend{
                    .list_output_devices = [] {
                        return list_miniaudio_output_devices();
                    },
                    .list_input_devices = [] {
                        return list_miniaudio_input_devices();
                    },
                    .make_output_device = [](
                        std::string const &device_id,
                        RenderConfig const &config) {
                        return make_miniaudio_output_device(config, device_id);
                    },
                    .make_input_device = [](
                        std::string const &device_id,
                        RenderConfig const &config) {
                        return make_miniaudio_input_device(config, device_id);
                    },
                },
                startup.output_device_id,
                startup.input_device_id);
            LaneFilters lane_filters;
            LaneViews lane_views;
            LanesVisualization lanes_visualization(
                std::chrono::milliseconds(33),
                16,
                startup.execution.block_size);
            IvModuleSourceIntrospection introspection;
            startup_log("binding runtime bridges");
            bind_audio_device_lanes_timeline_bridge(audio_device_lanes, timeline);
            bind_audio_device_lanes_timeline_execution_bridge(audio_device_lanes, timeline_execution);
            bind_task_runner_audio_device_lanes_bridge(audio_device_lanes);
            bind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
            bind_graph_input_lanes_timeline_bridge(graph_input_lanes, timeline);
            bind_task_runner_graph_input_lanes_bridge(graph_input_lanes);
            bind_runtime_project_graph_input_lanes_bridge(graph_input_lanes);
            bind_timeline_execution_task_runner_bridge(timeline_execution, task_runner);
            bind_timeline_timeline_execution_bridge(timeline, timeline_execution);
            bind_runtime_project_timeline_execution_bridge(
                timeline,
                timeline_execution,
                startup.workspace_root);
            bind_iv_module_definitions_builder_bridge(iv_module_definitions);
            bind_iv_module_instances_iv_module_definitions_bridge(iv_module_definitions);
            bind_iv_module_instances_execution_task_runner_bridge(iv_module_instances_execution, task_runner);
            bind_iv_module_instances_iv_module_instances_execution_bridge(iv_module_instances_execution);
            bind_iv_module_definitions_iv_module_instances_bridge(iv_module_instances);
            bind_runtime_project_iv_module_instances_bridge(iv_module_instances);
            bind_iv_module_definitions_iv_module_reload_bridge(iv_module_reload);
            bind_runtime_project_iv_module_reload_bridge(iv_module_reload);
            bind_task_runner_iv_module_reload_bridge(iv_module_reload);
            bind_iv_module_definitions_iv_module_source_introspection_bridge(introspection);
            bind_iv_module_instances_iv_module_source_introspection_bridge(introspection);
            bind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
            bind_graph_input_lanes_iv_module_instances_bridge(iv_module_instances);
            bind_iv_module_reload_iv_module_definitions_bridge(iv_module_definitions);
            bind_iv_module_source_introspection_graph_input_lanes_bridge(graph_input_lanes);
            bind_timeline_lane_filters_bridge(lane_filters);
            bind_lane_filters_lane_views_bridge(lane_filters, lane_views);
            bind_runtime_project_lane_views_bridge(lane_views);
            bind_lane_views_lanes_visualization_bridge(lanes_visualization);
            bind_lanes_visualization_timeline_bridge(lanes_visualization, timeline);
            bind_task_runner_lanes_visualization_bridge(lanes_visualization);
            bind_timeline_execution_lanes_visualization_bridge(timeline_execution);
            startup_log("binding audio device lanes");
            audio_device_lanes.bind();
            ProjectPersistence project_persistence(
                startup.workspace_root,
                startup);

            startup_log("constructing socket rpc server");
            SocketRpcServer server(options.workspace_root, options.rpc_fd);
            IvModuleReloadWatcherService iv_module_reload_watcher(iv_module_reload);
            std::function<void()> shutdown = [&]() {
                iv_module_reload_watcher.request_shutdown();
                server.request_shutdown();
            };
            shutdown_callback = &shutdown;
            install_shutdown_handlers(request_shutdown);
            startup_log("binding socket rpc bridges");
            bind_socket_rpc_lane_views_bridge(lane_views);
            bind_socket_rpc_audio_device_lanes_bridge(audio_device_lanes);
            bind_socket_rpc_iv_module_instances_bridge(iv_module_instances);
            bind_socket_rpc_timeline_execution_bridge(
                timeline,
                timeline_execution,
                startup.workspace_root);
            bind_socket_rpc_iv_module_source_introspection_bridge(
                introspection,
                graph_input_lanes,
                &shutdown);
            bind_socket_rpc_notification_bridge(server);
            bind_project_persistence_bridge(project_persistence);

            startup_log("loading project persistence");
            project_persistence.load();
            startup_log("project persistence loaded");
            startup_log("starting socket rpc server");
            server.start();
            startup_log("socket rpc server started");
            if (!server.wait_until_ready(std::chrono::seconds(10))) {
                throw std::runtime_error("socket rpc server did not deliver server.ready");
            }
            startup_log("starting iv module reload watcher");
            iv_module_reload_watcher.start();
            std::cout << "Intravenous server connected on rpc fd " << options.rpc_fd << '\n';
            server.wait();
            startup_log("socket rpc server stopped");
            iv_module_reload_watcher.request_shutdown();
            audio_device_lanes.request_shutdown();
            unbind_socket_rpc_notification_bridge(server);
            unbind_socket_rpc_lane_views_bridge(lane_views);
            unbind_socket_rpc_audio_device_lanes_bridge(audio_device_lanes);
            unbind_socket_rpc_iv_module_instances_bridge(iv_module_instances);
            unbind_socket_rpc_timeline_execution_bridge(timeline_execution);
            unbind_socket_rpc_iv_module_source_introspection_bridge(
                introspection,
                graph_input_lanes);
            unbind_project_persistence_bridge(project_persistence);
            unbind_runtime_project_lane_views_bridge(lane_views);
            unbind_runtime_project_iv_module_reload_bridge(iv_module_reload);
            unbind_task_runner_iv_module_reload_bridge(iv_module_reload);
            unbind_runtime_project_iv_module_instances_bridge(iv_module_instances);
            unbind_runtime_project_timeline_execution_bridge(timeline_execution);
            unbind_runtime_project_graph_input_lanes_bridge(graph_input_lanes);
            unbind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
            unbind_task_runner_audio_device_lanes_bridge(audio_device_lanes);
            unbind_audio_device_lanes_timeline_execution_bridge(audio_device_lanes, timeline_execution);
            unbind_audio_device_lanes_timeline_bridge(audio_device_lanes, timeline);
            unbind_timeline_execution_lanes_visualization_bridge(timeline_execution);
            unbind_task_runner_lanes_visualization_bridge(lanes_visualization);
            unbind_lanes_visualization_timeline_bridge(lanes_visualization, timeline);
            unbind_lane_views_lanes_visualization_bridge(lanes_visualization);
            unbind_lane_filters_lane_views_bridge(lane_filters, lane_views);
            unbind_timeline_lane_filters_bridge(lane_filters);
            unbind_iv_module_source_introspection_graph_input_lanes_bridge(graph_input_lanes);
            unbind_iv_module_reload_iv_module_definitions_bridge(iv_module_definitions);
            unbind_graph_input_lanes_iv_module_instances_bridge(iv_module_instances);
            unbind_iv_module_instances_execution_task_runner_bridge(iv_module_instances_execution, task_runner);
            unbind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
            unbind_iv_module_instances_iv_module_source_introspection_bridge(introspection);
            unbind_iv_module_definitions_iv_module_source_introspection_bridge(introspection);
            unbind_iv_module_definitions_iv_module_reload_bridge(iv_module_reload);
            unbind_iv_module_definitions_iv_module_instances_bridge(iv_module_instances);
            unbind_iv_module_instances_iv_module_instances_execution_bridge(iv_module_instances_execution);
            unbind_iv_module_instances_iv_module_definitions_bridge(iv_module_definitions);
            unbind_iv_module_definitions_builder_bridge(iv_module_definitions);
            unbind_timeline_execution_task_runner_bridge(timeline_execution, task_runner);
            unbind_timeline_timeline_execution_bridge(timeline, timeline_execution);
            unbind_task_runner_graph_input_lanes_bridge(graph_input_lanes);
            unbind_graph_input_lanes_timeline_bridge(graph_input_lanes, timeline);
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
        Timeline timeline;

        if (argc >= 2 && std::string_view(argv[1]) == "--server") {
            return run_server_mode(timeline, argc, argv);
        }
        throw std::runtime_error("intravenous runs as a server; use --server --workspace-root <path>");
    }
}
