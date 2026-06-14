#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/iv_module_definitions.h>

#include <string>

namespace iv {
using IvModuleDefinitionDeclarationsChangedEvent =
    void (*)(IvModuleDefinitionDeclarationsChanged const &);
using IvModuleDefinitionsChangedEvent =
    void (*)(IvModuleDefinitionsChanged const &);
using IvModuleDefinitionsNotificationEvent =
    void (*)(IvModuleDefinitionsNotification const &);

struct IvModuleDefinitionBuilderRequest {
    std::string definition_id {};
};

class IvModuleDefinitionBuilderBuilder {
    GraphBuilder const *builder_ = nullptr;

public:
    void succeed(GraphBuilder const *builder);
    [[nodiscard]] GraphBuilder const *build() const;
};

using IvModuleDefinitionBuilderRequestedEvent =
    void (*)(IvModuleDefinitionBuilderRequest const &, IvModuleDefinitionBuilderBuilder &);

IV_DECLARE_LINKER_EVENT(
    IvModuleDefinitionDeclarationsChangedEvent,
    iv_runtime_iv_module_definitions_declarations_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleDefinitionsChangedEvent,
    iv_runtime_iv_module_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleDefinitionsNotificationEvent,
    iv_runtime_iv_module_definitions_notification_event);
IV_DECLARE_SINGLETON_EVENT(
    IvModuleDefinitionBuilderRequestedEvent,
    iv_runtime_iv_module_definition_builder_requested_event);
} // namespace iv
