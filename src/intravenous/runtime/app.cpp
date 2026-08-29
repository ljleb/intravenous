#include <intravenous/runtime/app.h>

#include <intravenous/compat.h>
#include <intravenous/devices/miniaudio_device.h>
#include <intravenous/juce/vst_runtime.h>
#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/audio_device_lanes_iv_module_instances_execution_bridge.h>
#include <intravenous/runtime/authored_lanes.h>
#include <intravenous/runtime/authored_lanes_timeline_bridge.h>
#include <intravenous/runtime/audio_device_lanes_timeline_bridge.h>
#include <intravenous/runtime/audio_device_lanes_timeline_execution_bridge.h>
#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes_iv_module_instances_execution_bridge.h>
#include <intravenous/runtime/graph_input_lanes_timeline_bridge.h>
#include <intravenous/runtime/handlers.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_instances_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_reload_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_execution.h>
#include <intravenous/runtime/iv_module_instances_execution_task_runner_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_instances_execution_bridge.h>
#include <intravenous/runtime/iv_module_instances_graph_input_lanes_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/lane_filters.h>
#include <intravenous/runtime/lane_filters_lane_views_bridge.h>
#include <intravenous/runtime/lane_query_schema_service.h>
#include <intravenous/runtime/lane_views.h>
#include <intravenous/runtime/lane_views_lanes_visualization_bridge.h>
#include <intravenous/runtime/lanes_visualization.h>
#include <intravenous/runtime/lanes_visualization_timeline_bridge.h>
#include <intravenous/runtime/task_runner_lanes_visualization_bridge.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/iv_module_sources.h>
#include <intravenous/module/search_paths.h>
#include <intravenous/runtime/iv_module_source_introspection_graph_input_lanes_bridge.h>
#include <intravenous/runtime/project_persistence.h>
#include <intravenous/runtime/project_autosave.h>
#include <intravenous/runtime/project_persistence_project_autosave_bridge.h>
#include <intravenous/runtime/project_persistence_audio_device_lanes_bridge.h>
#include <intravenous/runtime/project_persistence_graph_input_lanes_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_sources_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/project_persistence_iv_module_instances_bridge.h>
#include <intravenous/runtime/project_persistence_iv_module_reload_bridge.h>
#include <intravenous/runtime/project_persistence_authored_lanes_bridge.h>
#include <intravenous/runtime/project_persistence_timeline_bridge.h>
#include <intravenous/runtime/project_persistence_timeline_execution_bridge.h>
#include <intravenous/runtime/server_options.h>
#include <intravenous/runtime/socket_rpc_lane_views_bridge.h>
#include <intravenous/runtime/socket_rpc_lane_query_schema_bridge.h>
#include <intravenous/runtime/socket_rpc_lane_query_completion_bridge.h>
#include <intravenous/runtime/socket_rpc_audio_device_lanes_bridge.h>
#include <intravenous/runtime/socket_rpc_iv_module_instances_bridge.h>
#include <intravenous/runtime/socket_rpc_iv_module_sources_bridge.h>
#include <intravenous/runtime/iv_module_definitions_socket_rpc_notification_bridge.h>
#include <intravenous/runtime/lanes_visualization_socket_rpc_notification_bridge.h>
#include <intravenous/runtime/socket_rpc_project_persistence_bridge.h>
#include <intravenous/runtime/socket_rpc_project_autosave_bridge.h>
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
#include <intravenous/runtime/timeline_execution_iv_module_instances_execution_bridge.h>
#include <intravenous/runtime/timeline_execution_lanes_visualization_bridge.h>
#include <intravenous/runtime/timeline_execution_task_runner_bridge.h>
#include <intravenous/runtime/timeline_lane_filters_bridge.h>
#include <intravenous/runtime/timeline_lane_query_schema_bridge.h>
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
            IvModuleInstances* instances_ = nullptr;
            IvModuleSources* sources_ = nullptr;
            std::optional<std::jthread> thread_ {};

        public:
            explicit IvModuleReloadWatcherService(
                IvModuleReload& reload,
                IvModuleInstances& instances,
                IvModuleSources& sources)
                : reload_(&reload)
                , instances_(&instances)
                , sources_(&sources)
            {
            }

            void start()
            {
                if (thread_.has_value()) {
                    return;
                }

                thread_.emplace([this](std::stop_token stop_token) {
                    while (!stop_token.stop_requested()) {
                        instances_->refresh_source_roots(*sources_);
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

        class ProjectAutosaveService {
            ProjectAutosave* autosave_ = nullptr;
            ProjectPersistence* persistence_ = nullptr;
            std::optional<std::jthread> thread_ {};

        public:
            ProjectAutosaveService(
                ProjectAutosave& autosave,
                ProjectPersistence& persistence)
                : autosave_(&autosave)
                , persistence_(&persistence)
            {
            }

            void start()
            {
                if (thread_.has_value()) {
                    return;
                }

                thread_.emplace([this](std::stop_token stop_token) {
                    while (!stop_token.stop_requested()) {
                        if (autosave_->take_due_save()) {
                            try {
                                persistence_->save();
                                autosave_->save_succeeded();
                            } catch (std::exception const& exception) {
                                autosave_->save_failed();
                                persistence_->report_autosave_failure(exception.what());
                            } catch (...) {
                                autosave_->save_failed();
                                persistence_->report_autosave_failure("unknown failure");
                            }
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

            void stop()
            {
                request_shutdown();
                thread_.reset();
                // Joining first lets an already-started write complete. Then
                // synchronously drain any debounced revision so a server
                // shutdown cannot discard the last user action.
                while (autosave_->take_pending_save()) {
                    try {
                        persistence_->save();
                        autosave_->save_succeeded();
                    } catch (std::exception const& exception) {
                        autosave_->save_failed();
                        persistence_->report_autosave_failure(exception.what());
                        break;
                    } catch (...) {
                        autosave_->save_failed();
                        persistence_->report_autosave_failure("unknown failure");
                        break;
                    }
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
            AuthoredLanes authored_lanes(LaneCreationContext{.sample_rate = startup.execution.sample_rate});
            TasksRunner task_runner;
            startup_log("constructing runtime modules");
            TimelineExecution timeline_execution(
                startup.execution.block_size,
                startup.execution.compiled_sample_cache_chunk_size_multiplier,
                true);
            IvModuleInstancesExecution iv_module_instances_execution(
                startup.execution.block_size,
                false,
                startup.execution.sample_rate);
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
            LaneQuerySchemaService lane_query_schema;
            LaneViews lane_views;
            LanesVisualization lanes_visualization(
                std::chrono::milliseconds(33),
                startup.execution.block_size);
            IvModuleSourceIntrospection introspection;
            IvModuleSources iv_module_sources(
                startup.workspace_root,
                parse_search_path_env());
            startup_log("binding runtime bridges");
            auto audio_device_lanes_timeline_scope =
                audio_device_lanes_timeline_bridge::bind(
                    audio_device_lanes,
                    timeline);
            auto audio_device_lanes_timeline_execution_scope =
                audio_device_lanes_timeline_execution_bridge::bind(
                    audio_device_lanes,
                    timeline_execution);
            auto task_runner_audio_device_lanes_scope =
                task_runner_audio_device_lanes_bridge::bind(
                    task_runner,
                    audio_device_lanes);
            auto graph_input_lanes_timeline_scope =
                graph_input_lanes_timeline_bridge::bind(
                    graph_input_lanes,
                    timeline);
            auto task_runner_graph_input_lanes_scope =
                task_runner_graph_input_lanes_bridge::bind(
                    task_runner,
                    graph_input_lanes);
            auto timeline_execution_task_runner_scope =
                timeline_execution_task_runner_bridge::bind(timeline_execution, task_runner);
            auto timeline_timeline_execution_scope =
                timeline_timeline_execution_bridge::bind(timeline, timeline_execution);
            auto authored_lanes_timeline_scope =
                authored_lanes_timeline_bridge::bind(authored_lanes, timeline);
            timeline_execution.publish_task_graph_update(
                timeline.with_graph([&](LaneGraph const &graph) {
                    return timeline_execution.synchronize_from_graph(graph);
                }));
            auto iv_module_definitions_iv_module_instances_scope =
                iv_module_definitions_iv_module_instances_bridge::bind(
                    iv_module_definitions,
                    iv_module_instances);
            auto iv_module_instances_execution_task_runner_scope =
                iv_module_instances_execution_task_runner_bridge::bind(
                    iv_module_instances_execution,
                    task_runner);
            auto iv_module_instances_iv_module_instances_execution_scope =
                iv_module_instances_iv_module_instances_execution_bridge::bind(
                    iv_module_instances,
                    iv_module_instances_execution);
            auto graph_input_lanes_iv_module_instances_execution_scope =
                graph_input_lanes_iv_module_instances_execution_bridge::bind(
                    graph_input_lanes,
                    iv_module_instances_execution);
            auto audio_device_lanes_iv_module_instances_execution_scope =
                audio_device_lanes_iv_module_instances_execution_bridge::bind(
                    audio_device_lanes,
                    iv_module_instances_execution);
            auto timeline_execution_iv_module_instances_execution_scope =
                timeline_execution_iv_module_instances_execution_bridge::bind(
                    timeline_execution,
                    iv_module_instances_execution);
            auto iv_module_instances_iv_module_sources_scope =
                iv_module_instances_iv_module_sources_bridge::bind(
                    iv_module_instances,
                    iv_module_sources);
            auto iv_module_definitions_iv_module_reload_scope =
                iv_module_definitions_iv_module_reload_bridge::bind(
                    iv_module_definitions,
                    iv_module_reload);
            auto task_runner_iv_module_reload_scope =
                task_runner_iv_module_reload_bridge::bind(task_runner, iv_module_reload);
            auto iv_module_definitions_iv_module_source_introspection_scope =
                iv_module_definitions_iv_module_source_introspection_bridge::bind(
                    iv_module_definitions,
                    introspection);
            auto iv_module_instances_iv_module_source_introspection_scope =
                iv_module_instances_iv_module_source_introspection_bridge::bind(
                    iv_module_instances,
                    introspection);
            auto iv_module_instances_graph_input_lanes_scope =
                iv_module_instances_graph_input_lanes_bridge::bind(
                    iv_module_instances,
                    graph_input_lanes);
            auto iv_module_source_introspection_graph_input_lanes_scope =
                iv_module_source_introspection_graph_input_lanes_bridge::bind(
                    introspection,
                    graph_input_lanes);
            auto timeline_lane_filters_scope =
                timeline_lane_filters_bridge::bind(timeline, lane_filters);
            lane_query_schema.initialize(timeline.lane_query_schema(0));
            auto timeline_lane_query_schema_scope =
                timeline_lane_query_schema_bridge::bind(timeline, lane_query_schema);
            auto lane_filters_lane_views_scope =
                lane_filters_lane_views_bridge::bind(&lane_filters, &lane_views);
            auto lane_views_lanes_visualization_scope =
                lane_views_lanes_visualization_bridge::bind(lane_views, lanes_visualization);
            auto lanes_visualization_timeline_scope =
                lanes_visualization_timeline_bridge::bind(
                    lanes_visualization,
                    timeline);
            auto task_runner_lanes_visualization_scope =
                task_runner_lanes_visualization_bridge::bind(
                    task_runner,
                    lanes_visualization);
            auto timeline_execution_lanes_visualization_scope =
                timeline_execution_lanes_visualization_bridge::bind(
                    timeline_execution,
                    lanes_visualization);
            startup_log("binding audio device lanes");
            audio_device_lanes.bind();
            ProjectPersistence project_persistence(
                startup.workspace_root,
                startup);
            auto project_persistence_timeline_execution_scope =
                project_persistence_timeline_execution_bridge::bind(
                    project_persistence,
                    timeline_execution);
            auto project_persistence_timeline_scope =
                project_persistence_timeline_bridge::bind(
                    project_persistence,
                    timeline);
            auto project_persistence_authored_lanes_scope =
                project_persistence_authored_lanes_bridge::bind(
                    project_persistence,
                    authored_lanes);
            auto project_persistence_iv_module_instances_scope =
                project_persistence_iv_module_instances_bridge::bind(
                    project_persistence,
                    iv_module_instances);
            auto project_persistence_iv_module_reload_scope =
                project_persistence_iv_module_reload_bridge::bind(
                    project_persistence,
                    iv_module_reload);
            auto project_persistence_audio_device_lanes_scope =
                project_persistence_audio_device_lanes_bridge::bind(
                    project_persistence,
                    audio_device_lanes);
            auto project_persistence_graph_input_lanes_scope =
                project_persistence_graph_input_lanes_bridge::bind(
                    project_persistence,
                    graph_input_lanes);
            ProjectAutosave project_autosave;
            auto project_persistence_project_autosave_scope =
                project_persistence_project_autosave_bridge::bind(
                    project_persistence,
                    project_autosave);
            ProjectAutosaveService project_autosave_service(
                project_autosave,
                project_persistence);

            startup_log("constructing socket rpc server");
            SocketRpcServer server(options.workspace_root, options.rpc_fd);
            IvModuleReloadWatcherService iv_module_reload_watcher(
                iv_module_reload,
                iv_module_instances,
                iv_module_sources);
            std::function<void()> shutdown = [&]() {
                iv_module_reload_watcher.request_shutdown();
                project_autosave_service.request_shutdown();
                server.request_shutdown();
            };
            shutdown_callback = &shutdown;
            install_shutdown_handlers(request_shutdown);
            startup_log("binding socket rpc bridges");
            auto socket_rpc_lane_views_scope =
                socket_rpc_lane_views_bridge::bind(server, lane_views);
            auto socket_rpc_lane_query_schema_scope =
                socket_rpc_lane_query_schema_bridge::bind(server, lane_query_schema);
            auto socket_rpc_lane_query_completion_scope =
                socket_rpc_lane_query_completion_bridge::bind(server, lane_query_schema);
            auto socket_rpc_audio_device_lanes_scope =
                socket_rpc_audio_device_lanes_bridge::bind(server, audio_device_lanes);
            auto socket_rpc_iv_module_instances_scope =
                socket_rpc_iv_module_instances_bridge::bind(
                    server,
                    iv_module_instances);
            auto socket_rpc_iv_module_sources_scope =
                socket_rpc_iv_module_sources_bridge::bind(
                    server,
                    iv_module_sources);
            auto socket_rpc_timeline_execution_scope =
                socket_rpc_timeline_execution_bridge::bind(server, timeline_execution);
            auto socket_rpc_iv_module_source_introspection_scope =
                socket_rpc_iv_module_source_introspection_bridge::bind(
                    server,
                    introspection);
            auto iv_module_definitions_socket_rpc_notification_scope =
                iv_module_definitions_socket_rpc_notification_bridge::bind(
                    iv_module_definitions,
                    server);
            auto lanes_visualization_socket_rpc_notification_scope =
                lanes_visualization_socket_rpc_notification_bridge::bind(
                    lanes_visualization,
                    server);
            auto socket_rpc_project_persistence_scope =
                socket_rpc_project_persistence_bridge::bind(server, project_persistence);
            auto socket_rpc_project_autosave_scope =
                socket_rpc_project_autosave_bridge::bind(server, project_autosave);

            startup_log("loading project persistence");
            project_persistence.load();
            startup_log("project persistence loaded");
            project_autosave_service.start();
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
            project_autosave_service.stop();
            audio_device_lanes.request_shutdown();
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
