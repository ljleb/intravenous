#include <intravenous/runtime/iv_module_instances.h>

#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/iv_module_sources.h>
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
    std::optional<size_t> default_silence_ttl_samples)
{
    IvModuleInstance instance{};
    instance.instance_id = instance_id;
    instance.definition_id = definition.definition_id;
    instance.module_root = definition.module_root;
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

std::string IvModuleInstances::create_instance(
    std::string_view definition_id,
    std::filesystem::path module_root,
    std::optional<std::string> requested_instance_id)
{
    IvModuleRequiredDefinitionsChanged required_diff{};
    bool list_changed = false;
    auto normalized_root = normalize_path(module_root);
    auto const definition_key = std::string(definition_id);
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
            .module_root = normalized_root,
        });
        list_changed = true;

        if (!required_definitions_by_id.contains(definition_key)) {
            IvModuleRequiredDefinition required{
                .definition_id = definition_key,
                .module_root = normalized_root,
            };
            required_definitions_by_id.emplace(definition_key, required);
            required_diff.created.push_back(std::move(required));
        } else {
            auto &required = required_definitions_by_id.at(definition_key);
            required.module_root = normalized_root;
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
        realized_builders_by_id.erase(instance_id);

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
    IvModuleInstanceBuildersChanged builders_diff{};

    {
        std::scoped_lock lock(mutex);
        auto desired = desired_instances_by_id.find(instance_id);
        if (desired == desired_instances_by_id.end()) {
            throw std::runtime_error("unknown iv module instance id: " + instance_id);
        }

        desired->second.default_silence_ttl_samples = default_silence_ttl_samples;

        if (auto realized = realized_instances_by_id.find(instance_id);
            realized != realized_instances_by_id.end()) {
            realized->second.default_silence_ttl_samples = default_silence_ttl_samples;
            auto builder_it = realized_builders_by_id.find(instance_id);
            if (builder_it != realized_builders_by_id.end()) {
                builders_diff.updated.push_back(IvModuleInstanceBuilderRef{
                    .instance = &realized->second,
                    .builder = &builder_it->second,
                    .module_refs = realized_module_refs_by_id[instance_id],
                    .default_silence_ttl_samples = default_silence_ttl_samples,
                });
            }
        }
    }

    if (!builders_diff.updated.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instance_builders_completed_event,
            builders_diff);
    }
}

