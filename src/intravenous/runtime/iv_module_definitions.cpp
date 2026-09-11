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
        .package_id = loaded.package_id,
        .package_root = normalize_path(loaded.package_root),
        .module_id = loaded.module_id,
        .introspection = loaded.introspection,
        .dependencies = loaded.dependencies,
        .module_refs = state->module_refs,
        .root = loaded.root,
        .configured_graph = loaded.configured_graph,
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
        .package_id = loaded.package_id,
        .package_root = normalize_path(loaded.package_root),
        .compiler_record = loaded.compiler_record,
        .module_refs = state->module_refs,
        .configured_graph = loaded.configured_graph,
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
    std::filesystem::path package_root) const
{
    emit_notification(IvModuleDefinitionsMessage{
        .level = std::move(level),
        .message = std::move(message),
        .package_root = std::move(package_root),
    });
}

std::string IvModuleDefinitions::declare_package(
    std::string package_id,
    std::filesystem::path package_root)
{
    IvPackageDeclaration declaration{
        .package_id = std::move(package_id),
        .package_root = normalize_path(package_root),
    };
    bool inserted = false;
    {
        std::scoped_lock lock(mutex);
        auto [position, was_inserted] = declarations_by_package_id.emplace(
            declaration.package_id, declaration);
        if (!was_inserted) {
            if (position->second.package_root == declaration.package_root) {
                return declaration.package_id;
            }
            position->second = declaration;
        }
        inserted = was_inserted;
    }
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_package_declarations_changed_event,
        IvPackageDeclarationsChanged{
            .created = inserted
                ? std::vector<IvPackageDeclaration>{declaration}
                : std::vector<IvPackageDeclaration>{},
            .updated = inserted
                ? std::vector<IvPackageDeclaration>{}
                : std::vector<IvPackageDeclaration>{declaration},
        });
    return declaration.package_id;
}

