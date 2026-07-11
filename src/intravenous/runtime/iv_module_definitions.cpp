#include <intravenous/runtime/iv_module_definitions.h>

#include <intravenous/graph/builder.h>
#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_reload.h>

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

std::unique_ptr<IvModuleDefinitions::DefinitionState> make_definition_state(
    IvModuleReloadedDefinition const &loaded)
{
    auto state = std::make_unique<IvModuleDefinitions::DefinitionState>();
    state->module_refs = loaded.module_refs;
    state->canonical_builder = loaded.canonical_builder;
    state->snapshot = IvModuleDefinition{
        .definition_id = loaded.definition_id,
        .module_root = normalize_path(loaded.module_root),
        .module_id = loaded.module_id,
        .introspection = loaded.introspection,
        .dependencies = loaded.dependencies,
        .module_refs = state->module_refs,
        .canonical_builder = &state->canonical_builder,
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
    std::string module_id,
    std::filesystem::path module_root)
{
    IvModuleDefinitionDeclaration declaration{
        .definition_id = std::move(module_id),
        .module_root = normalize_path(module_root),
    };

    bool inserted = false;
    {
        std::scoped_lock lock(mutex);
        auto [it, was_inserted] =
            declarations_by_id.emplace(declaration.definition_id, declaration);
        if (!was_inserted) {
            if (it->second.module_root == declaration.module_root) {
                return declaration.definition_id;
            }
            it->second = declaration;
        }
        inserted = was_inserted;
    }

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_definitions_declarations_changed_event,
        IvModuleDefinitionDeclarationsChanged{
            .created = inserted ? std::vector<IvModuleDefinitionDeclaration>{declaration}
                                : std::vector<IvModuleDefinitionDeclaration>{},
            .updated = inserted ? std::vector<IvModuleDefinitionDeclaration>{}
                                : std::vector<IvModuleDefinitionDeclaration>{declaration},
        });
    return declaration.definition_id;
}

void IvModuleDefinitions::remove_definition(std::string const &definition_id)
{
    IvModuleDefinitionsChanged public_diff{};
    bool removed_declaration = false;
    {
        std::scoped_lock lock(mutex);
        removed_declaration = declarations_by_id.erase(definition_id) > 0;
        if (loaded_definitions_by_id.erase(definition_id) > 0) {
            public_diff.deleted_definition_ids.push_back(definition_id);
        }
    }

    if (removed_declaration) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_definitions_declarations_changed_event,
            IvModuleDefinitionDeclarationsChanged{
                .deleted_definition_ids = {definition_id},
            });
    }
    if (!public_diff.created.empty() ||
        !public_diff.updated.empty() ||
        !public_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_definitions_changed_event,
            public_diff);
    }
}

void IvModuleDefinitions::handle_required_definitions_changed(
    IvModuleRequiredDefinitionsChanged const &diff)
{
    IvModuleDefinitionDeclarationsChanged declaration_diff{};
    IvModuleDefinitionsChanged public_diff{};

    {
        std::scoped_lock lock(mutex);
        for (auto const &required : diff.created) {
            auto [it, inserted] = declarations_by_id.emplace(required.definition_id, IvModuleDefinitionDeclaration{
                .definition_id = required.definition_id,
                .module_root = normalize_path(required.module_root),
            });
            if (inserted) {
                declaration_diff.created.push_back(it->second);
            }
        }
        for (auto const &required : diff.updated) {
            auto normalized_root = normalize_path(required.module_root);
            auto existing = declarations_by_id.find(required.definition_id);
            if (existing == declarations_by_id.end()) {
                IvModuleDefinitionDeclaration declaration{
                    .definition_id = required.definition_id,
                    .module_root = normalized_root,
                };
                declarations_by_id.emplace(required.definition_id, declaration);
                declaration_diff.created.push_back(std::move(declaration));
            } else if (existing->second.module_root != normalized_root) {
                existing->second.module_root = normalized_root;
                declaration_diff.updated.push_back(existing->second);
            } else if (auto loaded = loaded_definitions_by_id.find(required.definition_id);
                       loaded != loaded_definitions_by_id.end()) {
                public_diff.updated.push_back(loaded->second->snapshot);
            }
        }
        for (auto const &definition_id : diff.deleted_definition_ids) {
            if (declarations_by_id.erase(definition_id) > 0) {
                declaration_diff.deleted_definition_ids.push_back(definition_id);
            }
            if (loaded_definitions_by_id.erase(definition_id) > 0) {
                public_diff.deleted_definition_ids.push_back(definition_id);
            }
        }
    }

    if (!declaration_diff.created.empty() ||
        !declaration_diff.updated.empty() ||
        !declaration_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_definitions_declarations_changed_event,
            declaration_diff);
    }
    if (!public_diff.created.empty() ||
        !public_diff.updated.empty() ||
        !public_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_definitions_changed_event,
            public_diff);
    }
}

void IvModuleDefinitions::handle_reload_results(IvModuleReloadResults const &results)
{
    IvModuleDefinitionsChanged public_diff{};

    {
        std::scoped_lock lock(mutex);
        for (auto const &loaded : results.loaded) {
            auto declaration = declarations_by_id.find(loaded.definition_id);
            if (declaration == declarations_by_id.end()) {
                continue;
            }

            auto state = make_definition_state(loaded);
            auto snapshot = state->snapshot;
            auto existing = loaded_definitions_by_id.find(loaded.definition_id);
            if (existing == loaded_definitions_by_id.end()) {
                loaded_definitions_by_id.emplace(loaded.definition_id, std::move(state));
                public_diff.created.push_back(snapshot);
            } else {
                existing->second = std::move(state);
                public_diff.updated.push_back(snapshot);
            }
        }
    }

    if (!public_diff.created.empty() ||
        !public_diff.updated.empty() ||
        !public_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_definitions_changed_event,
            public_diff);
    }
}

void IvModuleDefinitions::seed_loaded_definition(
    IvModuleReloadedDefinition loaded_definition)
{
    {
        std::scoped_lock lock(mutex);
        declarations_by_id[loaded_definition.definition_id] = IvModuleDefinitionDeclaration{
            .definition_id = loaded_definition.definition_id,
            .module_root = normalize_path(loaded_definition.module_root),
        };
    }
    IvModuleReloadResults results;
    results.loaded.push_back(std::move(loaded_definition));
    handle_reload_results(results);
}

std::vector<IvModuleDefinition> IvModuleDefinitions::loaded_definitions() const
{
    std::vector<IvModuleDefinition> definitions;
    std::scoped_lock lock(mutex);
    definitions.reserve(loaded_definitions_by_id.size());
    for (auto const &entry : loaded_definitions_by_id) {
        definitions.push_back(entry.second->snapshot);
    }
    return definitions;
}

GraphBuilder const *IvModuleDefinitions::builder_for_definition(
    std::string const &definition_id) const
{
    std::scoped_lock lock(mutex);
    auto const it = loaded_definitions_by_id.find(definition_id);
    if (it == loaded_definitions_by_id.end()) {
        return nullptr;
    }
    return &it->second->canonical_builder;
}
} // namespace iv
