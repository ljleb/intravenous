#include <intravenous/runtime/app.h>

#include <intravenous/compat.h>
#include <intravenous/juce/vst_runtime.h>
#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes_iv_module_instances_bridge.h>
#include <intravenous/runtime/graph_input_lanes_iv_module_instances_execution_bridge.h>
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
#include <intravenous/runtime/server_options.h>
#include <intravenous/runtime/socket_rpc_lane_views_bridge.h>
#include <intravenous/runtime/socket_rpc_iv_module_instances_bridge.h>
#include <intravenous/runtime/socket_rpc_notification_bridge.h>
#include <intravenous/runtime/socket_rpc_timeline_execution_bridge.h>
#include <intravenous/runtime/socket_rpc_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/startup_config.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/task_runner_graph_input_lanes_bridge.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_execution.h>
#include <intravenous/runtime/timeline_execution_lanes_visualization_bridge.h>
#include <intravenous/runtime/timeline_execution_task_runner_bridge.h>
#include <intravenous/runtime/timeline_lane_filters_bridge.h>
#include <intravenous/runtime/timeline_timeline_execution_bridge.h>

#include <filesystem>
#include <functional>
#include <iostream>

namespace iv {
    namespace {
        std::function<void()>* shutdown_callback = nullptr;

        void request_shutdown()
        {
            if (shutdown_callback) {
                (*shutdown_callback)();
            }
        }

        int run_server_mode(Timeline& timeline, int argc, char** argv)
        {
            auto const options = ServerOptions::parse(argc, argv);
            StartupConfig startup_config(
                options.workspace_root,
                std::filesystem::current_path(),
                std::vector<std::filesystem::path>{});
            auto const startup = startup_config.initialize();
            IvModuleInstances iv_module_instances;
            IvModuleDefinitions iv_module_definitions;
            IvModuleReload iv_module_reload(startup);
            GraphInputLanes graph_input_lanes;
            TaskRunner task_runner;
            TimelineExecution timeline_execution(
                startup.execution.block_size,
                startup.execution.compiled_sample_cache_chunk_size_multiplier);
            IvModuleInstancesExecution iv_module_instances_execution(startup.execution.block_size);
            LaneFilters lane_filters;
            LaneViews lane_views;
            LanesVisualization lanes_visualization(
                std::chrono::milliseconds(33),
                16,
                startup.execution.block_size);
            IvModuleSourceIntrospection introspection;
            bind_graph_input_lanes_timeline_bridge(graph_input_lanes, timeline);
            bind_task_runner_graph_input_lanes_bridge(graph_input_lanes);
            bind_timeline_execution_task_runner_bridge(timeline_execution, task_runner);
            bind_timeline_timeline_execution_bridge(timeline, timeline_execution);
            bind_iv_module_definitions_builder_bridge(iv_module_definitions);
            bind_iv_module_instances_iv_module_definitions_bridge(iv_module_definitions);
            bind_iv_module_instances_execution_task_runner_bridge(iv_module_instances_execution, task_runner);
            bind_iv_module_instances_iv_module_instances_execution_bridge(iv_module_instances_execution);
            bind_graph_input_lanes_iv_module_instances_execution_bridge(iv_module_instances_execution);
            bind_iv_module_definitions_iv_module_instances_bridge(iv_module_instances);
            bind_iv_module_definitions_iv_module_reload_bridge(iv_module_reload);
            bind_iv_module_definitions_iv_module_source_introspection_bridge(introspection);
            bind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
            bind_graph_input_lanes_iv_module_instances_bridge(iv_module_instances);
            bind_iv_module_reload_iv_module_definitions_bridge(iv_module_definitions);
            bind_iv_module_source_introspection_graph_input_lanes_bridge(graph_input_lanes);
            bind_timeline_lane_filters_bridge(lane_filters);
            bind_lane_filters_lane_views_bridge(lane_filters, lane_views);
            bind_lane_views_lanes_visualization_bridge(lanes_visualization);
            bind_lanes_visualization_timeline_bridge(lanes_visualization, timeline);
            bind_task_runner_lanes_visualization_bridge(lanes_visualization);
            bind_timeline_execution_lanes_visualization_bridge(timeline_execution);

            SocketRpcServer server(options.workspace_root, options.rpc_fd);
            std::function<void()> shutdown = [&]() {
                server.request_shutdown();
            };
            shutdown_callback = &shutdown;
            install_shutdown_handlers(request_shutdown);
            bind_socket_rpc_lane_views_bridge(lane_views);
            bind_socket_rpc_iv_module_instances_bridge(iv_module_instances);
            bind_socket_rpc_timeline_execution_bridge(
                timeline_execution,
                startup.workspace_root);
            bind_socket_rpc_iv_module_source_introspection_bridge(
                introspection,
                graph_input_lanes,
                &shutdown);
            bind_socket_rpc_notification_bridge(server);

            server.start();
            std::cout << "Intravenous server connected on rpc fd " << options.rpc_fd << '\n';
            server.wait();
            unbind_socket_rpc_notification_bridge(server);
            unbind_socket_rpc_lane_views_bridge(lane_views);
            unbind_socket_rpc_iv_module_instances_bridge(iv_module_instances);
            unbind_socket_rpc_timeline_execution_bridge(timeline_execution);
            unbind_socket_rpc_iv_module_source_introspection_bridge(
                introspection,
                graph_input_lanes);
            unbind_timeline_execution_lanes_visualization_bridge(timeline_execution);
            unbind_task_runner_lanes_visualization_bridge(lanes_visualization);
            unbind_lanes_visualization_timeline_bridge(lanes_visualization, timeline);
            unbind_lane_views_lanes_visualization_bridge(lanes_visualization);
            unbind_lane_filters_lane_views_bridge(lane_filters, lane_views);
            unbind_timeline_lane_filters_bridge(lane_filters);
            unbind_iv_module_source_introspection_graph_input_lanes_bridge(graph_input_lanes);
            unbind_iv_module_reload_iv_module_definitions_bridge(iv_module_definitions);
            unbind_graph_input_lanes_iv_module_instances_execution_bridge(iv_module_instances_execution);
            unbind_graph_input_lanes_iv_module_instances_bridge(iv_module_instances);
            unbind_iv_module_instances_execution_task_runner_bridge(iv_module_instances_execution, task_runner);
            unbind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
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
