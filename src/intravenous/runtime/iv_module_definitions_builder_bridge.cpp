#include <intravenous/runtime/iv_module_definitions_builder_bridge.h>

#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_definitions_events.h>

namespace iv {
namespace {
IvModuleDefinitions *bound_definitions = nullptr;

void handle_definition_builder_requested(
    IvModuleDefinitionBuilderRequest const &request,
    IvModuleDefinitionBuilderBuilder &builder)
{
    if (bound_definitions == nullptr) {
        return;
    }
    if (auto const *result =
            bound_definitions->builder_for_definition(request.definition_id);
        result != nullptr) {
        builder.succeed(result);
    }
}

IV_SUBSCRIBE_SINGLETON_EVENT(
    IvModuleDefinitionBuilderRequestedEvent,
    iv_runtime_iv_module_definition_builder_requested_event,
    handle_definition_builder_requested);
} // namespace

void bind_iv_module_definitions_builder_bridge(
    IvModuleDefinitions &definitions)
{
    bound_definitions = &definitions;
}

void unbind_iv_module_definitions_builder_bridge(
    IvModuleDefinitions const &definitions)
{
    if (bound_definitions == &definitions) {
        bound_definitions = nullptr;
    }
}
} // namespace iv
