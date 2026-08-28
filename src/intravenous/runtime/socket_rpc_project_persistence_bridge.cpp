#include <intravenous/runtime/socket_rpc_project_persistence_bridge.h>

#include <intravenous/runtime/project_persistence.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_project_persistence_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_save_project_event,
    &ProjectPersistence::handle_socket_rpc_save_project);
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_create_iv_module_instance_event,
    &ProjectPersistence::handle_socket_rpc_create_iv_module_instance)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_delete_iv_module_instance_event,
    &ProjectPersistence::handle_socket_rpc_delete_iv_module_instance)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_update_iv_module_instances_event,
    &ProjectPersistence::handle_socket_rpc_update_iv_module_instances)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_set_timeline_compiled_sample_cache_chunk_size_multiplier_event,
    &ProjectPersistence::handle_socket_rpc_set_timeline_compiled_sample_cache_chunk_size_multiplier)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_set_timeline_lane_sample_channel_type_event,
    &ProjectPersistence::handle_socket_rpc_set_timeline_lane_sample_channel_type)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_set_timeline_lane_ui_state_event,
    &ProjectPersistence::handle_socket_rpc_set_timeline_lane_ui_state)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_connect_timeline_lanes_event,
    &ProjectPersistence::handle_socket_rpc_connect_timeline_lanes)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_disconnect_timeline_lanes_event,
    &ProjectPersistence::handle_socket_rpc_disconnect_timeline_lanes)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_get_timeline_lane_types_event,
    &ProjectPersistence::handle_socket_rpc_get_timeline_lane_types)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_create_timeline_lane_event,
    &ProjectPersistence::handle_socket_rpc_create_timeline_lane)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_delete_timeline_lane_event,
    &ProjectPersistence::handle_socket_rpc_delete_timeline_lane)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_duplicate_timeline_lane_event,
    &ProjectPersistence::handle_socket_rpc_duplicate_timeline_lane)
} // namespace iv
