#include <intravenous/runtime/iv_module_definitions.h>

#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_reload.h>

#include <algorithm>
#include <ranges>
#include <system_error>
#include <unordered_set>

namespace iv {
namespace {
std::filesystem::path normalize_path(std::filesystem::path const& path)
{
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? std::filesystem::absolute(path).lexically_normal()
              : canonical.lexically_normal();
}

std::unique_ptr<IvModuleDefinitions::DefinitionState> make_definition_state(
    IvModuleReloadedDefinition const& loaded)
{
    auto state = std::make_unique<IvModuleDefinitions::DefinitionState>();
    state->module_refs = loaded.module_refs;
    state->snapshot = IvModuleDefinition{
        .definition_id = loaded.definition_id,
        .source_id = loaded.source_id,
        .module_root = normalize_path(loaded.module_root),
        .module_id = loaded.module_id,
        .introspection = loaded.introspection,
        .dependencies = loaded.dependencies,
        .module_refs = state->module_refs,
        .root = loaded.root,
        .authored_graph = loaded.authored_graph,
    };
    return state;
}

std::unique_ptr<IvModuleDefinitions::NodeTypeState> make_node_type_state(
    IvModuleReloadedNodeType const& loaded)
{
    auto state = std::make_unique<IvModuleDefinitions::NodeTypeState>();
    state->module_refs = loaded.module_refs;
    state->snapshot = IvNodeTypeDefinition{
        .node_type_id = loaded.node_type_id,
        .source_id = loaded.source_id,
        .source_root = normalize_path(loaded.source_root),
        .compiler_record = loaded.compiler_record,
        .module_refs = state->module_refs,
        .authored_graph = loaded.authored_graph,
    };
    return state;
}
} // namespace

IvModuleDefinitions::~IvModuleDefinitions() = default;

void IvModuleDefinitions::emit_notification(
    IvModuleDefinitionsNotification notification) const
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_definitions_notification_event,
        notification);
}

void IvModuleDefinitions::emit_message(
    std::string level,
    std::string message,
    std::filesystem::path module_root) const
{
    emit_notification(IvModuleDefinitionsMessage{
        .level = std::move(level),
        .message = std::move(message),
        .module_root = std::move(module_root),
    });
}

std::string IvModuleDefinitions::declare_definition(
    std::string source_id,
    std::filesystem::path source_root)
{
    IvModuleDefinitionDeclaration declaration{
        .definition_id = std::move(source_id),
        .module_root = normalize_path(source_root),
    };
    bool inserted = false;
    {
        std::scoped_lock lock(mutex);
        auto [position, was_inserted] = declarations_by_source_id.emplace(
            declaration.definition_id, declaration);
        if (!was_inserted) {
            if (position->second.module_root == declaration.module_root) {
                return declaration.definition_id;
            }
            position->second = declaration;
        }
        inserted = was_inserted;
    }
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_definitions_declarations_changed_event,
        IvModuleDefinitionDeclarationsChanged{
            .created = inserted
                ? std::vector<IvModuleDefinitionDeclaration>{declaration}
                : std::vector<IvModuleDefinitionDeclaration>{},
            .updated = inserted
                ? std::vector<IvModuleDefinitionDeclaration>{}
                : std::vector<IvModuleDefinitionDeclaration>{declaration},
        });
    return declaration.definition_id;
}

void IvModuleDefinitions::remove_definition(std::string const& source_id)
{
    IvModuleDefinitionsChanged definition_diff;
    IvNodeTypeDefinitionsChanged node_type_diff;
    std::vector<IvModuleDefinitionsMessage> failures;
    bool removed = false;
    {
        std::scoped_lock lock(mutex);
        removed = declarations_by_source_id.erase(source_id) > 0;
        if (removed) {
            candidates_by_source_id.erase(source_id);
            rebuild_published_registry_locked(
                definition_diff, node_type_diff, failures,
                std::unordered_set<std::string>{source_id});
        }
    }
    if (removed) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_definitions_declarations_changed_event,
            IvModuleDefinitionDeclarationsChanged{
                .deleted_definition_ids = {source_id},
            });
    }
    if (!definition_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_definitions_changed_event, definition_diff);
    }
    if (!node_type_diff.deleted_node_type_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_node_type_definitions_changed_event, node_type_diff);
    }
    for (auto& failure : failures) emit_notification(std::move(failure));
}

