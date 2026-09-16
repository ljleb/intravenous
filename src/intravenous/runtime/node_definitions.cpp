#include <intravenous/runtime/node_definitions.h>

#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_package_reload.h>

#include <algorithm>
#include <cstddef>
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

std::unique_ptr<NodeDefinitions::ModuleDefinitionState> make_module_definition_state(
    IvPackageReloadedDefinition const& loaded,
    std::uint64_t version)
{
    auto state = std::make_unique<NodeDefinitions::ModuleDefinitionState>();
    state->module_refs = loaded.module_refs;
    state->version = version;
    state->snapshot = ModuleNodeDefinition{
        .definition_id = loaded.definition_id,
        .package_id = loaded.package_id,
        .package_root = normalize_path(loaded.package_root),
        .module_id = loaded.module_id,
        .provider = loaded.provider,
        .introspection = loaded.introspection,
        .dependencies = loaded.dependencies,
        .module_refs = state->module_refs,
        .root = loaded.root,
        .configured_graph = loaded.configured_graph,
    };
    return state;
}

std::unique_ptr<NodeDefinitions::LeafDefinitionState> make_leaf_definition_state(
    IvPackageReloadedNodeType const& loaded,
    std::uint64_t version)
{
    auto state = std::make_unique<NodeDefinitions::LeafDefinitionState>();
    state->module_refs = loaded.module_refs;
    state->version = version;
    state->snapshot = LeafNodeDefinition{
        .definition_id = loaded.node_type_id,
        .package_id = loaded.package_id,
        .package_root = normalize_path(loaded.package_root),
        .provider = loaded.provider,
        .compiler_record = loaded.compiler_record,
        .module_refs = state->module_refs,
    };
    return state;
}
} // namespace

NodeDefinitions::NodeDefinitions()
    : definitions_snapshot_(std::make_shared<NodeDefinitionsSnapshot const>())
{}

NodeDefinitions::~NodeDefinitions() = default;

std::unordered_map<std::string, IvPackageDeclaration>
NodeDefinitions::merge_declaration_sources_locked(
    std::unordered_map<std::string, IvPackageDeclaration> const& retained,
    std::unordered_map<std::string, IvPackageDeclaration> const& discovered) const
{
    auto merged = retained;
    for (auto const& [package_id, declaration] : discovered) {
        auto const [position, inserted] = merged.emplace(package_id, declaration);
        if (!inserted && position->second.package_root != declaration.package_root) {
            throw std::runtime_error(
                "IV package declaration sources disagree about package ID '"
                + package_id + "'");
        }
    }
    return merged;
}

void NodeDefinitions::publish_package_definitions_changed(
    ModuleNodeDefinitionsChanged modules,
    LeafNodeDefinitionsChanged leaf_definitions,
    bool force) const
{
    auto const has_module_changes = !modules.created.empty()
        || !modules.updated.empty()
        || !modules.deleted_definition_ids.empty();
    auto const has_leaf_changes = !leaf_definitions.created.empty()
        || !leaf_definitions.updated.empty()
        || !leaf_definitions.deleted_definition_ids.empty();
    if (!has_module_changes && !has_leaf_changes && !force) {
        return;
    }
    if (has_module_changes || has_leaf_changes) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_node_definitions_snapshot_changed_event,
            NodeDefinitionsSnapshotChanged{.snapshot = snapshot()});
    }
    std::unordered_map<std::string, std::string> publication_messages;
    for (auto const &snapshot : package_definition_snapshots()) {
        if (!snapshot.publication_message.empty()) {
            publication_messages.emplace(
                snapshot.declaration.package_id,
                snapshot.publication_message);
        }
    }
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_package_definitions_changed_event,
        IvPackageDefinitionsChanged{
            .module_definitions = std::move(modules),
            .leaf_definitions = std::move(leaf_definitions),
            .publication_messages_by_package_id = std::move(publication_messages),
        });
}

