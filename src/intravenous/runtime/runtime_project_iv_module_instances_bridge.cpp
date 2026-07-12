#include <intravenous/runtime/runtime_project_iv_module_instances_bridge.h>

#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_sources.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <stdexcept>

namespace iv {
namespace {
IvModuleInstances *bound_iv_module_instances = nullptr;
IvModuleSources *bound_sources = nullptr;

void handle_create_iv_module_instance(
    ProjectCreateIvModuleInstanceRequest const &request,
    ProjectStringBuilder &builder)
{
    if (bound_iv_module_instances == nullptr || bound_sources == nullptr) {
        return;
    }
    auto const source = bound_sources->find_source(request.module_id);
    if (!source.has_value()) {
        throw std::runtime_error("unknown iv module source: " + request.module_id);
    }
    builder.succeed(bound_iv_module_instances->create_instance(
        source->module_id,
        source->module_root,
        request.instance_id,
        request.display_name));
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_delete_iv_module_instance(
    ProjectDeleteIvModuleInstanceRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_iv_module_instances == nullptr) {
        return;
    }
    bound_iv_module_instances->remove_instance(request.instance_id);
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_update_iv_module_instances(
    ProjectUpdateIvModuleInstancesRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_iv_module_instances == nullptr) {
        return;
    }
    std::vector<IvModuleInstances::Update> updates;
    updates.reserve(request.updates.size());
    for (auto const &update : request.updates) {
        updates.push_back(IvModuleInstances::Update{
            .instance_id = update.instance_id,
            .display_name = update.display_name,
            .default_silence_ttl_samples = update.default_silence_ttl_samples,
        });
    }
    bound_iv_module_instances->update_instances(std::move(updates));
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

IV_SUBSCRIBE_LINKER_EVENT(
    ProjectCreateIvModuleInstanceRequestedEvent,
    iv_runtime_project_create_iv_module_instance_requested_event,
    handle_create_iv_module_instance);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectDeleteIvModuleInstanceRequestedEvent,
    iv_runtime_project_delete_iv_module_instance_requested_event,
    handle_delete_iv_module_instance);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectUpdateIvModuleInstancesRequestedEvent,
    iv_runtime_project_update_iv_module_instances_requested_event,
    handle_update_iv_module_instances);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectPersistenceCollectStateEvent,
    iv_runtime_project_persistence_collect_state_event,
    [](ProjectPersistenceBuilder &builder) {
        if (bound_iv_module_instances == nullptr) {
            return;
        }
        builder.add_iv_module_instances(bound_iv_module_instances->list_instances());
    });
} // namespace

void bind_runtime_project_iv_module_instances_bridge(
    IvModuleInstances &iv_module_instances,
    IvModuleSources &sources)
{
    bound_iv_module_instances = &iv_module_instances;
    bound_sources = &sources;
}

void unbind_runtime_project_iv_module_instances_bridge(
    IvModuleInstances const &iv_module_instances,
    IvModuleSources const &sources)
{
    if (bound_iv_module_instances == &iv_module_instances) {
        bound_iv_module_instances = nullptr;
    }
    if (bound_sources == &sources) {
        bound_sources = nullptr;
    }
}
} // namespace iv
