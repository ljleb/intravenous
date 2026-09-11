#include <intravenous/runtime/iv_module_instances.h>

#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/iv_packages.h>
#include <intravenous/runtime/iv_packages_events.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/uuid.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <algorithm>
#include <ranges>
#include <sstream>
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

IvModuleInstance make_instance_from_definition(
    IvModuleDefinition const &definition,
    std::string const &instance_id,
    std::string const &display_name,
    std::optional<size_t> default_silence_ttl_samples)
{
    IvModuleInstance instance{};
    instance.instance_id = instance_id;
    instance.definition_id = definition.definition_id;
    instance.display_name = display_name;
    instance.package_root = definition.package_root;
    instance.module_id = definition.module_id;
    instance.introspection = definition.introspection;
    instance.default_silence_ttl_samples = default_silence_ttl_samples;
    return instance;
}

void emit_debug_message(std::string message)
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectMessageNotification{
            .level = "debug",
            .message = std::move(message),
        }));
}

} // namespace

bool IvModuleInstances::realize_instance_locked(
    std::string const& instance_id,
    IvModuleDefinition const& definition,
    bool update_existing,
    IvModuleInstancesChanged& instance_diff,
    IvModuleInstanceBuildersChanged& builders_diff)
{
    auto const desired = desired_instances_by_id.find(instance_id);
    if (desired == desired_instances_by_id.end()
        || desired->second.definition_id != definition.definition_id) {
        return false;
    }

    auto const is_new_instance = !realized_instances_by_id.contains(instance_id);
    if (!is_new_instance && !update_existing) {
        return false;
    }

    auto instance = make_instance_from_definition(
        definition,
        desired->second.instance_id,
        desired->second.display_name,
        desired->second.default_silence_ttl_samples);
    auto &stored_instance = realized_instances_by_id[instance_id];
    stored_instance = instance;
    realized_module_refs_by_id[instance_id] = definition.module_refs;
    realized_roots_by_id[instance_id] = definition.root;
    (is_new_instance ? instance_diff.created : instance_diff.updated)
        .push_back(std::move(instance));
    auto root_ref = IvModuleInstanceBuilderRef{
        .instance = &stored_instance,
        .root = definition.root,
        .module_refs = definition.module_refs,
        .default_silence_ttl_samples =
            desired->second.default_silence_ttl_samples,
    };
    (is_new_instance ? builders_diff.created : builders_diff.updated)
        .push_back(std::move(root_ref));
    return true;
}

void IvModuleInstances::publish_instance_changes(
    IvModuleInstancesChanged instance_diff,
    IvModuleInstanceBuildersChanged builders_diff,
    bool list_changed)
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_instances_changed_event,
        instance_diff);
    IvModuleInstanceBuildersAckBuilder builders_ack;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_instance_builders_changed_event,
        builders_diff,
        builders_ack);
    builders_diff.version_index =
        builders_ack.version_index().value_or(builders_diff.version_index);
    for (auto &created : builders_diff.created) {
        if (!created.instance) {
            continue;
        }
        created.prerequisite_lanes =
            builders_ack.prerequisite_lanes_for(created.instance->instance_id)
                .value_or(std::vector<LaneId>{});
    }
    for (auto &updated : builders_diff.updated) {
        if (!updated.instance) {
            continue;
        }
        updated.prerequisite_lanes =
            builders_ack.prerequisite_lanes_for(updated.instance->instance_id)
                .value_or(std::vector<LaneId>{});
    }
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_instance_builders_completed_event,
        builders_diff);
    if (list_changed) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instances_list_changed_event,
            list_instances());
    }
    if (!instance_diff.created.empty() ||
        !instance_diff.updated.empty() ||
        !instance_diff.deleted_instance_ids.empty()) {
        auto const instances = list_instances();
        size_t realized_count = 0;
        for (auto const &instance : instances) {
            if (instance.realized) {
                ++realized_count;
            }
        }
        std::ostringstream message;
        message
            << "iv instances realized: created=" << instance_diff.created.size()
            << " updated=" << instance_diff.updated.size()
            << " deleted=" << instance_diff.deleted_instance_ids.size()
            << " rootsCreated=" << builders_diff.created.size()
            << " rootsUpdated=" << builders_diff.updated.size()
            << " rootsDeleted=" << builders_diff.deleted_instance_ids.size()
            << " totalInstances=" << instances.size()
            << " realizedInstances=" << realized_count;
        emit_debug_message(message.str());
    }
}

