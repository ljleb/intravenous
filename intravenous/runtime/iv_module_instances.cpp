#include "runtime/iv_module_instances.h"

#include "runtime/iv_module_instances_events.h"

#include <algorithm>
#include <ranges>
#include <system_error>

namespace iv {
namespace {
std::filesystem::path normalize_path(std::filesystem::path const &path)
{
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical.lexically_normal();
    }
    return std::filesystem::absolute(path).lexically_normal();
}

std::string definition_id_for(std::filesystem::path const &module_root)
{
    return normalize_path(module_root).string();
}

RuntimeIvModuleInstance make_instance_from_definition(
    RuntimeIvModuleDefinition const &definition,
    std::string const &instance_id)
{
    RuntimeIvModuleInstance instance{};
    instance.instance_id = instance_id;
    instance.definition_id = definition.definition_id;
    instance.module_root = definition.module_root;
    instance.module_id = definition.module_id;
    instance.introspection = definition.introspection;
    return instance;
}
} // namespace

std::string RuntimeIvModuleInstances::create_instance(std::filesystem::path module_root)
{
    RuntimeIvModuleRequiredDefinitionsChanged required_diff{};
    bool list_changed = false;
    auto normalized_root = normalize_path(module_root);
    auto definition_id = definition_id_for(normalized_root);
    std::string instance_id{};

    {
        std::scoped_lock lock(mutex);
        instance_id = "instance:" + std::to_string(next_instance_id++);
        desired_instances_by_id.emplace(instance_id, DesiredInstance{
            .instance_id = instance_id,
            .definition_id = definition_id,
            .module_root = normalized_root,
        });
        list_changed = true;

        if (!required_definitions_by_id.contains(definition_id)) {
            RuntimeIvModuleRequiredDefinition required{
                .definition_id = definition_id,
                .module_root = normalized_root,
            };
            required_definitions_by_id.emplace(definition_id, required);
            required_diff.created.push_back(std::move(required));
        }
    }

    if (!required_diff.created.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_required_definitions_changed_event,
            required_diff);
    }
    if (list_changed) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instances_list_changed_event,
            list_instances());
    }
    return instance_id;
}

void RuntimeIvModuleInstances::remove_instance(std::string const &instance_id)
{
    RuntimeIvModuleRequiredDefinitionsChanged required_diff{};
    RuntimeIvModuleInstancesChanged instance_diff{};
    bool list_changed = false;

    {
        std::scoped_lock lock(mutex);
        auto desired = desired_instances_by_id.find(instance_id);
        if (desired == desired_instances_by_id.end()) {
            return;
        }

        auto const definition_id = desired->second.definition_id;
        desired_instances_by_id.erase(desired);
        list_changed = true;

        if (realized_instances_by_id.erase(instance_id) > 0) {
            instance_diff.deleted_instance_ids.push_back(instance_id);
        }

        bool still_required = false;
        for (auto const &entry : desired_instances_by_id) {
            if (entry.second.definition_id == definition_id) {
                still_required = true;
                break;
            }
        }
        if (!still_required && required_definitions_by_id.erase(definition_id) > 0) {
            required_diff.deleted_definition_ids.push_back(definition_id);
        }
    }

    if (!instance_diff.deleted_instance_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instances_changed_event,
            instance_diff);
    }
    if (!required_diff.created.empty() ||
        !required_diff.updated.empty() ||
        !required_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_required_definitions_changed_event,
            required_diff);
    }
    if (list_changed) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instances_list_changed_event,
            list_instances());
    }
}

std::vector<RuntimeIvModuleInstanceInfo> RuntimeIvModuleInstances::list_instances() const
{
    std::vector<RuntimeIvModuleInstanceInfo> instances;

    std::scoped_lock lock(mutex);
    instances.reserve(desired_instances_by_id.size());
    for (auto const &entry : desired_instances_by_id) {
        auto info = RuntimeIvModuleInstanceInfo{
            .instance_id = entry.second.instance_id,
            .definition_id = entry.second.definition_id,
            .module_root = entry.second.module_root,
        };
        if (auto realized = realized_instances_by_id.find(entry.second.instance_id);
            realized != realized_instances_by_id.end()) {
            info.realized = true;
            info.module_id = realized->second.module_id;
        }
        instances.push_back(std::move(info));
    }

    std::ranges::sort(instances, [](auto const &a, auto const &b) {
        return a.instance_id < b.instance_id;
    });
    return instances;
}

void RuntimeIvModuleInstances::handle_iv_module_definitions_changed(
    RuntimeIvModuleDefinitionsChanged const &diff)
{
    RuntimeIvModuleInstancesChanged instance_diff{};
    bool list_changed = false;

    {
        std::scoped_lock lock(mutex);
        for (auto const &definition : diff.created) {
            for (auto const &entry : desired_instances_by_id) {
                if (entry.second.definition_id != definition.definition_id) {
                    continue;
                }
                auto instance = make_instance_from_definition(definition, entry.second.instance_id);
                realized_instances_by_id[entry.second.instance_id] = instance;
                instance_diff.created.push_back(std::move(instance));
                list_changed = true;
            }
        }
        for (auto const &definition : diff.updated) {
            for (auto const &entry : desired_instances_by_id) {
                if (entry.second.definition_id != definition.definition_id) {
                    continue;
                }
                auto instance = make_instance_from_definition(definition, entry.second.instance_id);
                realized_instances_by_id[entry.second.instance_id] = instance;
                instance_diff.updated.push_back(std::move(instance));
                list_changed = true;
            }
        }
        for (auto const &definition_id : diff.deleted_definition_ids) {
            for (auto it = realized_instances_by_id.begin();
                 it != realized_instances_by_id.end();) {
                if (it->second.definition_id == definition_id) {
                    instance_diff.deleted_instance_ids.push_back(it->second.instance_id);
                    it = realized_instances_by_id.erase(it);
                    list_changed = true;
                } else {
                    ++it;
                }
            }
        }
    }

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_instances_changed_event,
        instance_diff);
    if (list_changed) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instances_list_changed_event,
            list_instances());
    }
}
} // namespace iv
