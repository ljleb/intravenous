#include <intravenous/runtime/app.h>

#include <intravenous/compat.h>
#include <intravenous/devices/miniaudio_device.h>
#include <intravenous/juce/vst_runtime.h>
#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/graph_executor.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/handlers.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/node_definitions_project_graph_bridge.h>
#include <intravenous/runtime/package_definitions_node_definitions_bridge.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/node_instances_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/package_definitions.h>
#include <intravenous/runtime/package_jit.h>
#include <intravenous/runtime/package_watcher.h>
#include <intravenous/runtime/package_watcher_package_definitions_bridge.h>
#include <intravenous/runtime/package_watcher_package_jit_bridge.h>
#include <intravenous/runtime/package_watcher_node_instances_bridge.h>
#include <intravenous/runtime/package_watcher_service.h>
#include <intravenous/runtime/package_watcher_service_bridge.h>
#include <intravenous/runtime/project_autosave.h>
#include <intravenous/runtime/project_persistence.h>
#include <intravenous/runtime/project_persistence_node_instances_bridge.h>
#include <intravenous/runtime/project_persistence_graph_connections_bridge.h>
#include <intravenous/runtime/project_persistence_project_graph_bridge.h>
#include <intravenous/runtime/project_persistence_package_jit_bridge.h>
#include <intravenous/runtime/project_persistence_project_autosave_bridge.h>
#include <intravenous/runtime/project_persistence_system_audio_devices_bridge.h>
#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/project_graph_graph_connections_bridge.h>
#include <intravenous/runtime/project_graph_graph_executor_bridge.h>
#include <intravenous/runtime/project_graph_graph_jit_bridge.h>
#include <intravenous/runtime/project_graph_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/project_graph_node_instances_bridge.h>
#include <intravenous/runtime/project_graph_package_definitions_bridge.h>
#include <intravenous/runtime/server_options.h>
#include <intravenous/runtime/socket_rpc_node_instances_bridge.h>
#include <intravenous/runtime/socket_rpc_project_graph_bridge.h>
#include <intravenous/runtime/socket_rpc_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/socket_rpc_package_definitions_bridge.h>
#include <intravenous/runtime/socket_rpc_package_watcher_bridge.h>
#include <intravenous/runtime/socket_rpc_project_autosave_bridge.h>
#include <intravenous/runtime/socket_rpc_project_persistence_bridge.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/socket_rpc_system_audio_devices_bridge.h>
#include <intravenous/runtime/startup_config.h>
#include <intravenous/runtime/system_audio_devices.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace iv {
namespace {
std::function<void()>* shutdown_callback = nullptr;

class ScopedShutdownCallback {
    std::function<void()>* previous_ = nullptr;

public:
    explicit ScopedShutdownCallback(std::function<void()>& callback)
        : previous_(std::exchange(shutdown_callback, &callback))
    {}

    ~ScopedShutdownCallback() { shutdown_callback = previous_; }

    ScopedShutdownCallback(ScopedShutdownCallback const&) = delete;
    ScopedShutdownCallback& operator=(ScopedShutdownCallback const&) = delete;
};

class ProjectAutosaveService {
    ProjectAutosave* autosave_ = nullptr;
    ProjectPersistence* persistence_ = nullptr;
    std::optional<std::jthread> thread_ {};

public:
    ProjectAutosaveService(ProjectAutosave& autosave, ProjectPersistence& persistence)
        : autosave_(&autosave)
        , persistence_(&persistence)
    {}

    // One autosave attempt, including its failure report, is one event source
    // entry: the save completes work scheduled by earlier project changes.
    bool save_once()
    {
        bool saved = false;
        IV_EVENT_SOURCE(ProjectAutosave, "autosave") {
            try {
                persistence_->save();
                autosave_->save_succeeded();
                saved = true;
            } catch (std::exception const& exception) {
                autosave_->save_failed();
                persistence_->report_autosave_failure(exception.what());
            } catch (...) {
                autosave_->save_failed();
                persistence_->report_autosave_failure("unknown failure");
            }
        }
        return saved;
    }

