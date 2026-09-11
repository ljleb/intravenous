#include <intravenous/runtime/iv_module_definitions_events.h>

#include <utility>

namespace iv {
void IvModuleDefinitionLookupBuilder::succeed(
    std::optional<IvModuleDefinition> definition)
{
    has_response_ = true;
    definition_ = std::move(definition);
}

bool IvModuleDefinitionLookupBuilder::has_response() const
{
    return has_response_;
}

std::optional<IvModuleDefinition> IvModuleDefinitionLookupBuilder::definition() const
{
    return definition_;
}

IV_DEFINE_LINKER_EVENT(
    IvPackageDeclarationsChangedEvent,
    iv_runtime_iv_package_declarations_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleDefinitionLookupEvent,
    iv_runtime_iv_module_definition_lookup_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleDefinitionsChangedEvent,
    iv_runtime_iv_module_definitions_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvNodeTypeDefinitionsChangedEvent,
    iv_runtime_iv_node_type_definitions_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleDefinitionsNotificationEvent,
    iv_runtime_iv_module_definitions_notification_event);
} // namespace iv
