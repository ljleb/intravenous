#include <intravenous/runtime/iv_module_definitions.h>

#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_reload.h>

#include <algorithm>
#include <ranges>
#include <stdexcept>
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

void IvModuleDefinitions::sync_source_declarations(
    std::vector<std::pair<std::string, std::filesystem::path>> declarations)
{
    std::unordered_map<std::string, IvModuleDefinitionDeclaration> next;
    next.reserve(declarations.size());
    for (auto& [source_id, source_root] : declarations) {
        if (source_id.empty()) {
            throw std::runtime_error("discovered IV source has an empty source ID");
        }
        auto declaration = IvModuleDefinitionDeclaration{
            .definition_id = std::move(source_id),
            .module_root = normalize_path(source_root),
        };
        if (!next.emplace(declaration.definition_id, declaration).second) {
            throw std::runtime_error(
                "discovered IV source snapshot contains duplicate source ID '"
                + declaration.definition_id + "'");
        }
    }

    IvModuleDefinitionDeclarationsChanged declaration_diff;
    IvModuleDefinitionsChanged definition_diff;
    IvNodeTypeDefinitionsChanged node_type_diff;
    std::vector<IvModuleDefinitionsMessage> failures;
    {
        std::scoped_lock lock(mutex);
        std::unordered_set<std::string> removed_source_ids;
        for (auto const& declaration : declarations_by_source_id) {
            if (next.contains(declaration.first)) continue;
            declaration_diff.deleted_definition_ids.push_back(declaration.first);
            removed_source_ids.insert(declaration.first);
        }
        for (auto const& [source_id, declaration] : next) {
            auto const current = declarations_by_source_id.find(source_id);
            if (current == declarations_by_source_id.end()) {
                declaration_diff.created.push_back(declaration);
            } else if (current->second.module_root != declaration.module_root) {
                declaration_diff.updated.push_back(declaration);
            }
        }

        for (auto const& source_id : removed_source_ids) {
            candidates_by_source_id.erase(source_id);
        }
        declarations_by_source_id = std::move(next);
        if (!removed_source_ids.empty()) {
            rebuild_published_registry_locked(
                definition_diff, node_type_diff, failures, removed_source_ids);
        }
    }

    if (!declaration_diff.created.empty() || !declaration_diff.updated.empty()
        || !declaration_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_definitions_declarations_changed_event,
            declaration_diff);
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

    // Candidate validation is generation-wide. Never select or update an ID
    // independently: a collision in any source candidate leaves every live
    // map and ownership record on the previous complete generation.
    bool valid = true;
    for (auto const& [id, providers] : providers_by_id) {
        if (providers.size() == 1) continue;
        valid = false;
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
    if (!valid) return;

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

    std::unordered_map<std::string, std::unique_ptr<DefinitionState>> next_modules;
    next_modules.reserve(selected_modules.size());
    for (auto const& [module_id, provider] : selected_modules) {
        auto existing = loaded_definitions_by_module_id.find(module_id);
        auto const requires_publication = existing == loaded_definitions_by_module_id.end()
            || existing->second->snapshot.source_id != provider.source_id
            || changed_source_ids.contains(provider.source_id);
        auto state = make_definition_state(*provider.definition);
        auto snapshot = state->snapshot;
        if (existing == loaded_definitions_by_module_id.end()) {
            diff.created.push_back(std::move(snapshot));
        } else if (requires_publication) {
            diff.updated.push_back(std::move(snapshot));
        }
        next_modules.emplace(module_id, std::move(state));
    }

    for (auto const& [module_id, _] : loaded_definitions_by_module_id) {
        if (!next_modules.contains(module_id)) {
            diff.deleted_definition_ids.push_back(module_id);
        }
    }

    std::unordered_map<std::string, std::unique_ptr<NodeTypeState>> next_node_types;
    next_node_types.reserve(selected_node_types.size());
    for (auto const& [node_type_id, provider] : selected_node_types) {
        auto existing = loaded_node_types_by_id.find(node_type_id);
        auto const requires_publication = existing == loaded_node_types_by_id.end()
            || existing->second->snapshot.source_id != provider.source_id
            || changed_source_ids.contains(provider.source_id);
        auto state = make_node_type_state(*provider.definition);
        auto snapshot = state->snapshot;
        if (existing == loaded_node_types_by_id.end()) {
            node_type_diff.created.push_back(std::move(snapshot));
        } else if (requires_publication) {
            node_type_diff.updated.push_back(std::move(snapshot));
        }
        next_node_types.emplace(node_type_id, std::move(state));
    }

    for (auto const& [node_type_id, _] : loaded_node_types_by_id) {
        if (!next_node_types.contains(node_type_id)) {
            node_type_diff.deleted_node_type_ids.push_back(node_type_id);
        }
    }

    std::unordered_map<std::string, std::string> next_owners;
    std::unordered_map<std::string, std::vector<std::string>> next_modules_by_source;
    for (auto const& [module_id, state] : next_modules) {
        next_owners[module_id] = state->snapshot.source_id;
        next_modules_by_source[state->snapshot.source_id].push_back(module_id);
    }
    for (auto const& [node_type_id, state] : next_node_types) {
        next_owners[node_type_id] = state->snapshot.source_id;
    }
    for (auto& [_, module_ids] : next_modules_by_source) {
        std::ranges::sort(module_ids);
    }

    // One atomic state transition: notifications above describe exactly this
    // complete snapshot, never a mixture of old and candidate providers.
    loaded_definitions_by_module_id = std::move(next_modules);
    loaded_node_types_by_id = std::move(next_node_types);
    source_id_by_registered_id = std::move(next_owners);
    module_ids_by_source_id = std::move(next_modules_by_source);
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

std::optional<std::filesystem::path> IvModuleDefinitions::source_root_for_module(
    std::string const& module_id) const
{
    std::scoped_lock lock(mutex);
    auto const definition = loaded_definitions_by_module_id.find(module_id);
    if (definition == loaded_definitions_by_module_id.end()) return std::nullopt;
    return definition->second->snapshot.module_root;
}

std::vector<std::string> IvModuleDefinitions::module_ids_for_source(
    std::string const& source_id) const
{
    std::scoped_lock lock(mutex);
    auto const modules = module_ids_by_source_id.find(source_id);
    return modules == module_ids_by_source_id.end()
        ? std::vector<std::string>{}
        : modules->second;
}

std::vector<std::string> IvModuleDefinitions::node_type_ids_for_source(
    std::string const& source_id) const
{
    std::vector<std::string> node_type_ids;
    std::scoped_lock lock(mutex);
    for (auto const& [id, state] : loaded_node_types_by_id) {
        if (state->snapshot.source_id == source_id) {
            node_type_ids.push_back(id);
        }
    }
    std::ranges::sort(node_type_ids);
    return node_type_ids;
}
} // namespace iv