void NodeDefinitions::declare_packages(
    std::vector<IvPackageDeclaration> declarations)
{
    std::unordered_map<std::string, IvPackageDeclaration> declarations_by_id;
    declarations_by_id.reserve(declarations.size());
    for (auto& declaration : declarations) {
        if (declaration.package_id.empty()) {
            throw std::runtime_error("IV package declaration has an empty package ID");
        }
        declaration.package_root = normalize_path(declaration.package_root);
        auto const position = declarations_by_id.find(declaration.package_id);
        if (position == declarations_by_id.end()) {
            declarations_by_id.emplace(declaration.package_id, declaration);
        } else if (position->second.package_root != declaration.package_root) {
            throw std::runtime_error(
                "IV package declaration batch contains conflicting package ID '"
                + position->first + "'");
        }
    }

    IvPackageDeclarationsChanged diff;
    {
        std::scoped_lock lock(mutex);
        auto next_retained = retained_package_declarations_by_id;
        for (auto& [package_id, declaration] : declarations_by_id) {
            next_retained.insert_or_assign(package_id, declaration);
        }
        auto const next = merge_declaration_sources_locked(
            next_retained,
            discovered_package_declarations_by_id);
        for (auto const& [package_id, declaration] : next) {
            auto [position, inserted] = declarations_by_package_id.emplace(
                package_id, declaration);
            if (inserted) {
                diff.created.push_back(declaration);
                continue;
            }
            if (position->second.package_root == declaration.package_root) continue;
            position->second = declaration;
            diff.updated.push_back(declaration);
        }
        retained_package_declarations_by_id = std::move(next_retained);
    }

    if (!diff.created.empty() || !diff.updated.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_package_declarations_changed_event,
            diff);
    }
}

std::string NodeDefinitions::declare_package(
    std::string package_id,
    std::filesystem::path package_root)
{
    auto result = package_id;
    declare_packages({IvPackageDeclaration{
        .package_id = std::move(package_id),
        .package_root = std::move(package_root),
    }});
    return result;
}

void NodeDefinitions::sync_package_declarations(
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
    ModuleNodeDefinitionsChanged definition_diff;
    LeafNodeDefinitionsChanged leaf_diff;
    {
        std::scoped_lock lock(mutex);
        auto const effective = merge_declaration_sources_locked(
            retained_package_declarations_by_id,
            next);
        std::unordered_set<std::string> removed_package_ids;
        for (auto const& declaration : declarations_by_package_id) {
            if (effective.contains(declaration.first)) continue;
            declaration_diff.deleted_package_ids.push_back(declaration.first);
            removed_package_ids.insert(declaration.first);
        }
        for (auto const& [package_id, declaration] : effective) {
            auto const current = declarations_by_package_id.find(package_id);
            if (current == declarations_by_package_id.end()) {
                declaration_diff.created.push_back(declaration);
            } else if (current->second.package_root != declaration.package_root) {
                declaration_diff.updated.push_back(declaration);
            }
        }

        for (auto const& package_id : removed_package_ids) {
            candidates_by_package_id.erase(package_id);
            candidate_validation_messages_by_package_id.erase(package_id);
        }
        discovered_package_declarations_by_id = std::move(next);
        declarations_by_package_id = std::move(effective);
        if (!removed_package_ids.empty()) {
            rebuild_published_registry_locked(
                definition_diff, leaf_diff, removed_package_ids);
        }
    }

    if (!declaration_diff.created.empty() || !declaration_diff.updated.empty()
        || !declaration_diff.deleted_package_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_package_declarations_changed_event,
            declaration_diff);
    }
    publish_package_definitions_changed(
        std::move(definition_diff),
        std::move(leaf_diff));
}

