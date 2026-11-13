#include <intravenous/runtime/socket_rpc_routes.h>

#include <intravenous/runtime/socket_rpc_response_builders.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;

std::string jsonrpc_result(int id, Json result)
{
    return Json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", std::move(result)},
    }.dump() + "\n";
}

std::string jsonrpc_error(int id, int code, std::string message)
{
    return Json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", code}, {"message", std::move(message)}}},
    }.dump() + "\n";
}

Json built_result(std::string const& response)
{
    auto parsed = Json::parse(response);
    if (auto const error = parsed.find("error"); error != parsed.end()) {
        auto const message = error->find("message");
        throw std::runtime_error(
            message != error->end() && message->is_string()
                ? message->get<std::string>()
                : "session snapshot query failed");
    }
    auto const result = parsed.find("result");
    if (result == parsed.end()) {
        throw std::runtime_error("session snapshot query produced no result");
    }
    return *result;
}
} // namespace

struct SocketRpcRouteRegistry::Impl {
    using Handler = std::function<std::string(
        void const*, int, SocketRpcRouteContext const&)>;

    struct Entry {
        std::string method{};
        std::string required_capability{};
        bool mutates_session = false;
        bool shutdown_after_response = false;
        Handler handler{};
    };

    std::unordered_map<std::type_index, Entry> entries{};
    std::unordered_set<std::string> methods{};

    template<class Request, class Function>
    void add(
        std::string method,
        std::string required_capability,
        bool mutates_session,
        bool shutdown_after_response,
        Function function)
    {
        if (!method.empty() && !methods.emplace(method).second) {
            throw std::logic_error(
                "duplicate socket RPC method registration: " + method);
        }
        auto [position, inserted] = entries.emplace(
            std::type_index(typeid(Request)),
            Entry{
                .method = std::move(method),
                .required_capability = std::move(required_capability),
                .mutates_session = mutates_session,
                .shutdown_after_response = shutdown_after_response,
                .handler = [function = std::move(function)](
                    void const* value,
                    int id,
                    SocketRpcRouteContext const& context) {
                    return function(*static_cast<Request const*>(value), id, context);
                },
            });
        if (!inserted) {
            throw std::logic_error(
                "duplicate socket RPC route payload registration: "
                + position->second.method);
        }
    }
};