std::string IvModuleInstances::create_instance(
    std::string_view definition_id,
    std::filesystem::path package_root,
    std::optional<std::string> requested_instance_id,
    std::optional<std::string> requested_display_name)
{
    IvModuleRequiredDefinitionsChanged required_diff{};
    bool list_changed = false;
    auto normalized_root = normalize_path(package_root);
    auto const definition_key = std::string(definition_id);
    auto display_name = requested_display_name.value_or(definition_key);
    if (display_name.empty()) {
        display_name = definition_key;
    }
    std::string instance_id{};

    {
        std::scoped_lock lock(mutex);
        if (requested_instance_id.has_value()) {
            instance_id = *requested_instance_id;
            if (desired_instances_by_id.contains(instance_id)) {
                throw std::runtime_error("duplicate iv module instance id: " + instance_id);
            }
        } else {
            instance_id = generate_uuid_v4().str();
        }
        desired_instances_by_id.emplace(instance_id, DesiredInstance{
            .instance_id = instance_id,
            .definition_id = definition_key,
            .display_name = display_name,
            .package_root = normalized_root,
        });
        list_changed = true;

        if (!required_definitions_by_id.contains(definition_key)) {
            IvModuleRequiredDefinition required{
                .definition_id = definition_key,
                .package_root = normalized_root,
            };
            required_definitions_by_id.emplace(definition_key, required);
            required_diff.created.push_back(std::move(required));
        } else {
            auto &required = required_definitions_by_id.at(definition_key);
            required.package_root = normalized_root;
            required_diff.updated.push_back(required);
        }
    }

    if (!required_diff.created.empty() ||
        !required_diff.updated.empty() ||
        !required_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_required_definitions_changed_event,
            required_diff);
    }

    // Definitions can already be loaded before persistence recreates the desired
    // instance set. Query the current snapshot so this instance does not have to
    // wait for an unrelated future package reload to become realized.
    IvModuleDefinitionLookupBuilder definition_builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_definition_lookup_event,
        definition_key,
        definition_builder);
    if (definition_builder.has_response()) {
        auto const definition = definition_builder.definition();
        if (definition.has_value()
            && definition->package_root == normalized_root) {
            IvModuleInstancesChanged instance_diff{};
            IvModuleInstanceBuildersChanged builders_diff{};
            bool realized = false;
            {
                std::scoped_lock lock(mutex);
                realized = realize_instance_locked(
                    instance_id,
                    *definition,
                    false,
                    instance_diff,
                    builders_diff);
            }
            if (realized) {
                publish_instance_changes(
                    std::move(instance_diff),
                    std::move(builders_diff),
                    true);
                list_changed = false;
            }
        }
    }
    if (list_changed) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instances_list_changed_event,
            list_instances());
    }
    return instance_id;
}

void IvModuleInstances::remove_instance(std::string const &instance_id)
{
    IvModuleRequiredDefinitionsChanged required_diff{};
    IvModuleInstancesChanged instance_diff{};
    IvModuleInstanceBuildersChanged builders_diff{};
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
            builders_diff.deleted_instance_ids.push_back(instance_id);
        }
        realized_module_refs_by_id.erase(instance_id);
        realized_roots_by_id.erase(instance_id);

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
    if (!builders_diff.deleted_instance_ids.empty()) {
        IvModuleInstanceBuildersAckBuilder builders_ack;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instance_builders_changed_event,
            builders_diff,
            builders_ack);
        builders_diff.version_index = builders_ack.version_index().value_or(builders_diff.version_index);
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instance_builders_completed_event,
            builders_diff);
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

void IvModuleInstances::set_default_silence_ttl_samples(
    std::string const &instance_id,
    size_t default_silence_ttl_samples)
{
    update_instances({Update{
        .instance_id = instance_id,
        .default_silence_ttl_samples = default_silence_ttl_samples,
    }});
}

void IvModuleInstances::update_instances(std::vector<Update> updates)
{
    IvModuleInstancesChanged instance_diff{};
    IvModuleInstanceBuildersChanged builders_diff{};
    bool list_changed = false;

    {
        std::scoped_lock lock(mutex);
        for (auto const &update : updates) {
            auto desired = desired_instances_by_id.find(update.instance_id);
            if (desired == desired_instances_by_id.end()) {
                throw std::runtime_error("unknown iv module instance id: " + update.instance_id);
            }

            bool realized_changed = false;
            auto realized = realized_instances_by_id.find(update.instance_id);

            if (update.display_name.has_value()) {
                auto next_display_name = *update.display_name;
                if (next_display_name.empty()) {
                    next_display_name = desired->second.definition_id;
                }
                if (desired->second.display_name != next_display_name) {
                    desired->second.display_name = std::move(next_display_name);
                    list_changed = true;
                    if (realized != realized_instances_by_id.end()) {
                        realized->second.display_name = desired->second.display_name;
                        realized_changed = true;
                    }
                }
            }

            if (update.default_silence_ttl_samples.has_value()) {
                desired->second.default_silence_ttl_samples =
                    update.default_silence_ttl_samples;
                list_changed = true;
                if (realized != realized_instances_by_id.end()) {
                    realized->second.default_silence_ttl_samples =
                        update.default_silence_ttl_samples;
                    auto root_it = realized_roots_by_id.find(update.instance_id);
                    if (root_it != realized_roots_by_id.end()) {
                        builders_diff.updated.push_back(IvModuleInstanceBuilderRef{
                            .instance = &realized->second,
                            .root = root_it->second,
                            .module_refs = realized_module_refs_by_id[update.instance_id],
                            .default_silence_ttl_samples =
                                update.default_silence_ttl_samples,
                        });
                    }
                }
            }

            if (realized_changed && realized != realized_instances_by_id.end()) {
                instance_diff.updated.push_back(realized->second);
            }
        }
    }

    if (!instance_diff.created.empty() ||
        !instance_diff.updated.empty() ||
        !instance_diff.deleted_instance_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instances_changed_event,
            instance_diff);
    }
    if (!builders_diff.updated.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instance_builders_completed_event,
            builders_diff);
    }
    if (list_changed) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instances_list_changed_event,
            list_instances());
    }
}

