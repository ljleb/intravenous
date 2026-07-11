#include <intravenous/runtime/socket_rpc_iv_module_instances_bridge.h>

#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/iv_module_sources.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <algorithm>
#include <system_error>
#include <unordered_map>

namespace iv {
namespace {
IvModuleInstances *bound_iv_module_instances = nullptr;
IvModuleSourceIntrospection *bound_introspection = nullptr;
IvModuleSources *bound_sources = nullptr;

std::string normalized_path_key(std::filesystem::path const &path)
{
    std::error_code error;
    auto const canonical = std::filesystem::weakly_canonical(path, error);
    return (error ? std::filesystem::absolute(path) : canonical).lexically_normal().string();
}

void populate_module_ids(std::vector<IvModuleInstanceInfo> &instances)
{
    if (bound_sources == nullptr) return;
    std::unordered_map<std::string, std::string> module_ids;
    for (auto const &source : bound_sources->list_sources()) {
        module_ids.emplace(normalized_path_key(source.module_root), source.module_id);
    }
    for (auto &instance : instances) {
        if (instance.module_id.empty()) {
            if (auto source = module_ids.find(normalized_path_key(instance.module_root));
                source != module_ids.end()) {
                instance.module_id = source->second;
            }
        }
    }
}

void handle_get_iv_module_sources(
    GetIvModuleSourcesRequest const &,
    SocketRpcIvModuleSourcesResultBuilder &builder)
{
    if (bound_sources == nullptr) return;
    builder.succeed(bound_sources->list_sources());
}

void handle_create_iv_module_source(
    CreateIvModuleSourceRequest const &request,
    SocketRpcIvModuleSourceResultBuilder &builder)
{
    if (bound_sources == nullptr) return;
    try {
        builder.succeed(bound_sources->create_project_source(request.name));
    } catch (std::exception const &exception) {
        builder.fail(exception.what());
    }
}

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
void handle_get_iv_module_instances(
    GetIvModuleInstancesRequest const &request,
    SocketRpcIvModuleInstancesResultBuilder &builder)
{
    if (bound_iv_module_instances == nullptr) {
        return;
    }
    auto instances = bound_iv_module_instances->list_instances();
    populate_module_ids(instances);
    if (request.source_file_path.has_value() && bound_introspection != nullptr) {
        std::erase_if(instances, [&](IvModuleInstanceInfo const &instance) {
            return !bound_introspection->definition_uses_source_file(
                instance.definition_id,
                *request.source_file_path);
        });
    }
    builder.succeed(std::move(instances));
}

IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcCreateIvModuleInstanceEvent,
    iv_socket_rpc_create_iv_module_instance_event,
    handle_create_iv_module_instance);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcGetIvModuleSourcesEvent,
    iv_socket_rpc_get_iv_module_sources_event,
    handle_get_iv_module_sources);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcCreateIvModuleSourceEvent,
    iv_socket_rpc_create_iv_module_source_event,
    handle_create_iv_module_source);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcDeleteIvModuleInstanceEvent,
    iv_socket_rpc_delete_iv_module_instance_event,
    handle_delete_iv_module_instance);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetIvModuleInstanceDefaultSilenceTtlSamplesEvent,
    iv_socket_rpc_set_iv_module_instance_default_silence_ttl_samples_event,
    handle_set_iv_module_instance_default_silence_ttl_samples);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcGetIvModuleInstancesEvent,
    iv_socket_rpc_get_iv_module_instances_event,
    handle_get_iv_module_instances);
} // namespace

void bind_socket_rpc_iv_module_instances_bridge(
    IvModuleInstances &iv_module_instances,
    IvModuleSourceIntrospection &introspection,
    IvModuleSources &sources)
{
    bound_iv_module_instances = &iv_module_instances;
    bound_introspection = &introspection;
    bound_sources = &sources;
}

void unbind_socket_rpc_iv_module_instances_bridge(
    IvModuleInstances const &iv_module_instances,
    IvModuleSourceIntrospection const &introspection,
    IvModuleSources const &sources)
{
    if (bound_iv_module_instances == &iv_module_instances) {
        bound_iv_module_instances = nullptr;
    }
    if (bound_introspection == &introspection) {
        bound_introspection = nullptr;
    }
    if (bound_sources == &sources) {
        bound_sources = nullptr;
    }
}
} // namespace iv