void NodeDefinitions::remove_package(std::string const& package_id)
{
    ModuleNodeDefinitionsChanged definition_diff;
    LeafNodeDefinitionsChanged leaf_diff;
    bool removed = false;
    {
        std::scoped_lock lock(mutex);
        retained_package_declarations_by_id.erase(package_id);
        discovered_package_declarations_by_id.erase(package_id);
        removed = declarations_by_package_id.erase(package_id) > 0;
        if (removed) {
            candidates_by_package_id.erase(package_id);
            candidate_validation_messages_by_package_id.erase(package_id);
            rebuild_published_registry_locked(
                definition_diff, leaf_diff,
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
    publish_package_definitions_changed(
        std::move(definition_diff),
        std::move(leaf_diff));
}

void NodeDefinitions::handle_required_definitions_changed(
    IvModuleRequiredDefinitionsChanged const& diff)
{
    // One required-definitions event is one source-event batch. Consolidate all
    // package declarations before propagating so IvPackageReload is entered at most
    // once for this event, even when several definitions become required together.
    std::vector<IvPackageDeclaration> declarations;
    declarations.reserve(diff.created.size() + diff.updated.size());
    auto append_required = [&](IvModuleRequiredDefinition const& required) {
        auto package_root = normalize_path(required.package_root);
        declarations.push_back(IvPackageDeclaration{
            .package_id = package_root.generic_string(),
            .package_root = std::move(package_root),
        });
    };
    for (auto const& required : diff.created) append_required(required);
    for (auto const& required : diff.updated) append_required(required);
    declare_packages(std::move(declarations));

    // Package declarations stay resident after the last instance goes away so the
    // package's complete candidate set can be atomically replaced on the next edit.
}

void NodeDefinitions::rebuild_published_registry_locked(
    ModuleNodeDefinitionsChanged& diff,
    LeafNodeDefinitionsChanged& leaf_diff,
    std::unordered_set<std::string> const& changed_package_ids)
{
    struct ModuleProvider {
        std::string package_id;
        IvPackageReloadedDefinition const* definition = nullptr;
    };
    struct LeafProvider {
        std::string package_id;
        IvPackageReloadedNodeType const* definition = nullptr;
    };
    std::unordered_map<std::string, std::vector<ModuleProvider>> module_providers;
    std::unordered_map<std::string, std::vector<LeafProvider>> leaf_providers;
    std::unordered_map<std::string, std::size_t> provider_count_by_id;
    for (auto const& [package_id, candidate] : candidates_by_package_id) {
        if (!declarations_by_package_id.contains(package_id)) continue;
        for (auto const& definition : candidate.module_definitions) {
            module_providers[definition.module_id].push_back({
                .package_id = package_id,
                .definition = &definition,
            });
            ++provider_count_by_id[definition.module_id];
        }
        for (auto const& definition : candidate.leaf_definitions) {
            leaf_providers[definition.node_type_id].push_back({
                .package_id = package_id,
                .definition = &definition,
            });
            ++provider_count_by_id[definition.node_type_id];
        }
    }

    // Validate the complete package candidate set before publishing any ID. A
    // collision leaves all published maps and ownership records unchanged.
    // Desired project instances do not participate in this validation: a
    // successfully removed definition must publish so the desired instance can
    // remain visible as unrealized and the user can remove it from the project.
    bool valid = true;
    for (auto const& [_, provider_count] : provider_count_by_id) {
        if (provider_count == 1) continue;
        valid = false;
    }
    if (!valid) return;

    std::unordered_map<std::string, ModuleProvider> selected_modules;
    for (auto const& [id, providers] : module_providers) {
        if (provider_count_by_id[id] == 1) {
            selected_modules.emplace(id, providers.front());
        }
    }
    std::unordered_map<std::string, LeafProvider> selected_leaf_definitions;
    for (auto const& [id, providers] : leaf_providers) {
        if (provider_count_by_id[id] == 1) {
            selected_leaf_definitions.emplace(id, providers.front());
        }
    }

    std::unordered_map<std::string, std::unique_ptr<ModuleDefinitionState>> next_modules;
    next_modules.reserve(selected_modules.size());
    for (auto const& [module_id, provider] : selected_modules) {
        auto existing = loaded_module_definitions_by_id.find(module_id);
        auto const requires_publication = existing == loaded_module_definitions_by_id.end()
            || existing->second->snapshot.package_id != provider.package_id
            || changed_package_ids.contains(provider.package_id);
        auto const version = requires_publication
            ? next_definition_version_++
            : existing->second->version;
        auto state = make_module_definition_state(*provider.definition, version);
        auto snapshot = state->snapshot;
        if (existing == loaded_module_definitions_by_id.end()) {
            diff.created.push_back(std::move(snapshot));
        } else if (requires_publication) {
            diff.updated.push_back(std::move(snapshot));
        }
        next_modules.emplace(module_id, std::move(state));
    }

    for (auto const& [module_id, _] : loaded_module_definitions_by_id) {
        if (!next_modules.contains(module_id)) {
            diff.deleted_definition_ids.push_back(module_id);
        }
    }

    std::unordered_map<std::string, std::unique_ptr<LeafDefinitionState>> next_leaf_definitions;
    next_leaf_definitions.reserve(selected_leaf_definitions.size());
    for (auto const& [node_type_id, provider] : selected_leaf_definitions) {
        auto existing = loaded_leaf_definitions_by_id.find(node_type_id);
        auto const requires_publication = existing == loaded_leaf_definitions_by_id.end()
            || existing->second->snapshot.package_id != provider.package_id
            || changed_package_ids.contains(provider.package_id);
        auto const version = requires_publication
            ? next_definition_version_++
            : existing->second->version;
        auto state = make_leaf_definition_state(*provider.definition, version);
        auto snapshot = state->snapshot;
        if (existing == loaded_leaf_definitions_by_id.end()) {
            leaf_diff.created.push_back(std::move(snapshot));
        } else if (requires_publication) {
            leaf_diff.updated.push_back(std::move(snapshot));
        }
        next_leaf_definitions.emplace(node_type_id, std::move(state));
    }

    for (auto const& [node_type_id, _] : loaded_leaf_definitions_by_id) {
        if (!next_leaf_definitions.contains(node_type_id)) {
            leaf_diff.deleted_definition_ids.push_back(node_type_id);
        }
    }

    std::unordered_map<std::string, std::string> next_owners;
    std::unordered_map<std::string, std::vector<std::string>> next_modules_by_package;
    for (auto const& [module_id, state] : next_modules) {
        next_owners[module_id] = state->snapshot.package_id;
        next_modules_by_package[state->snapshot.package_id].push_back(module_id);
    }
    for (auto const& [node_type_id, state] : next_leaf_definitions) {
        next_owners[node_type_id] = state->snapshot.package_id;
    }
    for (auto& [_, module_ids] : next_modules_by_package) {
        std::ranges::sort(module_ids);
    }

    // One atomic state transition: downstream consumers see exactly this
    // complete snapshot, never a mixture of old and candidate providers.
    loaded_module_definitions_by_id = std::move(next_modules);
    loaded_leaf_definitions_by_id = std::move(next_leaf_definitions);
    package_id_by_definition_id = std::move(next_owners);
    module_definition_ids_by_package_id = std::move(next_modules_by_package);

    auto const registry_changed = !diff.created.empty() || !diff.updated.empty()
        || !diff.deleted_definition_ids.empty() || !leaf_diff.created.empty()
        || !leaf_diff.updated.empty()
        || !leaf_diff.deleted_definition_ids.empty();
    if (!registry_changed) return;

    auto next_snapshot = std::make_shared<NodeDefinitionsSnapshot>();
    next_snapshot->generation = ++snapshot_generation_;
    next_snapshot->by_id.reserve(
        loaded_module_definitions_by_id.size() + loaded_leaf_definitions_by_id.size());
    for (auto const& [definition_id, state] : loaded_module_definitions_by_id) {
        next_snapshot->by_id.emplace(
            definition_id,
            NodeDefinitionEntry{
                .definition_id = definition_id,
                .kind = NodeDefinitionKind::module,
                .version = state->version,
                .definition = state->snapshot,
            });
    }
    for (auto const& [definition_id, state] : loaded_leaf_definitions_by_id) {
        next_snapshot->by_id.emplace(
            definition_id,
            NodeDefinitionEntry{
                .definition_id = definition_id,
                .kind = NodeDefinitionKind::leaf,
                .version = state->version,
                .definition = state->snapshot,
            });
    }
    definitions_snapshot_ = std::move(next_snapshot);
}

void NodeDefinitions::handle_reload_results(IvPackageReloadResults const& results)
{
    std::unordered_map<std::string, std::vector<IvPackageReloadedDefinition const*>>
        modules_by_package_id;
    for (auto const& loaded : results.loaded) {
        modules_by_package_id[loaded.package_id].push_back(&loaded);
    }
    std::unordered_map<std::string, std::vector<IvPackageReloadedNodeType const*>>
        leaf_definitions_by_package_id;
    for (auto const& node_type : results.node_types) {
        leaf_definitions_by_package_id[node_type.package_id].push_back(&node_type);
    }

    ModuleNodeDefinitionsChanged definition_diff;
    LeafNodeDefinitionsChanged leaf_diff;
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
                candidate.module_definitions.reserve(modules->second.size());
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
                    candidate.module_definitions.push_back(*module);
                }
            }
            if (auto leaf_definitions =
                    leaf_definitions_by_package_id.find(package.package_id);
                leaf_definitions != leaf_definitions_by_package_id.end()) {
                candidate.leaf_definitions.reserve(leaf_definitions->second.size());
                for (auto const* node_type : leaf_definitions->second) {
                    if (node_type->node_type_id.empty()) {
                        error = "IV package published a node type with an empty ID";
                        break;
                    }
                    if (!definition_ids.insert(node_type->node_type_id).second) {
                        error = "IV package published duplicate IV definition ID '"
                            + node_type->node_type_id + "'";
                        break;
                    }
                    candidate.leaf_definitions.push_back(*node_type);
                }
            }
            if (!error.empty()) {
                candidate_validation_messages_by_package_id[package.package_id] = error;
                continue;
            }

            candidate_validation_messages_by_package_id.erase(package.package_id);
            candidates_by_package_id[package.package_id] = std::move(candidate);
            changed_package_ids.insert(package.package_id);
        }
        if (!changed_package_ids.empty()) {
            rebuild_published_registry_locked(
                definition_diff, leaf_diff, changed_package_ids);
        }
    }
    publish_package_definitions_changed(
        std::move(definition_diff),
        std::move(leaf_diff),
        !results.packages.empty());
}

