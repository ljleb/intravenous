#include "runtime/iv_modules_project_service_bridge.h"

#include "runtime/iv_modules_events.h"
#include "runtime/project_service.h"

namespace iv {
namespace {
RuntimeProjectService *bound_service = nullptr;

RuntimeProjectService *bound_service_or_null()
{
    return bound_service;
}

void handle_definitions_changed(RuntimeIvModuleDefinitionsChanged const &diff)
{
    auto *service = bound_service_or_null();
    if (service == nullptr) {
        return;
    }
    service->handle_iv_modules_definitions_changed(diff);
}

IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeIvModulesDefinitionsChangedEvent,
    iv_runtime_iv_modules_definitions_changed_event,
    handle_definitions_changed);
} // namespace

void bind_iv_modules_project_service_bridge(RuntimeProjectService &service)
{
    bound_service = &service;
}

void unbind_iv_modules_project_service_bridge(
    RuntimeProjectService const &service)
{
    if (bound_service == &service) {
        bound_service = nullptr;
    }
}
} // namespace iv
