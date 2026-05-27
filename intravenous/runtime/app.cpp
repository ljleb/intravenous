#include "runtime/app.h"

#include "compat.h"
#include "juce/vst_runtime.h"
#include "runtime/graph_input_lanes.h"
#include "runtime/graph_input_lanes_lane_views_bridge.h"
#include "runtime/graph_input_lanes_timeline_bridge.h"
#include "runtime/handlers.h"
#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_definitions_iv_module_instances_bridge.h"
#include "runtime/iv_module_definitions_iv_module_reload_bridge.h"
#include "runtime/iv_module_definitions_project_introspection_bridge.h"
#include "runtime/iv_module_instances.h"
#include "runtime/iv_module_instances_iv_module_definitions_bridge.h"
#include "runtime/iv_module_instances_graph_input_lanes_bridge.h"
#include "runtime/iv_module_reload.h"
#include "runtime/iv_module_reload_iv_module_definitions_bridge.h"
#include "runtime/lane_views.h"
#include "runtime/lane_views_timeline_bridge.h"
#include "runtime/project_introspection.h"
#include "runtime/project_introspection_graph_input_lanes_bridge.h"
#include "runtime/server_options.h"
#include "runtime/socket_rpc_lane_views_bridge.h"
#include "runtime/socket_rpc_iv_module_instances_bridge.h"
#include "runtime/socket_rpc_notification_bridge.h"
#include "runtime/socket_rpc_project_introspection_bridge.h"
#include "runtime/startup_config.h"
#include "runtime/socket_rpc_server.h"
#include "runtime/timeline.h"

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
            LaneViews lane_views;
            ProjectIntrospection introspection;
            bind_graph_input_lanes_timeline_bridge(timeline);
            bind_graph_input_lanes_lane_views_bridge(graph_input_lanes);
            bind_iv_module_instances_iv_module_definitions_bridge(iv_module_definitions);
            bind_iv_module_definitions_iv_module_instances_bridge(iv_module_instances);
            bind_iv_module_definitions_iv_module_reload_bridge(iv_module_reload);
            bind_iv_module_definitions_project_introspection_bridge(introspection);
            bind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
            bind_iv_module_reload_iv_module_definitions_bridge(iv_module_definitions);
            bind_project_introspection_graph_input_lanes_bridge(graph_input_lanes);
            bind_lane_views_timeline_bridge(lane_views);

            SocketRpcServer server(options.workspace_root, options.rpc_fd);
            std::function<void()> shutdown = [&]() {
                server.request_shutdown();
            };
            shutdown_callback = &shutdown;
            install_shutdown_handlers(request_shutdown);
            bind_socket_rpc_lane_views_bridge(lane_views);
            bind_socket_rpc_iv_module_instances_bridge(iv_module_instances);
            bind_socket_rpc_project_introspection_bridge(
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
            unbind_socket_rpc_project_introspection_bridge(
                introspection,
                graph_input_lanes);
            unbind_lane_views_timeline_bridge(lane_views);
            unbind_project_introspection_graph_input_lanes_bridge(graph_input_lanes);
            unbind_iv_module_reload_iv_module_definitions_bridge(iv_module_definitions);
            unbind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
            unbind_iv_module_definitions_project_introspection_bridge(introspection);
            unbind_iv_module_definitions_iv_module_reload_bridge(iv_module_reload);
            unbind_iv_module_definitions_iv_module_instances_bridge(iv_module_instances);
            unbind_iv_module_instances_iv_module_definitions_bridge(iv_module_definitions);
            unbind_graph_input_lanes_lane_views_bridge(graph_input_lanes);
            unbind_graph_input_lanes_timeline_bridge(timeline);
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