void NodeDefinitions::seed_loaded_definition(
    IvPackageReloadedDefinition loaded_definition)
{
    auto const package_id = loaded_definition.package_id.empty()
        ? loaded_definition.definition_id
        : loaded_definition.package_id;
    declare_package(package_id, loaded_definition.package_root);
    loaded_definition.package_id = package_id;
    IvPackageReloadResults results;
    results.packages.push_back({
        .package_id = package_id,
        .package_root = loaded_definition.package_root,
    });
    results.loaded.push_back(std::move(loaded_definition));
    handle_reload_results(results);
}

std::vector<IvPackageDefinitionSnapshot>
NodeDefinitions::package_definition_snapshots() const
{
    std::vector<IvPackageDefinitionSnapshot> snapshots;
    std::scoped_lock lock(mutex);

    std::unordered_map<std::string, std::vector<std::string>> candidate_ids_by_package;
    std::unordered_map<std::string, std::vector<std::string>> candidate_packages_by_id;
    for (auto const& [package_id, candidate] : candidates_by_package_id) {
        if (!declarations_by_package_id.contains(package_id)) continue;
        auto& ids = candidate_ids_by_package[package_id];
        ids.reserve(candidate.module_definitions.size() + candidate.leaf_definitions.size());
        for (auto const& module : candidate.module_definitions) {
            ids.push_back(module.module_id);
            candidate_packages_by_id[module.module_id].push_back(package_id);
        }
        for (auto const& node_type : candidate.leaf_definitions) {
            ids.push_back(node_type.node_type_id);
            candidate_packages_by_id[node_type.node_type_id].push_back(package_id);
        }
    }

    snapshots.reserve(declarations_by_package_id.size());
    for (auto const& [package_id, declaration] : declarations_by_package_id) {
        IvPackageDefinitionSnapshot snapshot{
            .declaration = declaration,
        };
        if (auto modules = module_definition_ids_by_package_id.find(package_id);
            modules != module_definition_ids_by_package_id.end()) {
            snapshot.published_module_definition_ids = modules->second;
        }
        for (auto const& [node_type_id, state] : loaded_leaf_definitions_by_id) {
            if (state->snapshot.package_id == package_id) {
                snapshot.published_leaf_definition_ids.push_back(node_type_id);
            }
        }
        std::ranges::sort(snapshot.published_leaf_definition_ids);

        if (auto const validation =
                candidate_validation_messages_by_package_id.find(package_id);
            validation != candidate_validation_messages_by_package_id.end()) {
            snapshot.publication_message = validation->second;
        } else if (auto candidates = candidate_ids_by_package.find(package_id);
                   candidates != candidate_ids_by_package.end()) {
            for (auto const& id : candidates->second) {
                auto const providers = candidate_packages_by_id.find(id);
                if (providers != candidate_packages_by_id.end()
                    && providers->second.size() > 1) {
                    snapshot.publication_message =
                        "IV package definition ID '" + id
                        + "' is provided by multiple IV packages";
                    break;
                }
            }
            if (snapshot.publication_message.empty()) {
                auto const has_unpublished_candidate = std::ranges::any_of(
                    candidates->second,
                    [&](std::string const& id) {
                        auto const owner = package_id_by_definition_id.find(id);
                        return owner == package_id_by_definition_id.end()
                            || owner->second != package_id;
                    });
                if (has_unpublished_candidate) {
                    snapshot.publication_message =
                        "Definitions are waiting for the registry to publish them";
                }
            }
        }
        snapshots.push_back(std::move(snapshot));
    }
    std::ranges::sort(
        snapshots,
        {},
        [](IvPackageDefinitionSnapshot const& snapshot) {
            return snapshot.declaration.package_id;
        });
    return snapshots;
}


std::shared_ptr<NodeDefinitionsSnapshot const> NodeDefinitions::snapshot() const
{
    std::scoped_lock lock(mutex);
    return definitions_snapshot_;
}

std::vector<ModuleNodeDefinition> NodeDefinitions::loaded_module_definitions() const
{
    std::vector<ModuleNodeDefinition> definitions;
    std::scoped_lock lock(mutex);
    definitions.reserve(loaded_module_definitions_by_id.size());
    for (auto const& [_, definition] : loaded_module_definitions_by_id) {
        definitions.push_back(definition->snapshot);
    }
    std::ranges::sort(definitions, {}, &ModuleNodeDefinition::definition_id);
    return definitions;
}

std::vector<LeafNodeDefinition> NodeDefinitions::loaded_leaf_definitions() const
{
    std::vector<LeafNodeDefinition> definitions;
    std::scoped_lock lock(mutex);
    definitions.reserve(loaded_leaf_definitions_by_id.size());
    for (auto const& [_, definition] : loaded_leaf_definitions_by_id) {
        definitions.push_back(definition->snapshot);
    }
    std::ranges::sort(definitions, {}, &LeafNodeDefinition::definition_id);
    return definitions;
}


} // namespace iv