    void start()
    {
        if (thread_.has_value()) return;
        thread_.emplace([this](std::stop_token stop_token) {
            while (!stop_token.stop_requested()) {
                if (autosave_->take_due_save()) (void)save_once();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

    void request_shutdown()
    {
        if (thread_.has_value()) thread_->request_stop();
    }

    void stop()
    {
        request_shutdown();
        thread_.reset();
        while (autosave_->take_pending_save()) {
            if (!save_once()) break;
        }
    }
};

class GraphExecutorAudioAttachment {
    SystemAudioDevices* devices_ = nullptr;
    GraphExecutor* executor_ = nullptr;

public:
    GraphExecutorAudioAttachment(
        SystemAudioDevices& devices,
        GraphExecutor& executor) noexcept
        : devices_(&devices)
        , executor_(&executor)
    {
        devices_->attach_graph_executor(*executor_);
    }

    ~GraphExecutorAudioAttachment() { stop(); }

    GraphExecutorAudioAttachment(GraphExecutorAudioAttachment const&) = delete;
    GraphExecutorAudioAttachment& operator=(
        GraphExecutorAudioAttachment const&) = delete;

    void stop() noexcept
    {
        if (!devices_) return;
        devices_->detach_graph_executor(*executor_);
        devices_->stop();
        devices_ = nullptr;
        executor_ = nullptr;
    }
};

class RuntimeExecutionService {
    enum class Failure : std::uint8_t {
        none,
        system_audio,
        realtime_production,
        capacity_allocation,
        capacity_unexpected,
        background_execution,
    };

    SystemAudioDevices* devices_ = nullptr;
    GraphExecutor* executor_ = nullptr;
    std::function<void()> request_shutdown_{};
    std::atomic<Failure> failure_ = Failure::none;
    std::optional<std::jthread> thread_{};

    [[nodiscard]] Failure current_failure() const noexcept
    {
        if (devices_->output_render_failed()) {
            return Failure::system_audio;
        }
        if (executor_->realtime_production_failures().any()) {
            return Failure::realtime_production;
        }
        auto const capacity =
            executor_->realtime_capacity_maintenance_failures();
        if (capacity.allocation_failed) {
            return Failure::capacity_allocation;
        }
        if (capacity.unexpected_failure) {
            return Failure::capacity_unexpected;
        }
        if (executor_->background_execution_failures().any()) {
            return Failure::background_execution;
        }
        return Failure::none;
    }

public:
    RuntimeExecutionService(
        SystemAudioDevices& devices,
        GraphExecutor& executor,
        std::function<void()> request_shutdown)
        : devices_(&devices)
        , executor_(&executor)
        , request_shutdown_(std::move(request_shutdown))
    {
    }

    ~RuntimeExecutionService() { stop(); }

    void start()
    {
        if (thread_) return;
        thread_.emplace([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                auto const failure = current_failure();
                if (failure != Failure::none) {
                    failure_.store(failure, std::memory_order_release);
                    request_shutdown_();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }

    void stop()
    {
        if (!thread_) return;
        thread_->request_stop();
        thread_.reset();
    }

    void throw_if_failed() const
    {
        auto failure = failure_.load(std::memory_order_acquire);
        if (failure == Failure::none) failure = current_failure();
        switch (failure) {
        case Failure::none:
            return;
        case Failure::system_audio:
            throw std::runtime_error("system audio rendering failed");
        case Failure::realtime_production:
            throw std::runtime_error(
                "realtime execution exhausted mandatory producer storage");
        case Failure::capacity_allocation:
            throw std::runtime_error(
                "realtime producer capacity allocation failed");
        case Failure::capacity_unexpected:
            throw std::runtime_error(
                "realtime producer capacity maintenance failed");
        case Failure::background_execution:
            throw std::runtime_error("background graph execution failed");
        }
        std::terminate();
    }
};

void startup_log(std::string const& message)
{
    std::cerr << "[intravenous startup] " << message << '\n';
}

void request_shutdown()
{
    if (shutdown_callback) (*shutdown_callback)();
}

int run_server_mode(int argc, char** argv)
{
    startup_log("parsing server options");
    auto const options = ServerOptions::parse(argc, argv);
    StartupConfig startup_config(options.workspace_root, std::filesystem::current_path());
    auto const startup = startup_config.initialize();

    // App modules are constructed independently. Cross-module behavior is
    // expressed only by explicit bridges below; none of these constructors
    // retains another app module.
    NodeInstances node_instances;
    NodeDefinitions node_definitions;
    ProjectGraph project_graph;
    GraphConnections graph_connections;
    GraphJit graph_jit(GraphJitConfig{
        .sample_rate = startup.execution.sample_rate,
        .block_size = startup.execution.block_size,
    });
    PackageWatcher package_watcher;
    PackageJit package_jit(startup);
    PackageDefinitions package_definitions(startup.workspace_root);
    IvModuleSourceIntrospection introspection;
#if IV_ENABLE_JUCE_VST
    JuceVstRuntimeManager vst_runtime_manager;
#endif
    SystemAudioDevices system_audio_devices(
        startup.execution.sample_rate,
        startup.execution.block_size,
        SystemAudioDevicesBackend{
            .list_output_devices = [] { return list_miniaudio_output_devices(); },
            .list_input_devices = [] { return list_miniaudio_input_devices(); },
            .make_output_device = [](std::string const& device_id, RenderConfig const& config) {
                return make_miniaudio_output_device(config, device_id);
            },
            .make_input_device = [](std::string const& device_id, RenderConfig const& config) {
                return make_miniaudio_input_device(config, device_id);
            },
        },
        startup.output_device_id,
        startup.input_device_id);
    auto execution_resources = system_audio_devices.resources();
#if IV_ENABLE_JUCE_VST
    JuceVstRuntimeSupport vst_runtime_support(
        vst_runtime_manager,
        static_cast<double>(startup.execution.sample_rate));
    execution_resources.vst = vst_runtime_support.resources().vst;
#endif
    GraphExecutor graph_executor(std::move(execution_resources));
    GraphExecutorAudioAttachment graph_executor_audio_attachment(
        system_audio_devices,
        graph_executor);
    ProjectPersistence project_persistence(startup.workspace_root, startup);
    ProjectAutosave project_autosave;
    SocketRpcServer server(options.workspace_root, options.rpc_fd);

    // Services own process-level loops only; durable domain state remains in
    // their corresponding app modules.
    ProjectAutosaveService project_autosave_service(project_autosave, project_persistence);
    PackageWatcherService package_watcher_service(
        package_watcher,
        startup.workspace_root,
        startup.package_roots);

    std::function<void()> shutdown = [&] {
        package_watcher_service.request_shutdown();
        project_autosave_service.request_shutdown();
        system_audio_devices.request_shutdown();
        server.request_shutdown();
    };
    ScopedShutdownCallback shutdown_callback_scope(shutdown);
    install_shutdown_handlers(request_shutdown);
    RuntimeExecutionService runtime_execution_service(
        system_audio_devices,
        graph_executor,
        shutdown);

    // Runtime bridges.
    auto definitions_project_graph_scope =
        node_definitions_project_graph_bridge::bind(node_definitions, project_graph);
    auto project_graph_instances_scope =
        project_graph_node_instances_bridge::bind(project_graph, node_instances);
    auto project_graph_packages_scope =
        project_graph_package_definitions_bridge::bind(project_graph, package_definitions);
    auto project_graph_connections_scope =
        project_graph_graph_connections_bridge::bind(project_graph, graph_connections);
    auto project_graph_graph_jit_scope =
        project_graph_graph_jit_bridge::bind(project_graph, graph_jit);
    auto project_graph_graph_executor_scope =
        project_graph_graph_executor_bridge::bind(project_graph, graph_executor);
    auto project_graph_introspection_scope =
        project_graph_iv_module_source_introspection_bridge::bind(
            project_graph, introspection);
    auto watcher_service_scope =
        package_watcher_service_bridge::bind(package_watcher_service, package_watcher);
    auto watcher_jit_scope =
        package_watcher_package_jit_bridge::bind(package_watcher, package_jit);
    auto watcher_packages_scope =
        package_watcher_package_definitions_bridge::bind(package_watcher, package_definitions);
    auto packages_definitions_scope =
        package_definitions_node_definitions_bridge::bind(package_definitions, node_definitions);
    auto watcher_instances_scope =
        package_watcher_node_instances_bridge::bind(package_watcher, node_instances);
    auto instances_introspection_scope =
        node_instances_iv_module_source_introspection_bridge::bind(node_instances, introspection);

    // Persistence bridges.
    auto persistence_project_graph_scope =
        project_persistence_project_graph_bridge::bind(project_persistence, project_graph);
    auto persistence_instances_scope =
        project_persistence_node_instances_bridge::bind(project_persistence, node_instances);
    auto persistence_connections_scope =
        project_persistence_graph_connections_bridge::bind(
            project_persistence, graph_connections);
    auto persistence_jit_scope =
        project_persistence_package_jit_bridge::bind(project_persistence, package_jit);
    auto persistence_audio_scope =
        project_persistence_system_audio_devices_bridge::bind(project_persistence, system_audio_devices);
    auto persistence_autosave_scope =
        project_persistence_project_autosave_bridge::bind(project_persistence, project_autosave);

    // RPC bridges.
    auto rpc_audio_scope =
        socket_rpc_system_audio_devices_bridge::bind(server, system_audio_devices);
    auto rpc_project_graph_scope =
        socket_rpc_project_graph_bridge::bind(server, project_graph);
    auto rpc_instances_scope =
        socket_rpc_node_instances_bridge::bind(server, node_instances);
    auto rpc_packages_scope =
        socket_rpc_package_definitions_bridge::bind(server, package_definitions);
    auto rpc_package_watcher_scope =
        socket_rpc_package_watcher_bridge::bind(server, package_watcher);
    auto rpc_introspection_scope =
        socket_rpc_iv_module_source_introspection_bridge::bind(server, introspection);
    auto rpc_persistence_scope =
        socket_rpc_project_persistence_bridge::bind(server, project_persistence);
    auto rpc_autosave_scope =
        socket_rpc_project_autosave_bridge::bind(server, project_autosave);

    startup_log("loading project persistence");
    // No client is attached during replay, so its messages go to the log.
    for (auto const& message : project_persistence.load()) {
        diagnostic_stream() << "project replay " << message.level << ": "
                            << message.message << '\n';
    }
    flush_diagnostic_stream();
    project_autosave_service.start();
    runtime_execution_service.start();

    startup_log("starting socket rpc server");
    server.start();
    if (!server.wait_until_ready(std::chrono::seconds(10))) {
        throw std::runtime_error("socket rpc server did not become ready");
    }

    startup_log("starting IV package watcher");
    package_watcher_service.start();
    if (auto const endpoint = server.endpoint()) {
        std::cout << "Intravenous project session listening on "
                  << endpoint->string() << '\n';
    } else {
        std::cout << "Intravenous server connected on rpc fd "
                  << options.rpc_fd << '\n';
    }
    server.wait();

    package_watcher_service.request_shutdown();
    project_autosave_service.stop();
    runtime_execution_service.stop();
    graph_executor_audio_attachment.stop();
    runtime_execution_service.throw_if_failed();
    return 0;
}
} // namespace

int run_intravenous_cli(int argc, char** argv)
{
    install_crash_handlers();
#if IV_ENABLE_JUCE_VST
    warmup_juce_vst_scan_cache();
#endif
    if (argc >= 2 && std::string_view(argv[1]) == "--server") {
        return run_server_mode(argc, argv);
    }
    throw std::runtime_error("intravenous runs as a server; use --server --workspace-root <path>");
}
} // namespace iv
