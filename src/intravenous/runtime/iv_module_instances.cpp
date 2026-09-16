#include <intravenous/runtime/iv_module_instances.h>

#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/uuid.h>

#include <algorithm>
#include <ranges>
#include <stdexcept>
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

IvModuleInstance make_instance(
    ModuleNodeDefinition const &definition,
    std::string const &instance_id,
    std::string const &display_name)
{
    return IvModuleInstance{
        .instance_id = instance_id,
        .definition_id = definition.definition_id,
        .display_name = display_name,
        .package_root = definition.package_root,
        .module_id = definition.module_id,
    };
}
} // namespace

bool IvModuleInstances::publish_instance_locked(
    std::string const &instance_id,
    ModuleNodeDefinition const &definition,
    IvModuleInstancesChanged &diff)
{
    auto desired = desired_instances_by_id_.find(instance_id);
    if (desired == desired_instances_by_id_.end()
        || desired->second.definition_id != definition.definition_id) {
        return false;
    }

    if (desired->second.package_root != definition.package_root) {
        desired->second.package_root = definition.package_root;
        if (auto required = required_definitions_by_id_.find(definition.definition_id);
            required != required_definitions_by_id_.end()) {
            required->second.package_root = definition.package_root;
        }
    }

    auto instance = make_instance(
        definition,
        desired->second.instance_id,
        desired->second.display_name);
    auto const is_new = !published_instances_by_id_.contains(instance_id);
    published_instances_by_id_[instance_id] = instance;
    (is_new ? diff.created : diff.updated).push_back(std::move(instance));
    return true;
}

void IvModuleInstances::publish_instance_changes(
    IvModuleInstancesChanged diff,
    bool list_changed) const
{
    if (!diff.created.empty() || !diff.updated.empty() || !diff.deleted_instance_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(iv_runtime_iv_module_instances_changed_event, diff);
    }
    if (list_changed) {
        IV_INVOKE_LINKER_EVENT(iv_runtime_iv_module_instances_list_changed_event, list_instances());
    }
}

void IvModuleInstances::publish_instance_declarations_changed() const
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_instance_declarations_changed_event,
        list_instances());
}

std::string IvModuleInstances::create_instance(
    std::string_view definition_id,
    std::filesystem::path package_root,
    std::optional<std::string> requested_instance_id,
    std::optional<std::string> requested_display_name)
{
    IvModuleRequiredDefinitionsChanged required_diff;
    IvModuleInstancesChanged instance_diff;
    auto const definition_key = std::string(definition_id);
    auto normalized_root = normalize_path(package_root);
    auto display_name = requested_display_name.value_or(definition_key);
    if (display_name.empty()) {
        display_name = definition_key;
    }
    std::string instance_id;

    {
        std::scoped_lock lock(mutex_);
        instance_id = requested_instance_id.value_or(generate_uuid_v4().str());
        if (desired_instances_by_id_.contains(instance_id)) {
            throw std::runtime_error("duplicate iv module instance id: " + instance_id);
        }

        desired_instances_by_id_.emplace(instance_id, DesiredInstance{
            .instance_id = instance_id,
            .definition_id = definition_key,
            .display_name = std::move(display_name),
            .package_root = normalized_root,
        });

        if (auto required = required_definitions_by_id_.find(definition_key);
            required == required_definitions_by_id_.end()) {
            auto value = IvModuleRequiredDefinition{
                .definition_id = definition_key,
                .package_root = normalized_root,
            };
            required_definitions_by_id_.emplace(definition_key, value);
            required_diff.created.push_back(std::move(value));
        } else if (required->second.package_root != normalized_root) {
            required->second.package_root = normalized_root;
            required_diff.updated.push_back(required->second);
        }

        if (auto definition = definitions_by_id_.find(definition_key);
            definition != definitions_by_id_.end()) {
            publish_instance_locked(instance_id, definition->second, instance_diff);
        }
    }

    if (!required_diff.created.empty() || !required_diff.updated.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_required_definitions_changed_event,
            required_diff);
    }
    publish_instance_changes(std::move(instance_diff), true);
    publish_instance_declarations_changed();
    return instance_id;
}