void IvModuleDefinitions::handle_required_definitions_changed(
    IvModuleRequiredDefinitionsChanged const& diff)
{
    // Project persistence still requests source packages until its protocol is
    // migrated.  Localize that transitional input here: all published data
    // below is keyed by the registered IV module ID, never by a source.
    for (auto const& required : diff.created) {
        auto const source_root = normalize_path(required.module_root);
        declare_definition(source_root.generic_string(), source_root);
    }
    for (auto const& required : diff.updated) {
        auto const source_root = normalize_path(required.module_root);
        declare_definition(source_root.generic_string(), source_root);
    }
    // Source declarations stay resident after the last instance goes away so
    // the source's complete candidate set remains available for discovery and
    // can be atomically replaced on the next edit.  Publication is still
    // entirely module-ID based.
}

void IvModuleDefinitions::rebuild_published_registry_locked(
    IvModuleDefinitionsChanged& diff,
    IvNodeTypeDefinitionsChanged& node_type_diff,
    std::vector<IvModuleDefinitionsMessage>& failures,
    std::unordered_set<std::string> const& changed_source_ids)
{
    struct ModuleProvider {
        std::string source_id;
        IvModuleReloadedDefinition const* definition = nullptr;
    };
    struct NodeTypeProvider {
        std::string source_id;
        IvModuleReloadedNodeType const* definition = nullptr;
    };
    struct RegisteredProvider {
        std::string source_id;
    };
    std::unordered_map<std::string, std::vector<ModuleProvider>> module_providers;
    std::unordered_map<std::string, std::vector<NodeTypeProvider>> node_type_providers;
    std::unordered_map<std::string, std::vector<RegisteredProvider>> providers_by_id;
    for (auto const& [source_id, candidate] : candidates_by_source_id) {
        if (!declarations_by_source_id.contains(source_id)) continue;
        for (auto const& definition : candidate.modules) {
            module_providers[definition.module_id].push_back({
                .source_id = source_id,
                .definition = &definition,
            });
            providers_by_id[definition.module_id].push_back({.source_id = source_id});
        }
        for (auto const& definition : candidate.node_types) {
            node_type_providers[definition.node_type_id].push_back({
                .source_id = source_id,
                .definition = &definition,
            });
            providers_by_id[definition.node_type_id].push_back({.source_id = source_id});
        }
    }

    std::unordered_set<std::string> preserve_previous;
    for (auto const& [id, providers] : providers_by_id) {
        if (providers.size() == 1) continue;
        auto const previous = source_id_by_registered_id.find(id);
        auto const previous_is_still_a_candidate = previous
                != source_id_by_registered_id.end()
            && std::ranges::any_of(providers, [&](RegisteredProvider const& provider) {
                return provider.source_id == previous->second;
            });
        if (previous_is_still_a_candidate) {
            // Keep the last valid published kind and provider live. The
            // complete candidates remain staged, so either save order for a
            // source-to-source move resolves without rebuilding the other
            // source a second time.
            preserve_previous.insert(id);
            continue;
        }
        if (std::ranges::any_of(providers, [&](RegisteredProvider const& provider) {
                return changed_source_ids.contains(provider.source_id);
            })) {
            auto source = declarations_by_source_id.find(providers.front().source_id);
            failures.push_back({
                .level = "error",
                .message = "registered IV definition ID '" + id
                    + "' is provided by multiple IV sources",
                .module_root = source == declarations_by_source_id.end()
                    ? std::filesystem::path{}
                    : source->second.module_root,
            });
        }
    }

    std::unordered_map<std::string, ModuleProvider> selected_modules;
    for (auto const& [id, providers] : module_providers) {
        if (providers_by_id[id].size() == 1) {
            selected_modules.emplace(id, providers.front());
        }
    }
    std::unordered_map<std::string, NodeTypeProvider> selected_node_types;
    for (auto const& [id, providers] : node_type_providers) {
        if (providers_by_id[id].size() == 1) {
            selected_node_types.emplace(id, providers.front());
        }
    }

    std::unordered_set<std::string> desired_modules;
    for (auto const& [id, _] : selected_modules) desired_modules.insert(id);
    for (auto const& id : preserve_previous) {
        if (loaded_definitions_by_module_id.contains(id)) desired_modules.insert(id);
    }
    for (auto it = loaded_definitions_by_module_id.begin();
         it != loaded_definitions_by_module_id.end();) {
        if (desired_modules.contains(it->first)) {
            ++it;
            continue;
        }
        diff.deleted_definition_ids.push_back(it->first);
        it = loaded_definitions_by_module_id.erase(it);
    }

    for (auto const& [module_id, provider] : selected_modules) {
        auto existing = loaded_definitions_by_module_id.find(module_id);
        auto const requires_publication = existing == loaded_definitions_by_module_id.end()
            || existing->second->snapshot.source_id != provider.source_id
            || changed_source_ids.contains(provider.source_id);
        if (!requires_publication) continue;
        auto state = make_definition_state(*provider.definition);
        auto snapshot = state->snapshot;
        if (existing == loaded_definitions_by_module_id.end()) {
            loaded_definitions_by_module_id.emplace(module_id, std::move(state));
            diff.created.push_back(std::move(snapshot));
        } else {
            existing->second = std::move(state);
            diff.updated.push_back(std::move(snapshot));
        }
    }

    std::unordered_set<std::string> desired_node_types;
    for (auto const& [id, _] : selected_node_types) desired_node_types.insert(id);
    for (auto const& id : preserve_previous) {
        if (loaded_node_types_by_id.contains(id)) desired_node_types.insert(id);
    }
    for (auto it = loaded_node_types_by_id.begin();
         it != loaded_node_types_by_id.end();) {
        if (desired_node_types.contains(it->first)) {
            ++it;
            continue;
        }
        node_type_diff.deleted_node_type_ids.push_back(it->first);
        it = loaded_node_types_by_id.erase(it);
    }
    for (auto const& [node_type_id, provider] : selected_node_types) {
        auto existing = loaded_node_types_by_id.find(node_type_id);
        auto const requires_publication = existing == loaded_node_types_by_id.end()
            || existing->second->snapshot.source_id != provider.source_id
            || changed_source_ids.contains(provider.source_id);
        if (!requires_publication) continue;
        auto state = make_node_type_state(*provider.definition);
        auto snapshot = state->snapshot;
        if (existing == loaded_node_types_by_id.end()) {
            loaded_node_types_by_id.emplace(node_type_id, std::move(state));
            node_type_diff.created.push_back(std::move(snapshot));
        } else {
            existing->second = std::move(state);
            node_type_diff.updated.push_back(std::move(snapshot));
        }
    }

    source_id_by_registered_id.clear();
    module_ids_by_source_id.clear();
    for (auto const& [module_id, state] : loaded_definitions_by_module_id) {
        source_id_by_registered_id[module_id] = state->snapshot.source_id;
        module_ids_by_source_id[state->snapshot.source_id].push_back(module_id);
    }
    for (auto const& [node_type_id, state] : loaded_node_types_by_id) {
        source_id_by_registered_id[node_type_id] = state->snapshot.source_id;
    }
    for (auto& [_, module_ids] : module_ids_by_source_id) {
        std::ranges::sort(module_ids);
    }
}

