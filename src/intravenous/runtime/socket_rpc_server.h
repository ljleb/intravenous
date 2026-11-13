#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/linker_event.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/runtime_project_api_types.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <intravenous/runtime/socket_rpc_response_builders.h>
#include <intravenous/sample.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace iv {
    struct PackageRefreshPublication;
    struct ProjectGraphPublication;

    using SocketRpcGraphQueryResult = ProjectQueryResult;
    using SocketRpcRegionQueryResult = ProjectRegionQueryResult;
    using SocketRpcServerMessage = ProjectMessageNotification;
    using SocketRpcServerStatus = ProjectStatusNotification;

    using SocketRpcGraphQueryBySpansEvent =
        void (*)(GraphQueryBySpansRequest const &, SocketRpcGraphQueryResultBuilder &);
    using SocketRpcGraphQueryActiveRegionsEvent =
        void (*)(GraphQueryActiveRegionsRequest const &, SocketRpcRegionQueryResultBuilder &);
    using SocketRpcGetVirtualNodeEvent =
        void (*)(GetVirtualNodeRequest const &, SocketRpcVirtualNodeResultBuilder &);
    using SocketRpcGetVirtualNodesEvent =
        void (*)(GetVirtualNodesRequest const &, SocketRpcVirtualNodesResultBuilder &);
    using SocketRpcGetGraphScopeEvent =
        void (*)(GetGraphScopeRequest const &, SocketRpcGraphScopeResultBuilder &);
    using SocketRpcCreateNodeInstanceEvent =
        void (*)(CreateNodeInstanceRequest const &, SocketRpcCreateNodeInstanceResultBuilder &);
    using SocketRpcGetIvPackageDefinitionsEvent =
        void (*)(GetIvPackageDefinitionsRequest const &, SocketRpcIvPackageDefinitionsResultBuilder &);
    using SocketRpcCreateIvPackageEvent =
        void (*)(CreateIvPackageRequest const &, SocketRpcIvPackageResultBuilder &);
    using SocketRpcGetNodeInstancesEvent =
        void (*)(GetNodeInstancesRequest const &, SocketRpcNodeInstancesResultBuilder &);
    using SocketRpcApplyProjectEditEvent = void (*)(ApplyProjectEditRequest const&, SocketRpcAckResponseBuilder&);
    using SocketRpcUndoProjectEditEvent = void (*)(UndoProjectEditRequest const&, SocketRpcAckResponseBuilder&);
    using SocketRpcRedoProjectEditEvent = void (*)(RedoProjectEditRequest const&, SocketRpcAckResponseBuilder&);
    using SocketRpcDeleteNodeInstanceEvent =
        void (*)(DeleteNodeInstanceRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcUpdateNodeInstancesEvent =
        void (*)(UpdateNodeInstancesRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcUpsertGraphConnectionEvent =
        void (*)(UpsertGraphConnectionRequest const&, SocketRpcAckResponseBuilder&);
    using SocketRpcDeleteGraphConnectionEvent =
        void (*)(DeleteGraphConnectionRequest const&, SocketRpcAckResponseBuilder&);
    using SocketRpcUpsertManualControlEvent =
        void (*)(UpsertManualControlRequest const&, SocketRpcAckResponseBuilder&);
    using SocketRpcDeleteManualControlEvent =
        void (*)(DeleteManualControlRequest const&, SocketRpcAckResponseBuilder&);
    using SocketRpcSetManualControlHoverEvent =
        void (*)(SetManualControlHoverRequest const&, SocketRpcAckResponseBuilder&);
    using SocketRpcBeginManualControlInteractionEvent = void (*)(
        BeginManualControlInteractionRequest const&, SocketRpcAckResponseBuilder&);
    using SocketRpcUpdateManualControlInteractionEvent = void (*)(
        UpdateManualControlInteractionRequest const&, SocketRpcAckResponseBuilder&);
    using SocketRpcEndManualControlInteractionEvent = void (*)(
        EndManualControlInteractionRequest const&, SocketRpcAckResponseBuilder&);
    using SocketRpcClientDisconnectedEvent = void (*)(std::string const&);
    using SocketRpcDispatchNodeMessageEvent =
        void (*)(DispatchNodeMessageRequest const&, SocketRpcNodeMessageResultBuilder&);
    using SocketRpcGetNodeEventProtocolEvent =
        void (*)(GetNodeEventProtocolRequest const&,
                 SocketRpcNodeEventProtocolResultBuilder&);
    using SocketRpcGetAudioDevicesEvent =
        void (*)(GetAudioDevicesRequest const &, SocketRpcAudioDevicesResultBuilder &);
    using SocketRpcSetAudioDevicesEvent =
        void (*)(SetAudioDevicesRequest const &, SocketRpcAudioDevicesResultBuilder &);
    using SocketRpcSaveProjectEvent =
        void (*)(SaveProjectRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcEnableProjectAutosaveEvent =
        void (*)(EnableProjectAutosaveRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcDisableProjectAutosaveEvent =
        void (*)(DisableProjectAutosaveRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcGetSessionProjectStateEvent =
        void (*)(SessionSnapshotRequest const&,
                 SocketRpcSessionProjectStateResultBuilder&);
    using SocketRpcGetAutosaveStateEvent =
        void (*)(SessionSnapshotRequest const&,
                 SocketRpcAutosaveStateResultBuilder&);

    IV_DECLARE_LINKER_EVENT(SocketRpcGraphQueryBySpansEvent, iv_socket_rpc_graph_query_by_spans_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGraphQueryActiveRegionsEvent, iv_socket_rpc_graph_query_active_regions_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetVirtualNodeEvent, iv_socket_rpc_get_virtual_node_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetVirtualNodesEvent, iv_socket_rpc_get_virtual_nodes_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetGraphScopeEvent, iv_socket_rpc_get_graph_scope_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcCreateNodeInstanceEvent, iv_socket_rpc_create_node_instance_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetIvPackageDefinitionsEvent, iv_socket_rpc_get_iv_package_definitions_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcCreateIvPackageEvent, iv_socket_rpc_create_iv_package_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetNodeInstancesEvent, iv_socket_rpc_get_node_instances_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcApplyProjectEditEvent, iv_socket_rpc_apply_project_edit_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcUndoProjectEditEvent, iv_socket_rpc_undo_project_edit_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcRedoProjectEditEvent, iv_socket_rpc_redo_project_edit_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcDeleteNodeInstanceEvent, iv_socket_rpc_delete_node_instance_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcUpdateNodeInstancesEvent, iv_socket_rpc_update_node_instances_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcUpsertGraphConnectionEvent, iv_socket_rpc_upsert_graph_connection_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcDeleteGraphConnectionEvent, iv_socket_rpc_delete_graph_connection_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcUpsertManualControlEvent, iv_socket_rpc_upsert_manual_control_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcDeleteManualControlEvent, iv_socket_rpc_delete_manual_control_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcSetManualControlHoverEvent, iv_socket_rpc_set_manual_control_hover_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcBeginManualControlInteractionEvent, iv_socket_rpc_begin_manual_control_interaction_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcUpdateManualControlInteractionEvent, iv_socket_rpc_update_manual_control_interaction_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcEndManualControlInteractionEvent, iv_socket_rpc_end_manual_control_interaction_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcClientDisconnectedEvent, iv_socket_rpc_client_disconnected_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcDispatchNodeMessageEvent, iv_socket_rpc_dispatch_node_message_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetNodeEventProtocolEvent, iv_socket_rpc_get_node_event_protocol_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetAudioDevicesEvent, iv_socket_rpc_get_audio_devices_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcSetAudioDevicesEvent, iv_socket_rpc_set_audio_devices_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcSaveProjectEvent, iv_socket_rpc_save_project_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcEnableProjectAutosaveEvent, iv_socket_rpc_enable_project_autosave_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcDisableProjectAutosaveEvent, iv_socket_rpc_disable_project_autosave_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetSessionProjectStateEvent, iv_socket_rpc_get_session_project_state_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetAutosaveStateEvent, iv_socket_rpc_get_autosave_state_event);

    class SocketRpcServer {
        struct Impl;
        std::unique_ptr<Impl> impl_;

    public:
        explicit SocketRpcServer(
            std::filesystem::path workspace_root,
            int initial_rpc_fd = -1);
        ~SocketRpcServer();
        SocketRpcServer(SocketRpcServer &&) = delete;
        SocketRpcServer &operator=(SocketRpcServer &&) = delete;

        SocketRpcServer(SocketRpcServer const &) = delete;
        SocketRpcServer &operator=(SocketRpcServer const &) = delete;

        void start();
        void attach_client(int connected_fd, bool trusted = true);
        bool wait_until_ready(std::chrono::milliseconds timeout) const;
        void wait();
        void request_shutdown();

        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] std::size_t client_count() const;
        [[nodiscard]] std::optional<std::filesystem::path> endpoint() const;

        void send_server_message(SocketRpcServerMessage const &notification);
        void send_server_status(SocketRpcServerStatus const &notification);
        void send_node_instances_updated(
            std::vector<IvModuleInstanceInfo> const &instances);
        void send_iv_package_definitions_updated();
        void send_virtual_nodes_updated(
            ProjectVirtualNodesNotification const &notification);
        // The read-model changes a project-graph change committed: the
        // instance list and the virtual-node replacements.
        void send_project_graph_publication(
            ProjectGraphPublication const &publication);
        void handle_project_notification(ProjectNotification const &notification);
        // The one report of a package refresh.
        void handle_package_refresh_published(
            PackageRefreshPublication const &publication);
    };
} // namespace iv