void IvModuleInstances::remove_instance(std::string const &instance_id)
{
    IvModuleRequiredDefinitionsChanged required_diff;
    IvModuleInstancesChanged instance_diff;
    bool changed = false;

    {
        std::scoped_lock lock(mutex_);
        auto desired = desired_instances_by_id_.find(instance_id);
        if (desired == desired_instances_by_id_.end()) {
            return;
        }
        auto const definition_id = desired->second.definition_id;
        desired_instances_by_id_.erase(desired);
        changed = true;

        if (published_instances_by_id_.erase(instance_id) > 0) {
            instance_diff.deleted_instance_ids.push_back(instance_id);
        }

        auto const still_required = std::ranges::any_of(
            desired_instances_by_id_,
            [&](auto const &entry) { return entry.second.definition_id == definition_id; });
        if (!still_required && required_definitions_by_id_.erase(definition_id) > 0) {
            required_diff.deleted_definition_ids.push_back(definition_id);
        }
    }

    publish_instance_changes(std::move(instance_diff), changed);
    if (changed) {
        publish_instance_declarations_changed();
    }
    if (!required_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_required_definitions_changed_event,
            required_diff);
    }
}

void IvModuleInstances::update_instances(std::vector<Update> updates)
{
    IvModuleInstancesChanged diff;
    bool list_changed = false;
    {
        std::scoped_lock lock(mutex_);
        for (auto const &update : updates) {
            auto desired = desired_instances_by_id_.find(update.instance_id);
            if (desired == desired_instances_by_id_.end()) {
                throw std::runtime_error("unknown iv module instance id: " + update.instance_id);
            }
            if (!update.display_name.has_value()) {
                continue;
            }
            auto next = *update.display_name;
            if (next.empty()) {
                next = desired->second.definition_id;
            }
            if (desired->second.display_name == next) {
                continue;
            }
            desired->second.display_name = std::move(next);
            list_changed = true;
            if (auto instance = published_instances_by_id_.find(update.instance_id);
                instance != published_instances_by_id_.end()) {
                instance->second.display_name = desired->second.display_name;
                diff.updated.push_back(instance->second);
            }
        }
    }
    publish_instance_changes(std::move(diff), list_changed);
    if (list_changed) {
        publish_instance_declarations_changed();
    }
}

std::vector<IvModuleInstanceInfo> IvModuleInstances::list_instances() const
{
    std::vector<IvModuleInstanceInfo> result;
    std::scoped_lock lock(mutex_);
    result.reserve(desired_instances_by_id_.size());
    for (auto const &[id, desired] : desired_instances_by_id_) {
        auto info = IvModuleInstanceInfo{
            .instance_id = id,
            .definition_id = desired.definition_id,
            .display_name = desired.display_name,
            .package_root = desired.package_root,
            .module_id = desired.definition_id,
        };
        if (auto published = published_instances_by_id_.find(id);
            published != published_instances_by_id_.end()) {
            info.realized = true;
            info.module_id = published->second.module_id;
        }
        result.push_back(std::move(info));
    }
    std::ranges::sort(result, {}, &IvModuleInstanceInfo::instance_id);
    return result;
}

void IvModuleInstances::handle_iv_package_definitions_changed(
    IvPackageDefinitionsChanged const &package_diff)
{
    IvModuleInstancesChanged diff;
    bool list_changed = false;
    {
        std::scoped_lock lock(mutex_);
        auto apply = [&](ModuleNodeDefinition const &definition) {
            definitions_by_id_[definition.definition_id] = definition;
            for (auto &[instance_id, desired] : desired_instances_by_id_) {
                if (desired.definition_id != definition.definition_id) {
                    continue;
                }
                list_changed = publish_instance_locked(instance_id, definition, diff) || list_changed;
            }
        };
        for (auto const &definition : package_diff.module_definitions.created) {
            apply(definition);
        }
        for (auto const &definition : package_diff.module_definitions.updated) {
            apply(definition);
        }
        for (auto const &definition_id : package_diff.module_definitions.deleted_definition_ids) {
            definitions_by_id_.erase(definition_id);
            for (auto it = published_instances_by_id_.begin();
                 it != published_instances_by_id_.end();) {
                if (it->second.definition_id != definition_id) {
                    ++it;
                    continue;
                }
                diff.deleted_instance_ids.push_back(it->first);
                it = published_instances_by_id_.erase(it);
                list_changed = true;
            }
        }
    }
    publish_instance_changes(std::move(diff), list_changed);
}

