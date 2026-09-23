#include <intravenous/runtime/node_definitions.h>

#include <intravenous/runtime/node_definitions_events.h>

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace iv {
namespace {
std::filesystem::path normalize_path(std::filesystem::path const& path)
{
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error) {
        error.clear();
        canonical = std::filesystem::absolute(path, error);
    }
    return (error ? path : canonical).lexically_normal();
}

std::unique_ptr<NodeDefinitions::ModuleDefinitionState> make_module_definition_state(
    PackageModuleDefinition const& loaded,
    PackageRevision const& package_revision,
    std::uint64_t version)
{
    auto state = std::make_unique<NodeDefinitions::ModuleDefinitionState>();
    state->module_refs = loaded.module_refs;
    state->package_revision = package_revision.revision;
    state->version = version;
    state->snapshot = ModuleNodeDefinition{
        .definition_id = loaded.definition_id,
        .package_id = package_revision.package_id,
        .package_root = normalize_path(package_revision.package_root),
        .module_id = loaded.module_id,
        .provider = loaded.provider,
        .introspection = loaded.introspection,
        .dependencies = loaded.dependencies,
        .module_refs = state->module_refs,
        .configured_graph = loaded.configured_graph,
    };
    return state;
}

std::unique_ptr<NodeDefinitions::LeafDefinitionState> make_leaf_definition_state(
    PackageLeafDefinition const& loaded,
    PackageRevision const& package_revision,
    std::uint64_t version)
{
    auto state = std::make_unique<NodeDefinitions::LeafDefinitionState>();
    state->module_refs = loaded.module_refs;
    state->package_revision = package_revision.revision;
    state->version = version;
    state->snapshot = LeafNodeDefinition{
        .definition_id = loaded.definition_id,
        .package_id = package_revision.package_id,
        .package_root = normalize_path(package_revision.package_root),
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

void NodeDefinitions::fill_publication_projection_locked(
    PackageDefinitionsPublicationRequest& request) const
{
    request.published_module_ids_by_package_id.clear();
    request.published_leaf_ids_by_package_id.clear();
    for (auto const& [definition_id, state] : loaded_module_definitions_by_id_) {
        request.published_module_ids_by_package_id[state->snapshot.package_id].push_back(
            definition_id);
    }
    for (auto const& [definition_id, state] : loaded_leaf_definitions_by_id_) {
        request.published_leaf_ids_by_package_id[state->snapshot.package_id].push_back(
            definition_id);
    }
    for (auto& [_, ids] : request.published_module_ids_by_package_id) {
        std::ranges::sort(ids);
    }
    for (auto& [_, ids] : request.published_leaf_ids_by_package_id) {
        std::ranges::sort(ids);
    }
}

void NodeDefinitions::handle_package_definitions_publication(
    PackageDefinitionsPublicationRequest& request)
{
    if (!request.snapshot) {
        throw std::invalid_argument("PackageDefinitions publication requires a snapshot");
    }

    struct ModuleProvider {
        std::shared_ptr<PackageRevision const> revision{};
        PackageModuleDefinition const* definition = nullptr;
    };
    struct LeafProvider {
        std::shared_ptr<PackageRevision const> revision{};
        PackageLeafDefinition const* definition = nullptr;
    };

    std::unordered_map<std::string, std::vector<ModuleProvider>> module_providers;
    std::unordered_map<std::string, std::vector<LeafProvider>> leaf_providers;
    std::unordered_map<std::string, std::size_t> provider_count_by_id;
    request.publication_messages_by_package_id.clear();

    bool valid = true;
    for (auto const& [package_id, revision] : request.snapshot->by_package_id) {
        if (!revision) continue;
        std::unordered_set<std::string> package_ids;
        std::string validation_message;

        for (auto const& definition : revision->module_definitions) {
            if (definition.definition_id.empty() || definition.module_id.empty()) {
                validation_message = "IV package published an IV module with an empty ID";
                break;
            }
            if (!package_ids.insert(definition.definition_id).second) {
                validation_message = "IV package published duplicate IV definition ID '"
                    + definition.definition_id + "'";
                break;
            }
        }
        if (validation_message.empty()) {
            for (auto const& definition : revision->leaf_definitions) {
                if (definition.definition_id.empty()) {
                    validation_message = "IV package published a leaf definition with an empty ID";
                    break;
                }
                if (!package_ids.insert(definition.definition_id).second) {
                    validation_message = "IV package published duplicate IV definition ID '"
                        + definition.definition_id + "'";
                    break;
                }
            }
        }
        if (!validation_message.empty()) {
            request.publication_messages_by_package_id[package_id] =
                std::move(validation_message);
            valid = false;
            continue;
        }

        for (auto const& definition : revision->module_definitions) {
            module_providers[definition.definition_id].push_back({revision, &definition});
            ++provider_count_by_id[definition.definition_id];
        }
        for (auto const& definition : revision->leaf_definitions) {
            leaf_providers[definition.definition_id].push_back({revision, &definition});
            ++provider_count_by_id[definition.definition_id];
        }
    }

    for (auto const& [definition_id, provider_count] : provider_count_by_id) {
        if (provider_count <= 1) continue;
        valid = false;
        auto const message = "IV package definition ID '" + definition_id
            + "' is provided by multiple IV packages";
        if (auto modules = module_providers.find(definition_id);
            modules != module_providers.end()) {
            for (auto const& provider : modules->second) {
                request.publication_messages_by_package_id[provider.revision->package_id] = message;
            }
        }
        if (auto leaves = leaf_providers.find(definition_id);
            leaves != leaf_providers.end()) {
            for (auto const& provider : leaves->second) {
                request.publication_messages_by_package_id[provider.revision->package_id] = message;
            }
        }
    }

    ModuleNodeDefinitionsChanged module_diff;
    LeafNodeDefinitionsChanged leaf_diff;
    bool registry_changed = false;
    {
        std::scoped_lock lock(mutex_);
        if (!valid) {
            fill_publication_projection_locked(request);
            return;
        }

        std::unordered_map<std::string, std::unique_ptr<ModuleDefinitionState>> next_modules;
        next_modules.reserve(module_providers.size());
        for (auto const& [definition_id, providers] : module_providers) {
            auto const& provider = providers.front();
            auto existing = loaded_module_definitions_by_id_.find(definition_id);
            auto const requires_publication = existing == loaded_module_definitions_by_id_.end()
                || existing->second->snapshot.package_id != provider.revision->package_id
                || existing->second->package_revision != provider.revision->revision;
            auto const version = requires_publication
                ? next_definition_version_++
                : existing->second->version;
            auto state = make_module_definition_state(
                *provider.definition, *provider.revision, version);
            if (existing == loaded_module_definitions_by_id_.end()) {
                module_diff.created.push_back(state->snapshot);
            } else if (requires_publication) {
                module_diff.updated.push_back(state->snapshot);
            }
            next_modules.emplace(definition_id, std::move(state));
        }
        for (auto const& [definition_id, _] : loaded_module_definitions_by_id_) {
            if (!next_modules.contains(definition_id)) {
                module_diff.deleted_definition_ids.push_back(definition_id);
            }
        }

        std::unordered_map<std::string, std::unique_ptr<LeafDefinitionState>> next_leaves;
        next_leaves.reserve(leaf_providers.size());
        for (auto const& [definition_id, providers] : leaf_providers) {
            auto const& provider = providers.front();
            auto existing = loaded_leaf_definitions_by_id_.find(definition_id);
            auto const requires_publication = existing == loaded_leaf_definitions_by_id_.end()
                || existing->second->snapshot.package_id != provider.revision->package_id
                || existing->second->package_revision != provider.revision->revision;
            auto const version = requires_publication
                ? next_definition_version_++
                : existing->second->version;
            auto state = make_leaf_definition_state(
                *provider.definition, *provider.revision, version);
            if (existing == loaded_leaf_definitions_by_id_.end()) {
                leaf_diff.created.push_back(state->snapshot);
            } else if (requires_publication) {
                leaf_diff.updated.push_back(state->snapshot);
            }
            next_leaves.emplace(definition_id, std::move(state));
        }
        for (auto const& [definition_id, _] : loaded_leaf_definitions_by_id_) {
            if (!next_leaves.contains(definition_id)) {
                leaf_diff.deleted_definition_ids.push_back(definition_id);
            }
        }

        registry_changed = !module_diff.created.empty() || !module_diff.updated.empty()
            || !module_diff.deleted_definition_ids.empty() || !leaf_diff.created.empty()
            || !leaf_diff.updated.empty() || !leaf_diff.deleted_definition_ids.empty();

        loaded_module_definitions_by_id_ = std::move(next_modules);
        loaded_leaf_definitions_by_id_ = std::move(next_leaves);

        if (registry_changed) {
            auto next_snapshot = std::make_shared<NodeDefinitionsSnapshot>();
            next_snapshot->generation = ++snapshot_generation_;
            next_snapshot->package_revisions.reserve(request.snapshot->by_package_id.size());
            for (auto const& [_, revision] : request.snapshot->by_package_id) {
                next_snapshot->package_revisions.push_back(revision);
            }
            std::ranges::sort(
                next_snapshot->package_revisions,
                {},
                [](auto const& revision) { return revision->package_id; });
            next_snapshot->by_id.reserve(
                loaded_module_definitions_by_id_.size()
                + loaded_leaf_definitions_by_id_.size());
            for (auto const& [definition_id, state] : loaded_module_definitions_by_id_) {
                next_snapshot->by_id.emplace(definition_id, NodeDefinitionEntry{
                    .definition_id = definition_id,
                    .kind = NodeDefinitionKind::module,
                    .version = state->version,
                    .definition = state->snapshot,
                });
            }
            for (auto const& [definition_id, state] : loaded_leaf_definitions_by_id_) {
                next_snapshot->by_id.emplace(definition_id, NodeDefinitionEntry{
                    .definition_id = definition_id,
                    .kind = NodeDefinitionKind::leaf,
                    .version = state->version,
                    .definition = state->snapshot,
                });
            }
            definitions_snapshot_ = std::move(next_snapshot);
        }
        fill_publication_projection_locked(request);
    }

    if (!registry_changed) return;

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_node_definitions_snapshot_changed_event,
        NodeDefinitionsSnapshotChanged{.snapshot = snapshot()});
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_package_definitions_changed_event,
        IvPackageDefinitionsChanged{
            .module_definitions = std::move(module_diff),
            .leaf_definitions = std::move(leaf_diff),
        });
}

void NodeDefinitions::seed_loaded_definition(PackageModuleDefinition loaded_definition)
{
    auto package_id = loaded_definition.package_id;
    if (package_id.empty()) {
        package_id = loaded_definition.package_root.empty()
            ? loaded_definition.definition_id
            : normalize_path(loaded_definition.package_root).generic_string();
        loaded_definition.package_id = package_id;
    }
    auto revision = std::make_shared<PackageRevision const>(PackageRevision{
        .package_id = package_id,
        .package_root = loaded_definition.package_root,
        .revision = 1,
        .module_definitions = {std::move(loaded_definition)},
    });
    auto package_snapshot = std::make_shared<PackageDefinitionsSnapshot>();
    package_snapshot->generation = 1;
    package_snapshot->by_package_id.emplace(package_id, std::move(revision));
    PackageDefinitionsPublicationRequest request{.snapshot = std::move(package_snapshot)};
    handle_package_definitions_publication(request);
}

std::shared_ptr<NodeDefinitionsSnapshot const> NodeDefinitions::snapshot() const
{
    std::scoped_lock lock(mutex_);
    return definitions_snapshot_;
}

std::vector<ModuleNodeDefinition> NodeDefinitions::loaded_module_definitions() const
{
    std::vector<ModuleNodeDefinition> definitions;
    std::scoped_lock lock(mutex_);
    definitions.reserve(loaded_module_definitions_by_id_.size());
    for (auto const& [_, definition] : loaded_module_definitions_by_id_) {
        definitions.push_back(definition->snapshot);
    }
    std::ranges::sort(definitions, {}, &ModuleNodeDefinition::definition_id);
    return definitions;
}

std::vector<LeafNodeDefinition> NodeDefinitions::loaded_leaf_definitions() const
{
    std::vector<LeafNodeDefinition> definitions;
    std::scoped_lock lock(mutex_);
    definitions.reserve(loaded_leaf_definitions_by_id_.size());
    for (auto const& [_, definition] : loaded_leaf_definitions_by_id_) {
        definitions.push_back(definition->snapshot);
    }
    std::ranges::sort(definitions, {}, &LeafNodeDefinition::definition_id);
    return definitions;
}
} // namespace iv
