#include "runtime/iv_module_instances.h"

#include "runtime/iv_module_instances_events.h"

namespace iv {
namespace {
RuntimeIvModuleInstance make_instance_from_definition(
    RuntimeIvModuleDefinition const &definition)
{
    RuntimeIvModuleInstance instance{};
    instance.instance_id = definition.definition_id;
    instance.definition_id = definition.definition_id;
    instance.module_root = definition.module_root;
    instance.module_id = definition.module_id;
    instance.introspection = definition.introspection;
    return instance;
}
} // namespace

void RuntimeIvModuleInstances::handle_iv_module_definitions_changed(
    RuntimeIvModuleDefinitionsChanged const &diff)
{
    RuntimeIvModuleInstancesChanged instance_diff{};

    std::scoped_lock lock(mutex);
    for (auto const &definition : diff.created) {
        auto instance = make_instance_from_definition(definition);
        instances_by_definition_id[definition.definition_id] = instance;
        instance_diff.created.push_back(std::move(instance));
    }
    for (auto const &definition : diff.updated) {
        auto instance = make_instance_from_definition(definition);
        instances_by_definition_id[definition.definition_id] = instance;
        instance_diff.updated.push_back(std::move(instance));
    }
    for (auto const &definition_id : diff.deleted_definition_ids) {
        if (auto existing = instances_by_definition_id.find(definition_id);
            existing != instances_by_definition_id.end()) {
            instance_diff.deleted_instance_ids.push_back(existing->second.instance_id);
            instances_by_definition_id.erase(existing);
        }
    }

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_instances_changed_event,
        instance_diff);
}
} // namespace iv