SocketRpcRouteRegistry::SocketRpcRouteRegistry()
    : impl_(std::make_unique<Impl>())
{
    using Context = SocketRpcRouteContext;
    impl_->add<GraphQueryBySpansRequest>(
        "graph.queryBySpans", "project.read", false, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcGraphQueryResultBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_graph_query_by_spans_event, request, builder);
            if (!builder.has_response()) {
                builder.fail("iv module source introspection service is unavailable");
            }
            return builder.build(id);
        });
    impl_->add<GraphQueryActiveRegionsRequest>(
        "graph.queryActiveRegions", "project.read", false, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcRegionQueryResultBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_graph_query_active_regions_event, request, builder);
            if (!builder.has_response()) {
                builder.fail("iv module source introspection service is unavailable");
            }
            return builder.build(id);
        });
    impl_->add<GetVirtualNodeRequest>(
        "graph.getVirtualNode", "project.read", false, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcVirtualNodeResultBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_get_virtual_node_event, request, builder);
            if (!builder.has_response()) {
                builder.fail("iv module source introspection service is unavailable");
            }
            return builder.build(id);
        });
    impl_->add<GetVirtualNodesRequest>(
        "graph.getVirtualNodes", "project.read", false, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcVirtualNodesResultBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_get_virtual_nodes_event, request, builder);
            if (!builder.has_response()) {
                builder.fail("iv module source introspection service is unavailable");
            }
            return builder.build(id);
        });
    impl_->add<GetGraphScopeRequest>(
        "graph.getScope", "project.read", false, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcGraphScopeResultBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_get_graph_scope_event, request, builder);
            if (!builder.has_response()) builder.fail("project graph service is unavailable");
            return builder.build(id);
        });
    impl_->add<CreateNodeInstanceRequest>(
        "nodeInstances.create", "project.write", true, false,
        [](auto const& request, int id, Context const& context) {
            SocketRpcCreateNodeInstanceResultBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_create_node_instance_event, request, builder);
            if (!builder.has_response()) builder.fail("node instance service is unavailable");
            // The request's committed read-model changes go out after its response.
            if (context.publish_project_graph) {
                context.publish_project_graph(builder.take_publication());
            }
            return builder.build(id);
        });
    impl_->add<GetIvPackageDefinitionsRequest>(
        "ivPackages.list", "project.read", false, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcIvPackageDefinitionsResultBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_get_iv_package_definitions_event, request, builder);
            if (!builder.has_response()) builder.fail("IV package service is unavailable");
            return builder.build(id);
        });
    impl_->add<CreateIvPackageRequest>(
        "ivPackages.create", "project.write", true, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcIvPackageResultBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_create_iv_package_event, request, builder);
            if (!builder.has_response()) builder.fail("IV package service is unavailable");
            return builder.build(id);
        });
    impl_->add<GetNodeInstancesRequest>(
        "nodeInstances.get", "project.read", false, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcNodeInstancesResultBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_get_node_instances_event, request, builder);
            if (!builder.has_response()) builder.fail("node instance service is unavailable");
            return builder.build(id);
        });
    impl_->add<DeleteNodeInstanceRequest>(
        "nodeInstances.delete", "project.write", true, false,
        [](auto const& request, int id, Context const& context) {
            SocketRpcAckResponseBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_delete_node_instance_event, request, builder);
            if (!builder.has_response()) builder.fail("node instance service is unavailable");
            // The request's committed read-model changes go out after its response.
            if (context.publish_project_graph) {
                context.publish_project_graph(builder.take_publication());
            }
            return builder.build(id);
        });
    impl_->add<UpdateNodeInstancesRequest>(
        "nodeInstances.update", "project.write", true, false,
        [](auto const& request, int id, Context const& context) {
            SocketRpcAckResponseBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_update_node_instances_event, request, builder);
            if (!builder.has_response()) builder.fail("node instance service is unavailable");
            // The request's committed read-model changes go out after its response.
            if (context.publish_project_graph) {
                context.publish_project_graph(builder.take_publication());
            }
            return builder.build(id);
        });
    impl_->add<UpsertGraphConnectionRequest>(
        "graphConnections.upsert", "project.write", true, false,
        [](auto const& request, int id, Context const& context) {
            SocketRpcAckResponseBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_upsert_graph_connection_event, request, builder);
            if (!builder.has_response()) builder.fail("project graph service is unavailable");
            // The request's committed read-model changes go out after its response.
            if (context.publish_project_graph) {
                context.publish_project_graph(builder.take_publication());
            }
            return builder.build(id);
        });
    impl_->add<DeleteGraphConnectionRequest>(
        "graphConnections.delete", "project.write", true, false,
        [](auto const& request, int id, Context const& context) {
            SocketRpcAckResponseBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_delete_graph_connection_event, request, builder);
            if (!builder.has_response()) builder.fail("project graph service is unavailable");
            // The request's committed read-model changes go out after its response.
            if (context.publish_project_graph) {
                context.publish_project_graph(builder.take_publication());
            }
            return builder.build(id);
        });
    impl_->add<UpsertManualControlRequest>(
        "manualControls.upsert", "project.write", true, false,
        [](auto const& request, int id, Context const& context) {
            SocketRpcAckResponseBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_upsert_manual_control_event, request, builder);
            if (!builder.has_response()) builder.fail("project graph service is unavailable");
            // The request's committed read-model changes go out after its response.
            if (context.publish_project_graph) {
                context.publish_project_graph(builder.take_publication());
            }
            return builder.build(id);
        });
    impl_->add<DeleteManualControlRequest>(
        "manualControls.delete", "project.write", true, false,
        [](auto const& request, int id, Context const& context) {
            SocketRpcAckResponseBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_delete_manual_control_event, request, builder);
            if (!builder.has_response()) builder.fail("project graph service is unavailable");
            // The request's committed read-model changes go out after its response.
            if (context.publish_project_graph) {
                context.publish_project_graph(builder.take_publication());
            }
            return builder.build(id);
        });
    impl_->add<SetManualControlHoverRequest>(
        "manualControls.hover", "project.write", false, false,
        [](auto const& request, int id, Context const& context) {
            SocketRpcAckResponseBuilder builder;
            auto bound = request;
            bound.client_id = context.client_id;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_set_manual_control_hover_event, bound, builder);
            if (!builder.has_response()) builder.fail("project graph service is unavailable");
            // The request's committed read-model changes go out after its response.
            if (context.publish_project_graph) {
                context.publish_project_graph(builder.take_publication());
            }
            return builder.build(id);
        });
    impl_->add<BeginManualControlInteractionRequest>(
        "manualControls.begin", "project.write", true, false,
        [](auto const& request, int id, Context const& context) {
            SocketRpcAckResponseBuilder builder;
            auto bound = request;
            bound.interaction.client_id = context.client_id;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_begin_manual_control_interaction_event,
                bound,
                builder);
            if (!builder.has_response()) builder.fail("project graph service is unavailable");
            // The request's committed read-model changes go out after its response.
            if (context.publish_project_graph) {
                context.publish_project_graph(builder.take_publication());
            }
            return builder.build(id);
        });
    impl_->add<UpdateManualControlInteractionRequest>(
        "manualControls.update", "project.write", true, false,
        [](auto const& request, int id, Context const& context) {
            SocketRpcAckResponseBuilder builder;
            auto bound = request;
            bound.interaction.client_id = context.client_id;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_update_manual_control_interaction_event,
                bound,
                builder);
            if (!builder.has_response()) builder.fail("project graph service is unavailable");
            // The request's committed read-model changes go out after its response.
            if (context.publish_project_graph) {
                context.publish_project_graph(builder.take_publication());
            }
            return builder.build(id);
        });
    impl_->add<EndManualControlInteractionRequest>(
        "manualControls.end", "project.write", true, false,
        [](auto const& request, int id, Context const& context) {
            SocketRpcAckResponseBuilder builder;
            auto bound = request;
            bound.interaction.client_id = context.client_id;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_end_manual_control_interaction_event,
                bound,
                builder);
            if (!builder.has_response()) builder.fail("project graph service is unavailable");
            // The request's committed read-model changes go out after its response.
            if (context.publish_project_graph) {
                context.publish_project_graph(builder.take_publication());
            }
            return builder.build(id);
        });
    impl_->add<GetNodeEventProtocolRequest>(
        "nodeMessages.getProtocol", "project.read", false, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcNodeEventProtocolResultBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_get_node_event_protocol_event, request, builder);
            if (!builder.has_response()) builder.fail("project graph service is unavailable");
            return builder.build(id);
        });
    impl_->add<DispatchNodeMessageRequest>(
        "nodeMessages.dispatch", "project.write", true, false,
        [](auto const& request, int id, Context const& context) {
            SocketRpcNodeMessageResultBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_dispatch_node_message_event, request, builder);
            if (!builder.has_response()) builder.fail("project graph service is unavailable");
            // The request's committed read-model changes go out after its response.
            if (context.publish_project_graph) {
                context.publish_project_graph(builder.take_publication());
            }
            return builder.build(id);
        });
    impl_->add<GetAudioDevicesRequest>(
        "audioDevices.get", "project.read", false, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcAudioDevicesResultBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_get_audio_devices_event, request, builder);
            if (!builder.has_response()) builder.fail("audio device service is unavailable");
            return builder.build(id);
        });
    impl_->add<SetAudioDevicesRequest>(
        "audioDevices.set", "audio.control", true, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcAudioDevicesResultBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_set_audio_devices_event, request, builder);
            if (!builder.has_response()) builder.fail("audio device service is unavailable");
            return builder.build(id);
        });
    impl_->add<SaveProjectRequest>(
        "project.save", "project.write", false, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcAckResponseBuilder builder;
            IV_INVOKE_LINKER_EVENT(iv_socket_rpc_save_project_event, request, builder);
            if (!builder.has_response()) builder.fail("runtime project event was not handled");
            return builder.build(id);
        });
    impl_->add<EnableProjectAutosaveRequest>(
        "project.enableAutosave", "project.write", true, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcAckResponseBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_enable_project_autosave_event, request, builder);
            if (!builder.has_response()) builder.fail("project autosave service is unavailable");
            return builder.build(id);
        });
    impl_->add<DisableProjectAutosaveRequest>(
        "project.disableAutosave", "project.write", true, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcAckResponseBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_disable_project_autosave_event, request, builder);
            if (!builder.has_response()) builder.fail("project autosave service is unavailable");
            return builder.build(id);
        });
    impl_->add<SessionHelloRequest>(
        "session.hello", "", false, false,
        [](auto const& request, int id, Context const& context) {
            if (!context.hello) throw std::logic_error("session hello is unavailable");
            return context.hello(id, request);
        });
    impl_->add<SessionSubscribeRequest>(
        "session.subscribe", "project.read", false, false,
        [](auto const& request, int id, Context const& context) {
            if (!context.subscribe) throw std::logic_error("session subscribe is unavailable");
            return context.subscribe(id, request);
        });
    impl_->add<SessionSnapshotRequest>(
        "session.snapshot", "project.read", false, false,
        [](auto const& request, int id, Context const&) {
            SocketRpcSessionProjectStateResultBuilder project;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_get_session_project_state_event,
                request,
                project);
            if (!project.has_response()) {
                project.fail("project persistence service is unavailable");
            }

            SocketRpcAutosaveStateResultBuilder autosave;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_get_autosave_state_event,
                request,
                autosave);
            if (!autosave.has_response()) {
                autosave.fail("project autosave service is unavailable");
            }

            SocketRpcNodeInstancesResultBuilder instances;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_get_node_instances_event,
                GetNodeInstancesRequest{},
                instances);
            if (!instances.has_response()) instances.fail("node instance service is unavailable");

            SocketRpcIvPackageDefinitionsResultBuilder packages;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_get_iv_package_definitions_event,
                GetIvPackageDefinitionsRequest{},
                packages);
            if (!packages.has_response()) packages.fail("IV package service is unavailable");

            SocketRpcAudioDevicesResultBuilder audio;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_get_audio_devices_event,
                GetAudioDevicesRequest{},
                audio);
            if (!audio.has_response()) audio.fail("audio device service is unavailable");

            SocketRpcNodeEventProtocolResultBuilder events;
            IV_INVOKE_LINKER_EVENT(
                iv_socket_rpc_get_node_event_protocol_event,
                GetNodeEventProtocolRequest{},
                events);
            if (!events.has_response()) events.fail("project graph service is unavailable");

            return jsonrpc_result(id, Json{
                {"project", built_result(project.build(id))},
                {"autosave", built_result(autosave.build(id))},
                {"nodeInstances", built_result(instances.build(id))},
                {"packages", built_result(packages.build(id))},
                {"audioDevices", built_result(audio.build(id))},
                {"nodeEventProtocol", built_result(events.build(id))},
            });
        });
    impl_->add<ServerShutdownRequest>(
        "server.shutdown", "server.shutdown", false, true,
        [](auto const&, int id, Context const&) {
            return jsonrpc_result(id, Json{{"ok", true}});
        });
    impl_->add<UnsupportedSocketRpcRequest>(
        "", "", false, false,
        [](auto const& request, int id, Context const&) {
            return jsonrpc_error(
                id, -32601, "unsupported JSON-RPC method: " + request.method);
        });
}

SocketRpcRouteRegistry::~SocketRpcRouteRegistry() = default;

SocketRpcRouteResult SocketRpcRouteRegistry::dispatch(
    ParsedSocketRpcRequest const& request,
    SocketRpcRouteContext const& context) const
{
    return std::visit([&](auto const& payload) {
        auto const position = impl_->entries.find(std::type_index(typeid(payload)));
        if (position == impl_->entries.end()) {
            throw std::logic_error("socket RPC payload has no registered route");
        }
        auto const& entry = position->second;
        if (!entry.method.empty() && entry.method != request.method) {
            throw std::logic_error("socket RPC parser and route registry disagree");
        }
        if (!entry.method.empty()) {
            if (context.authorize) context.authorize(entry.required_capability);
            if (context.before_invoke) context.before_invoke(entry.mutates_session);
        }
        return SocketRpcRouteResult{
            .response = entry.handler(&payload, request.request_id, context),
            .mutates_session = entry.mutates_session,
            .shutdown_after_response = entry.shutdown_after_response,
        };
    }, request.payload);
}
} // namespace iv
