#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/iv_module_definitions.h>

#include <optional>
#include <string>

namespace iv {
class IvModuleDefinitionLookupBuilder {
    bool has_response_ = false;
    std::optional<IvModuleDefinition> definition_ {};

public:
    void succeed(std::optional<IvModuleDefinition> definition);
    [[nodiscard]] bool has_response() const;
    [[nodiscard]] std::optional<IvModuleDefinition> definition() const;
};

using IvPackageDeclarationsChangedEvent =
    void (*)(IvPackageDeclarationsChanged const &);
using IvModuleDefinitionLookupEvent =
    void (*)(std::string const&, IvModuleDefinitionLookupBuilder&);
using IvModuleDefinitionsChangedEvent =
    void (*)(IvModuleDefinitionsChanged const &);
using IvNodeTypeDefinitionsChangedEvent =
    void (*)(IvNodeTypeDefinitionsChanged const &);
using IvModuleDefinitionsNotificationEvent =
    void (*)(IvModuleDefinitionsNotification const &);

IV_DECLARE_LINKER_EVENT(
    IvPackageDeclarationsChangedEvent,
    iv_runtime_iv_package_declarations_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleDefinitionLookupEvent,
    iv_runtime_iv_module_definition_lookup_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleDefinitionsChangedEvent,
    iv_runtime_iv_module_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvNodeTypeDefinitionsChangedEvent,
    iv_runtime_iv_node_type_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleDefinitionsNotificationEvent,
    iv_runtime_iv_module_definitions_notification_event);
} // namespace iv