void IvModuleInstances::refresh_source_roots(IvModuleSources const &sources)
{
    auto const listed_sources = sources.list_sources();
    std::unordered_map<std::string, std::filesystem::path> roots_by_definition_id;
    roots_by_definition_id.reserve(listed_sources.size());
    for (auto const &source : listed_sources) {
        roots_by_definition_id.emplace(
            source.module_id,
            normalize_path(source.module_root));
    }

    IvModuleRequiredDefinitionsChanged required_diff{};
    bool list_changed = false;

    {
        std::scoped_lock lock(mutex);
        for (auto &entry : desired_instances_by_id) {
            auto const source = roots_by_definition_id.find(entry.second.definition_id);
            if (source == roots_by_definition_id.end()) {
                continue;
            }
            if (entry.second.module_root == source->second) {
                continue;
            }

            entry.second.module_root = source->second;
            list_changed = true;

            auto required = required_definitions_by_id.find(entry.second.definition_id);
            if (required != required_definitions_by_id.end() &&
                required->second.module_root != source->second) {
                required->second.module_root = source->second;
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

std::vector<IvModuleInstanceInfo> IvModuleInstances::list_instances() const
{
    std::vector<IvModuleInstanceInfo> instances;

    std::scoped_lock lock(mutex);
    instances.reserve(desired_instances_by_id.size());
    for (auto const &entry : desired_instances_by_id) {
        auto info = IvModuleInstanceInfo{
            .instance_id = entry.second.instance_id,
            .definition_id = entry.second.definition_id,
            .module_root = entry.second.module_root,
            .default_silence_ttl_samples = entry.second.default_silence_ttl_samples,
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
                if (entry.second.definition_id != definition.definition_id) {
                    continue;
                }
                auto const is_new_instance = !realized_instances_by_id.contains(entry.second.instance_id);
                auto instance = make_instance_from_definition(
                    definition,
                    entry.second.instance_id,
                    entry.second.default_silence_ttl_samples);
                auto &stored_instance = realized_instances_by_id[entry.second.instance_id];
                stored_instance = instance;
                realized_module_refs_by_id[entry.second.instance_id] = definition.module_refs;
                auto &stored_builder = realized_builders_by_id[entry.second.instance_id];
                stored_builder = definition.canonical_builder != nullptr
                    ? *definition.canonical_builder
                    : GraphBuilder{};
                (is_new_instance ? instance_diff.created : instance_diff.updated).push_back(std::move(instance));
                auto builder_ref = IvModuleInstanceBuilderRef{
                    .instance = &stored_instance,
                    .builder = &stored_builder,
                    .module_refs = definition.module_refs,
                    .default_silence_ttl_samples =
                        entry.second.default_silence_ttl_samples,
                };
                (is_new_instance ? builders_diff.created : builders_diff.updated).push_back(std::move(builder_ref));
                list_changed = true;
            }
        }
        for (auto const &definition : diff.updated) {
            for (auto const &entry : desired_instances_by_id) {
                if (entry.second.definition_id != definition.definition_id) {
                    continue;
                }
                auto const is_new_instance = !realized_instances_by_id.contains(entry.second.instance_id);
                auto instance = make_instance_from_definition(
                    definition,
                    entry.second.instance_id,
                    entry.second.default_silence_ttl_samples);
                auto &stored_instance = realized_instances_by_id[entry.second.instance_id];
                stored_instance = instance;
                realized_module_refs_by_id[entry.second.instance_id] = definition.module_refs;
                auto &stored_builder = realized_builders_by_id[entry.second.instance_id];
                stored_builder = definition.canonical_builder != nullptr
                    ? *definition.canonical_builder
                    : GraphBuilder{};
                (is_new_instance ? instance_diff.created : instance_diff.updated).push_back(std::move(instance));
                auto builder_ref = IvModuleInstanceBuilderRef{
                    .instance = &stored_instance,
                    .builder = &stored_builder,
                    .module_refs = definition.module_refs,
                    .default_silence_ttl_samples =
                        entry.second.default_silence_ttl_samples,
                };
                (is_new_instance ? builders_diff.created : builders_diff.updated).push_back(std::move(builder_ref));
                list_changed = true;
            }
        }
        for (auto const &definition_id : diff.deleted_definition_ids) {
            for (auto it = realized_instances_by_id.begin();
                 it != realized_instances_by_id.end();) {
                if (it->second.definition_id == definition_id) {
                    instance_diff.deleted_instance_ids.push_back(it->second.instance_id);
                    builders_diff.deleted_instance_ids.push_back(it->second.instance_id);
                    realized_module_refs_by_id.erase(it->second.instance_id);
                    realized_builders_by_id.erase(it->second.instance_id);
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
    IvModuleInstanceBuildersAckBuilder builders_ack;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_instance_builders_changed_event,
        builders_diff,
        builders_ack);
    builders_diff.version_index = builders_ack.version_index().value_or(builders_diff.version_index);
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
            << " buildersCreated=" << builders_diff.created.size()
            << " buildersUpdated=" << builders_diff.updated.size()
            << " buildersDeleted=" << builders_diff.deleted_instance_ids.size()
            << " totalInstances=" << instances.size()
            << " realizedInstances=" << realized_count;
        emit_debug_message(message.str());
    }
}

void IvModuleInstances::handle_graph_input_lanes_rebuild_requested(
    GraphInputLanesRebuildRequested const &request)
{
    IvModuleInstanceBuildersChanged builders_diff{};
    builders_diff.version_index = request.version_index;

    {
        std::scoped_lock lock(mutex);
        for (auto const &instance_id : request.instance_ids) {
            auto desired = desired_instances_by_id.find(instance_id);
            if (desired == desired_instances_by_id.end()) {
                continue;
            }
            auto realized = realized_instances_by_id.find(instance_id);
            if (realized == realized_instances_by_id.end()) {
                continue;
            }

            IvModuleDefinitionBuilderBuilder builder_query;
            IV_INVOKE_SINGLETON_EVENT(
                iv_runtime_iv_module_definition_builder_requested_event,
                IvModuleDefinitionBuilderRequest{
                    .definition_id = desired->second.definition_id,
                },
                builder_query);
            auto const *builder = builder_query.build();
            if (builder == nullptr) {
                continue;
            }

            auto &stored_builder = realized_builders_by_id[instance_id];
            stored_builder = *builder;
            builders_diff.updated.push_back(IvModuleInstanceBuilderRef{
                .instance = &realized->second,
                .builder = &stored_builder,
                .module_refs = realized_module_refs_by_id[instance_id],
                .default_silence_ttl_samples =
                    desired->second.default_silence_ttl_samples,
            });
        }
    }

    if (builders_diff.updated.empty()) {
        emit_debug_message(
            "iv instances rebuild request produced no builders: revision="
            + std::to_string(request.version_index)
            + " requestedInstances=" + std::to_string(request.instance_ids.size()));
        return;
    }

    IvModuleInstanceBuildersAckBuilder builders_ack;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_instance_builders_changed_event,
        builders_diff,
        builders_ack);
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
    emit_debug_message(
        "iv instances rebuild applied builders: revision="
        + std::to_string(request.version_index)
        + " updatedBuilders=" + std::to_string(builders_diff.updated.size()));
}
} // namespace iv