void IvModuleInstances::refresh_package_roots(IvPackages const &packages)
{
    IvModuleRequiredDefinitionsChanged required_diff{};
    bool list_changed = false;

    {
        std::scoped_lock lock(mutex);
        for (auto &entry : desired_instances_by_id) {
            auto const package = packages.find_package(entry.second.definition_id);
            if (!package) {
                continue;
            }
            auto const package_root = normalize_path(package->package_root);
            if (entry.second.package_root == package_root) {
                continue;
            }

            entry.second.package_root = package_root;
            list_changed = true;

            auto required = required_definitions_by_id.find(entry.second.definition_id);
            if (required != required_definitions_by_id.end() &&
                required->second.package_root != package_root) {
                required->second.package_root = package_root;
                required_diff.updated.push_back(required->second);
            }
        }
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

void IvModuleInstances::handle_project_create_iv_module_instance(
    ProjectCreateIvModuleInstanceRequest const &request,
    ProjectStringBuilder &builder)
{
    IvPackageLookupBuilder package_builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_package_lookup_event,
        request.module_id,
        package_builder);
    if (!package_builder.has_response()) {
        throw std::runtime_error("IV package service is unavailable");
    }
    auto const package = package_builder.package();
    if (!package.has_value()) {
        throw std::runtime_error("unknown IV package for module: " + request.module_id);
    }
    builder.succeed(create_instance(
        request.module_id,
        package->package_root,
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
            .default_silence_ttl_samples = update.default_silence_ttl_samples,
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

std::vector<IvModuleInstanceInfo> IvModuleInstances::list_instances() const
{
    std::vector<IvModuleInstanceInfo> instances;

    std::scoped_lock lock(mutex);
    instances.reserve(desired_instances_by_id.size());
    for (auto const &entry : desired_instances_by_id) {
        auto info = IvModuleInstanceInfo{
            .instance_id = entry.second.instance_id,
            .definition_id = entry.second.definition_id,
            .display_name = entry.second.display_name,
            .package_root = entry.second.package_root,
            .default_silence_ttl_samples = entry.second.default_silence_ttl_samples,
            .module_id = entry.second.definition_id,
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

void IvModuleInstances::handle_iv_module_definitions_changed(
    IvModuleDefinitionsChanged const &diff)
{
    IvModuleInstancesChanged instance_diff{};
    IvModuleInstanceBuildersChanged builders_diff{};
    bool list_changed = false;

    {
        std::scoped_lock lock(mutex);
        for (auto const &definition : diff.created) {
            for (auto const &entry : desired_instances_by_id) {
                if (realize_instance_locked(
                        entry.second.instance_id,
                        definition,
                        true,
                        instance_diff,
                        builders_diff)) {
                    list_changed = true;
                }
            }
        }
        for (auto const &definition : diff.updated) {
            for (auto const &entry : desired_instances_by_id) {
                if (realize_instance_locked(
                        entry.second.instance_id,
                        definition,
                        true,
                        instance_diff,
                        builders_diff)) {
                    list_changed = true;
                }
            }
        }
        for (auto const &definition_id : diff.deleted_definition_ids) {
            for (auto it = realized_instances_by_id.begin();
                 it != realized_instances_by_id.end();) {
                if (it->second.definition_id == definition_id) {
                    instance_diff.deleted_instance_ids.push_back(it->second.instance_id);
                    builders_diff.deleted_instance_ids.push_back(it->second.instance_id);
                    realized_module_refs_by_id.erase(it->second.instance_id);
                    realized_roots_by_id.erase(it->second.instance_id);
                    it = realized_instances_by_id.erase(it);
                    list_changed = true;
                } else {
                    ++it;
                }
            }
        }
    }

    publish_instance_changes(
        std::move(instance_diff),
        std::move(builders_diff),
        list_changed);
}

} // namespace iv
