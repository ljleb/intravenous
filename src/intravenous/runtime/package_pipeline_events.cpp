#include <intravenous/runtime/package_pipeline_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    PackageWatcherRefreshRequestedEvent,
    iv_runtime_package_watcher_refresh_requested_event);
IV_DEFINE_LINKER_EVENT(
    PackageJitBatchRequestedEvent,
    iv_runtime_package_jit_batch_requested_event);
IV_DEFINE_LINKER_EVENT(
    PackageRefreshEvent,
    iv_runtime_package_refresh_event);
IV_DEFINE_LINKER_EVENT(
    PackageDefinitionsPublicationRequestedEvent,
    iv_runtime_package_definitions_publication_requested_event);
IV_DEFINE_LINKER_EVENT(
    IvPackageCatalogChangedEvent,
    iv_runtime_iv_package_catalog_changed_event);
} // namespace iv
