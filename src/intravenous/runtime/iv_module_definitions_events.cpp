#include <intravenous/runtime/iv_module_definitions_events.h>

#include <utility>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    IvPackageDeclarationsChangedEvent,
    iv_runtime_iv_package_declarations_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvPackageDefinitionsChangedEvent,
    iv_runtime_iv_package_definitions_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvPackageCatalogChangedEvent,
    iv_runtime_iv_package_catalog_changed_event);
} // namespace iv
