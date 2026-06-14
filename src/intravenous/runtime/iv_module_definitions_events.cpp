#include <intravenous/runtime/iv_module_definitions_events.h>

namespace iv {
void IvModuleDefinitionBuilderBuilder::succeed(GraphBuilder const *builder)
{
    builder_ = builder;
}

GraphBuilder const *IvModuleDefinitionBuilderBuilder::build() const
{
    return builder_;
}

IV_DEFINE_LINKER_EVENT(
    IvModuleDefinitionDeclarationsChangedEvent,
    iv_runtime_iv_module_definitions_declarations_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleDefinitionsChangedEvent,
    iv_runtime_iv_module_definitions_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleDefinitionsNotificationEvent,
    iv_runtime_iv_module_definitions_notification_event);
IV_DEFINE_SINGLETON_EVENT(
    IvModuleDefinitionBuilderRequestedEvent,
    iv_runtime_iv_module_definition_builder_requested_event,
    +[](IvModuleDefinitionBuilderRequest const &, IvModuleDefinitionBuilderBuilder &) {});
} // namespace iv