void IvModuleDefinitions::handle_reload_results(IvModuleReloadResults const& results)
{
    std::unordered_map<std::string, std::vector<IvModuleReloadedDefinition const*>>
        modules_by_source_id;
    for (auto const& loaded : results.loaded) {
        modules_by_source_id[loaded.source_id].push_back(&loaded);
    }
    std::unordered_map<std::string, std::vector<IvModuleReloadedNodeType const*>>
        node_types_by_source_id;
    for (auto const& node_type : results.node_types) {
        node_types_by_source_id[node_type.source_id].push_back(&node_type);
    }

    IvModuleDefinitionsChanged definition_diff;
    IvNodeTypeDefinitionsChanged node_type_diff;
    std::vector<IvModuleDefinitionsMessage> failures;
    {
        std::scoped_lock lock(mutex);
        std::unordered_set<std::string> changed_source_ids;
        for (auto const& source : results.sources) {
            auto declaration = declarations_by_source_id.find(source.definition_id);
            if (declaration == declarations_by_source_id.end()) continue;

            SourceCandidate candidate;
            std::unordered_set<std::string> registered_ids;
            std::string error;
            if (auto modules = modules_by_source_id.find(source.definition_id);
                modules != modules_by_source_id.end()) {
                candidate.modules.reserve(modules->second.size());
                for (auto const* module : modules->second) {
                    if (module->module_id.empty()) {
                        error = "IV source published an IV module with an empty ID";
                        break;
                    }
                    if (!registered_ids.insert(module->module_id).second) {
                        error = "IV source published duplicate registered IV definition ID '"
                            + module->module_id + "'";
                        break;
                    }
                    candidate.modules.push_back(*module);
                }
            }
            if (auto node_types = node_types_by_source_id.find(source.definition_id);
                node_types != node_types_by_source_id.end()) {
                candidate.node_types.reserve(node_types->second.size());
                for (auto const* node_type : node_types->second) {
                    if (node_type->node_type_id.empty()) {
                        error = "IV source published a node type with an empty ID";
                        break;
                    }
                    if (!node_type->authored_graph) {
                        error = "IV source node type '" + node_type->node_type_id
                            + "' has no authored graph";
                        break;
                    }
                    if (!registered_ids.insert(node_type->node_type_id).second) {
                        error = "IV source published duplicate registered IV definition ID '"
                            + node_type->node_type_id + "'";
                        break;
                    }
                    candidate.node_types.push_back(*node_type);
                }
            }
            if (!error.empty()) {
                failures.push_back({
                    .level = "error",
                    .message = std::move(error),
                    .module_root = declaration->second.module_root,
                });
                continue;
            }

            candidates_by_source_id[source.definition_id] = std::move(candidate);
            changed_source_ids.insert(source.definition_id);
        }
        if (!changed_source_ids.empty()) {
            rebuild_published_registry_locked(
                definition_diff, node_type_diff, failures, changed_source_ids);
        }
    }
    for (auto& failure : failures) emit_notification(std::move(failure));
    if (!definition_diff.created.empty() || !definition_diff.updated.empty()
        || !definition_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_definitions_changed_event, definition_diff);
    }
    if (!node_type_diff.created.empty() || !node_type_diff.updated.empty()
        || !node_type_diff.deleted_node_type_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_node_type_definitions_changed_event, node_type_diff);
    }
}

