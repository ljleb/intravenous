#include <intravenous/runtime/socket_rpc_iv_module_instances_bridge.h>

#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
namespace {
IvModuleInstances *bound_iv_module_instances = nullptr;

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

void handle_set_iv_module_instance_default_silence_ttl_samples(
    SetIvModuleInstanceDefaultSilenceTtlSamplesRequest const &request,
    SocketRpcAckResponseBuilder &)
{
    if (bound_iv_module_instances == nullptr) {
        return;
    }
    bound_iv_module_instances->set_default_silence_ttl_samples(
        request.instance_id,
        request.default_silence_ttl_samples);
}

IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcCreateIvModuleInstanceEvent,
    iv_socket_rpc_create_iv_module_instance_event,
    handle_create_iv_module_instance);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcDeleteIvModuleInstanceEvent,
    iv_socket_rpc_delete_iv_module_instance_event,
    handle_delete_iv_module_instance);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetIvModuleInstanceDefaultSilenceTtlSamplesEvent,
    iv_socket_rpc_set_iv_module_instance_default_silence_ttl_samples_event,
    handle_set_iv_module_instance_default_silence_ttl_samples);
} // namespace

void bind_socket_rpc_iv_module_instances_bridge(IvModuleInstances &iv_module_instances)
{
    bound_iv_module_instances = &iv_module_instances;
}

void unbind_socket_rpc_iv_module_instances_bridge(
    IvModuleInstances const &iv_module_instances)
{
    if (bound_iv_module_instances == &iv_module_instances) {
        bound_iv_module_instances = nullptr;
    }
}
} // namespace iv
