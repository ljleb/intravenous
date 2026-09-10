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
        .module_root = normalize_path(loaded.module_root),
        .module_id = loaded.module_id,
        .introspection = loaded.introspection,
        .dependencies = loaded.dependencies,
        .module_refs = state->module_refs,
        .root = loaded.root,
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
    bool removed = false;
    {
        std::scoped_lock lock(mutex);
        removed = declarations_by_source_id.erase(source_id) > 0;
        auto module_ids = module_ids_by_source_id.find(source_id);
        if (module_ids != module_ids_by_source_id.end()) {
            for (auto const& module_id : module_ids->second) {
                loaded_definitions_by_module_id.erase(module_id);
                source_id_by_module_id.erase(module_id);
                definition_diff.deleted_definition_ids.push_back(module_id);
            }
            module_ids_by_source_id.erase(module_ids);
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

void IvModuleDefinitions::handle_reload_results(IvModuleReloadResults const& results)
{
    std::unordered_map<std::string, std::vector<IvModuleReloadedDefinition const*>> candidates;
    std::vector<std::string> candidate_source_ids;
    auto remember_candidate_source = [&](std::string const& source_id) {
        if (candidates.contains(source_id)) return;
        candidates.try_emplace(source_id);
        candidate_source_ids.push_back(source_id);
    };
    for (auto const& source : results.sources) {
        remember_candidate_source(source.definition_id);
    }
    for (auto const& loaded : results.loaded) {
        remember_candidate_source(loaded.source_id);
        candidates[loaded.source_id].push_back(&loaded);
    }

    IvModuleDefinitionsChanged definition_diff;
    std::vector<IvModuleDefinitionsMessage> failures;
    {
        std::scoped_lock lock(mutex);
        for (auto const& source_id : candidate_source_ids) {
            auto const& loaded = candidates.at(source_id);
            auto declaration = declarations_by_source_id.find(source_id);
            if (declaration == declarations_by_source_id.end()) continue;

            std::unordered_set<std::string> candidate_ids;
            std::string error;
            for (auto const* definition : loaded) {
                if (definition->module_id.empty()) {
                    error = "IV source published an IV module with an empty ID";
                    break;
                }
                if (!candidate_ids.insert(definition->module_id).second) {
                    error = "IV source published duplicate IV module ID '"
                        + definition->module_id + "'";
                    break;
                }
                if (auto owner = source_id_by_module_id.find(definition->module_id);
                    owner != source_id_by_module_id.end() && owner->second != source_id) {
                    error = "IV module ID '" + definition->module_id
                        + "' is also provided by IV source '" + owner->second + "'";
                    break;
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

            auto previous = std::move(module_ids_by_source_id[source_id]);
            module_ids_by_source_id[source_id].clear();
            for (auto const& module_id : previous) {
                if (candidate_ids.contains(module_id)) continue;
                loaded_definitions_by_module_id.erase(module_id);
                source_id_by_module_id.erase(module_id);
                definition_diff.deleted_definition_ids.push_back(module_id);
            }
            for (auto const* loaded_definition : loaded) {
                auto state = make_definition_state(*loaded_definition);
                auto snapshot = state->snapshot;
                auto existing = loaded_definitions_by_module_id.find(snapshot.module_id);
                if (existing == loaded_definitions_by_module_id.end()) {
                    loaded_definitions_by_module_id.emplace(
                        snapshot.module_id, std::move(state));
                    definition_diff.created.push_back(snapshot);
                } else {
                    existing->second = std::move(state);
                    definition_diff.updated.push_back(snapshot);
                }
                source_id_by_module_id[snapshot.module_id] = source_id;
                module_ids_by_source_id[source_id].push_back(snapshot.module_id);
            }
        }
    }
    for (auto& failure : failures) emit_notification(std::move(failure));
    if (!definition_diff.created.empty() || !definition_diff.updated.empty()
        || !definition_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_definitions_changed_event, definition_diff);
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
} // namespace iv