void IvModuleInstances::handle_project_create_iv_module_instance(
    ProjectCreateIvModuleInstanceRequest const &request,
    ProjectStringBuilder &builder)
{
    std::optional<std::filesystem::path> package_root;
    {
        std::scoped_lock lock(mutex_);
        if (auto definition = definitions_by_id_.find(request.module_id);
            definition != definitions_by_id_.end()) {
            package_root = definition->second.package_root;
        }
    }
    if (!package_root.has_value()) {
        package_root = request.package_root;
    }
    if (!package_root.has_value()) {
        throw std::runtime_error("unknown loaded IV module definition: " + request.module_id);
    }

    builder.succeed(create_instance(
        request.module_id,
        *package_root,
        request.instance_id,
        request.display_name));
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void IvModuleInstances::handle_project_delete_iv_module_instance(
    ProjectDeleteIvModuleInstanceRequest const &request,
    ProjectAckBuilder &builder)
{
    remove_instance(request.instance_id);
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void IvModuleInstances::handle_project_update_iv_module_instances(
    ProjectUpdateIvModuleInstancesRequest const &request,
    ProjectAckBuilder &builder)
{
    std::vector<Update> updates;
    updates.reserve(request.updates.size());
    for (auto const &update : request.updates) {
        updates.push_back(Update{
            .instance_id = update.instance_id,
            .display_name = update.display_name,
        });
    }
    update_instances(std::move(updates));
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void IvModuleInstances::handle_project_persistence_collect_state(
    ProjectPersistenceBuilder &builder) const
{
    builder.add_iv_module_instances(list_instances());
}

void IvModuleInstances::handle_socket_rpc_create_iv_module_instance(
    CreateIvModuleInstanceRequest const &request,
    SocketRpcCreateIvModuleInstanceResultBuilder &builder)
{
    try {
        ProjectStringBuilder project_builder;
        handle_project_create_iv_module_instance(
            ProjectCreateIvModuleInstanceRequest{
                .module_id = request.module_id,
                .display_name = request.display_name,
            },
            project_builder);
        builder.succeed(project_builder.build());
    } catch (std::exception const &error) {
        builder.fail(error.what());
    }
}

void IvModuleInstances::handle_socket_rpc_delete_iv_module_instance(
    DeleteIvModuleInstanceRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_delete_iv_module_instance(
            ProjectDeleteIvModuleInstanceRequest{.instance_id = request.instance_id},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const &error) {
        builder.fail(error.what());
    }
}

void IvModuleInstances::handle_socket_rpc_update_iv_module_instances(
    UpdateIvModuleInstancesRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    try {
        std::vector<ProjectUpdateIvModuleInstance> updates;
        updates.reserve(request.updates.size());
        for (auto const &update : request.updates) {
            updates.push_back(ProjectUpdateIvModuleInstance{
                .instance_id = update.instance_id,
                .display_name = update.display_name,
            });
        }
        ProjectAckBuilder project_builder;
        handle_project_update_iv_module_instances(
            ProjectUpdateIvModuleInstancesRequest{.updates = std::move(updates)},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const &error) {
        builder.fail(error.what());
    }
}

void IvModuleInstances::handle_socket_rpc_get_iv_module_instances(
    GetIvModuleInstancesRequest const &request,
    SocketRpcIvModuleInstancesResultBuilder &builder) const
{
    auto instances = list_instances();
    if (request.source_file_path.has_value()) {
        IvModuleInstancesSourceFileFilterBuilder filter_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instances_source_file_filter_event,
            *request.source_file_path,
            instances,
            filter_builder);
        if (filter_builder.has_response()) {
            instances = filter_builder.build();
        }
    }
    builder.succeed(std::move(instances));
}
} // namespace iv
