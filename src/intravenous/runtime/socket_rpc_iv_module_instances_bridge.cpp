#include <intravenous/runtime/socket_rpc_iv_module_instances_bridge.h>

#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
namespace {
void handle_create_iv_module_instance(
    CreateIvModuleInstanceRequest const &request,
    SocketRpcCreateIvModuleInstanceResultBuilder &builder)
{
    try {
        ProjectStringBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_create_iv_module_instance_requested_event,
            ProjectCreateIvModuleInstanceRequest{
                .module_root = request.module_root,
            },
            project_builder);
        builder.succeed(project_builder.build());
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

void handle_delete_iv_module_instance(
    DeleteIvModuleInstanceRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_delete_iv_module_instance_requested_event,
            ProjectDeleteIvModuleInstanceRequest{
                .instance_id = request.instance_id,
            },
            project_builder);
        project_builder.build();
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

void handle_set_iv_module_instance_default_silence_ttl_samples(
    SetIvModuleInstanceDefaultSilenceTtlSamplesRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        ProjectAckBuilder project_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_iv_module_instance_default_silence_ttl_samples_requested_event,
            ProjectSetIvModuleInstanceDefaultSilenceTtlSamplesRequest{
                .instance_id = request.instance_id,
                .default_silence_ttl_samples = request.default_silence_ttl_samples,
            },
            project_builder);
        project_builder.build();
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
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
    (void)iv_module_instances;
}

void unbind_socket_rpc_iv_module_instances_bridge(
    IvModuleInstances const &iv_module_instances)
{
    (void)iv_module_instances;
}
} // namespace iv
