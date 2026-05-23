#include "runtime/socket_rpc_iv_module_instances_bridge.h"

#include "runtime/iv_module_instances.h"
#include "runtime/socket_rpc_server.h"

namespace iv {
namespace {
RuntimeIvModuleInstances *bound_iv_module_instances = nullptr;

void handle_create_iv_module_instance(
    CreateIvModuleInstanceRequest const &request,
    SocketRpcCreateIvModuleInstanceResultBuilder &builder)
{
    if (bound_iv_module_instances == nullptr) {
        return;
    }
    builder.succeed(bound_iv_module_instances->create_instance(request.module_root));
}

void handle_delete_iv_module_instance(
    DeleteIvModuleInstanceRequest const &request,
    SocketRpcAckResponseBuilder &)
{
    if (bound_iv_module_instances == nullptr) {
        return;
    }
    bound_iv_module_instances->remove_instance(request.instance_id);
}

IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcCreateIvModuleInstanceEvent,
    iv_socket_rpc_create_iv_module_instance_event,
    handle_create_iv_module_instance);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcDeleteIvModuleInstanceEvent,
    iv_socket_rpc_delete_iv_module_instance_event,
    handle_delete_iv_module_instance);
} // namespace

void bind_socket_rpc_iv_module_instances_bridge(RuntimeIvModuleInstances &iv_module_instances)
{
    bound_iv_module_instances = &iv_module_instances;
}

void unbind_socket_rpc_iv_module_instances_bridge(
    RuntimeIvModuleInstances const &iv_module_instances)
{
    if (bound_iv_module_instances == &iv_module_instances) {
        bound_iv_module_instances = nullptr;
    }
}
} // namespace iv