void IvModuleDefinitions::seed_loaded_definition(
    IvModuleReloadedDefinition loaded_definition)
{
    auto const source_id = loaded_definition.source_id.empty()
        ? loaded_definition.definition_id
        : loaded_definition.source_id;
    declare_definition(source_id, loaded_definition.module_root);
    loaded_definition.source_id = source_id;
    IvModuleReloadResults results;
    results.sources.push_back({
        .definition_id = source_id,
        .module_root = loaded_definition.module_root,
    });
    results.loaded.push_back(std::move(loaded_definition));
    handle_reload_results(results);
}

std::vector<IvModuleDefinition> IvModuleDefinitions::loaded_definitions() const
{
    std::vector<IvModuleDefinition> definitions;
    std::scoped_lock lock(mutex);
    definitions.reserve(loaded_definitions_by_module_id.size());
    for (auto const& [_, definition] : loaded_definitions_by_module_id) {
        definitions.push_back(definition->snapshot);
    }
    std::ranges::sort(definitions, {}, &IvModuleDefinition::module_id);
    return definitions;
}

std::vector<IvNodeTypeDefinition> IvModuleDefinitions::loaded_node_types() const
{
    std::vector<IvNodeTypeDefinition> node_types;
    std::scoped_lock lock(mutex);
    node_types.reserve(loaded_node_types_by_id.size());
    for (auto const& [_, node_type] : loaded_node_types_by_id) {
        node_types.push_back(node_type->snapshot);
    }
    std::ranges::sort(node_types, {}, &IvNodeTypeDefinition::node_type_id);
    return node_types;
}
} // namespace iv