void IvModuleDefinitions::sync_package_declarations(
    std::vector<std::pair<std::string, std::filesystem::path>> declarations)
{
    std::unordered_map<std::string, IvPackageDeclaration> next;
    next.reserve(declarations.size());
    for (auto& [package_id, package_root] : declarations) {
        if (package_id.empty()) {
            throw std::runtime_error("discovered IV package has an empty package ID");
        }
        auto declaration = IvPackageDeclaration{
            .package_id = std::move(package_id),
            .package_root = normalize_path(package_root),
        };
        if (!next.emplace(declaration.package_id, declaration).second) {
            throw std::runtime_error(
                "discovered IV package snapshot contains duplicate package ID '"
                + declaration.package_id + "'");
        }
    }

    IvPackageDeclarationsChanged declaration_diff;
    IvModuleDefinitionsChanged definition_diff;
    IvNodeTypeDefinitionsChanged node_type_diff;
    std::vector<IvModuleDefinitionsMessage> failures;
    {
        std::scoped_lock lock(mutex);
        std::unordered_set<std::string> removed_package_ids;
        for (auto const& declaration : declarations_by_package_id) {
            if (next.contains(declaration.first)) continue;
            declaration_diff.deleted_package_ids.push_back(declaration.first);
            removed_package_ids.insert(declaration.first);
        }
        for (auto const& [package_id, declaration] : next) {
            auto const current = declarations_by_package_id.find(package_id);
            if (current == declarations_by_package_id.end()) {
                declaration_diff.created.push_back(declaration);
            } else if (current->second.package_root != declaration.package_root) {
                declaration_diff.updated.push_back(declaration);
            }
        }

        for (auto const& package_id : removed_package_ids) {
            candidates_by_package_id.erase(package_id);
        }
        declarations_by_package_id = std::move(next);
        if (!removed_package_ids.empty()) {
            rebuild_published_registry_locked(
                definition_diff, node_type_diff, failures, removed_package_ids);
        }
    }

    if (!declaration_diff.created.empty() || !declaration_diff.updated.empty()
        || !declaration_diff.deleted_package_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_package_declarations_changed_event,
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

void IvModuleDefinitions::remove_package(std::string const& package_id)
{
    IvModuleDefinitionsChanged definition_diff;
    IvNodeTypeDefinitionsChanged node_type_diff;
    std::vector<IvModuleDefinitionsMessage> failures;
    bool removed = false;
    {
        std::scoped_lock lock(mutex);
        removed = declarations_by_package_id.erase(package_id) > 0;
        if (removed) {
            candidates_by_package_id.erase(package_id);
            rebuild_published_registry_locked(
                definition_diff, node_type_diff, failures,
                std::unordered_set<std::string>{package_id});
        }
    }
    if (removed) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_package_declarations_changed_event,
            IvPackageDeclarationsChanged{
                .deleted_package_ids = {package_id},
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
    // Project persistence requests package roots for required module definitions.
    // Published data remains keyed by stable IV module ID and explicit package ID.
    for (auto const& required : diff.created) {
        auto const package_root = normalize_path(required.package_root);
        declare_package(package_root.generic_string(), package_root);
    }
    for (auto const& required : diff.updated) {
        auto const package_root = normalize_path(required.package_root);
        declare_package(package_root.generic_string(), package_root);
    }
    // Package declarations stay resident after the last instance goes away so the
    // package's complete candidate set can be atomically replaced on the next edit.
}

void IvModuleDefinitions::handle_iv_module_definition_lookup(
    std::string const& definition_id,
    IvModuleDefinitionLookupBuilder& builder) const
{
    std::scoped_lock lock(mutex);
    auto const definition = loaded_definitions_by_module_id.find(definition_id);
    builder.succeed(definition == loaded_definitions_by_module_id.end()
        ? std::optional<IvModuleDefinition>{}
        : std::optional<IvModuleDefinition>{definition->second->snapshot});
}

void IvModuleDefinitions::rebuild_published_registry_locked(
    IvModuleDefinitionsChanged& diff,
    IvNodeTypeDefinitionsChanged& node_type_diff,
    std::vector<IvModuleDefinitionsMessage>& failures,
    std::unordered_set<std::string> const& changed_package_ids)
{
    struct ModuleProvider {
        std::string package_id;
        IvModuleReloadedDefinition const* definition = nullptr;
    };
    struct NodeTypeProvider {
        std::string package_id;
        IvModuleReloadedNodeType const* definition = nullptr;
    };
    struct RegisteredProvider {
        std::string package_id;
    };
    std::unordered_map<std::string, std::vector<ModuleProvider>> module_providers;
    std::unordered_map<std::string, std::vector<NodeTypeProvider>> node_type_providers;
    std::unordered_map<std::string, std::vector<RegisteredProvider>> providers_by_id;
    for (auto const& [package_id, candidate] : candidates_by_package_id) {
        if (!declarations_by_package_id.contains(package_id)) continue;
        for (auto const& definition : candidate.modules) {
            module_providers[definition.module_id].push_back({
                .package_id = package_id,
                .definition = &definition,
            });
            providers_by_id[definition.module_id].push_back({.package_id = package_id});
        }
        for (auto const& definition : candidate.node_types) {
            node_type_providers[definition.node_type_id].push_back({
                .package_id = package_id,
                .definition = &definition,
            });
            providers_by_id[definition.node_type_id].push_back({.package_id = package_id});
        }
    }

    // Validate the complete package candidate set before publishing any ID. A
    // collision leaves all published maps and ownership records unchanged.
    bool valid = true;
    for (auto const& [id, providers] : providers_by_id) {
        if (providers.size() == 1) continue;
        valid = false;
        if (std::ranges::any_of(providers, [&](RegisteredProvider const& provider) {
                return changed_package_ids.contains(provider.package_id);
            })) {
            auto package = declarations_by_package_id.find(providers.front().package_id);
            failures.push_back({
                .level = "error",
                .message = "IV package definition ID '" + id
                    + "' is provided by multiple IV packages",
                .package_root = package == declarations_by_package_id.end()
                    ? std::filesystem::path{}
                    : package->second.package_root,
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
            || existing->second->snapshot.package_id != provider.package_id
            || changed_package_ids.contains(provider.package_id);
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
            || existing->second->snapshot.package_id != provider.package_id
            || changed_package_ids.contains(provider.package_id);
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
    std::unordered_map<std::string, std::vector<std::string>> next_modules_by_package;
    for (auto const& [module_id, state] : next_modules) {
        next_owners[module_id] = state->snapshot.package_id;
        next_modules_by_package[state->snapshot.package_id].push_back(module_id);
    }
    for (auto const& [node_type_id, state] : next_node_types) {
        next_owners[node_type_id] = state->snapshot.package_id;
    }
    for (auto& [_, module_ids] : next_modules_by_package) {
        std::ranges::sort(module_ids);
    }

    // One atomic state transition: notifications above describe exactly this
    // complete snapshot, never a mixture of old and candidate providers.
    loaded_definitions_by_module_id = std::move(next_modules);
    loaded_node_types_by_id = std::move(next_node_types);
    package_id_by_definition_id = std::move(next_owners);
    module_ids_by_package_id = std::move(next_modules_by_package);
}

void IvModuleDefinitions::handle_reload_results(IvModuleReloadResults const& results)
{
    std::unordered_map<std::string, std::vector<IvModuleReloadedDefinition const*>>
        modules_by_package_id;
    for (auto const& loaded : results.loaded) {
        modules_by_package_id[loaded.package_id].push_back(&loaded);
    }
    std::unordered_map<std::string, std::vector<IvModuleReloadedNodeType const*>>
        node_types_by_package_id;
    for (auto const& node_type : results.node_types) {
        node_types_by_package_id[node_type.package_id].push_back(&node_type);
    }

    IvModuleDefinitionsChanged definition_diff;
    IvNodeTypeDefinitionsChanged node_type_diff;
    std::vector<IvModuleDefinitionsMessage> failures;
    {
        std::scoped_lock lock(mutex);
        std::unordered_set<std::string> changed_package_ids;
        for (auto const& package : results.packages) {
            auto declaration = declarations_by_package_id.find(package.package_id);
            if (declaration == declarations_by_package_id.end()) continue;

            PackageCandidate candidate;
            std::unordered_set<std::string> definition_ids;
            std::string error;
            if (auto modules = modules_by_package_id.find(package.package_id);
                modules != modules_by_package_id.end()) {
                candidate.modules.reserve(modules->second.size());
                for (auto const* module : modules->second) {
                    if (module->module_id.empty()) {
                        error = "IV package published an IV module with an empty ID";
                        break;
                    }
                    if (!definition_ids.insert(module->module_id).second) {
                        error = "IV package published duplicate IV definition ID '"
                            + module->module_id + "'";
                        break;
                    }
                    candidate.modules.push_back(*module);
                }
            }
            if (auto node_types = node_types_by_package_id.find(package.package_id);
                node_types != node_types_by_package_id.end()) {
                candidate.node_types.reserve(node_types->second.size());
                for (auto const* node_type : node_types->second) {
                    if (node_type->node_type_id.empty()) {
                        error = "IV package published a node type with an empty ID";
                        break;
                    }
                    if (!node_type->configured_graph) {
                        error = "IV package node type '" + node_type->node_type_id
                            + "' has no configured graph";
                        break;
                    }
                    if (!definition_ids.insert(node_type->node_type_id).second) {
                        error = "IV package published duplicate IV definition ID '"
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
                    .package_root = declaration->second.package_root,
                });
                continue;
            }

            candidates_by_package_id[package.package_id] = std::move(candidate);
            changed_package_ids.insert(package.package_id);
        }
        if (!changed_package_ids.empty()) {
            rebuild_published_registry_locked(
                definition_diff, node_type_diff, failures, changed_package_ids);
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
    auto const package_id = loaded_definition.package_id.empty()
        ? loaded_definition.definition_id
        : loaded_definition.package_id;
    declare_package(package_id, loaded_definition.package_root);
    loaded_definition.package_id = package_id;
    IvModuleReloadResults results;
    results.packages.push_back({
        .package_id = package_id,
        .package_root = loaded_definition.package_root,
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

std::optional<std::filesystem::path> IvModuleDefinitions::package_root_for_module(
    std::string const& module_id) const
{
    std::scoped_lock lock(mutex);
    auto const definition = loaded_definitions_by_module_id.find(module_id);
    if (definition == loaded_definitions_by_module_id.end()) return std::nullopt;
    return definition->second->snapshot.package_root;
}

std::vector<std::string> IvModuleDefinitions::module_ids_for_package(
    std::string const& package_id) const
{
    std::scoped_lock lock(mutex);
    auto const modules = module_ids_by_package_id.find(package_id);
    return modules == module_ids_by_package_id.end()
        ? std::vector<std::string>{}
        : modules->second;
}

std::vector<std::string> IvModuleDefinitions::node_type_ids_for_package(
    std::string const& package_id) const
{
    std::vector<std::string> node_type_ids;
    std::scoped_lock lock(mutex);
    for (auto const& [id, state] : loaded_node_types_by_id) {
        if (state->snapshot.package_id == package_id) {
            node_type_ids.push_back(id);
        }
    }
    std::ranges::sort(node_type_ids);
    return node_type_ids;
}
} // namespace iv
