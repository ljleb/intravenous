#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/package_pipeline_types.h>

namespace iv {
using PackageWatcherRefreshRequestedEvent = void (*)(PackageWatcherRefreshRequest&);
using PackageJitBatchRequestedEvent = void (*)(PackageJitBatchRequest&);
using PackageRefreshEvent = void (*)(PackageRefreshTransaction const&);
using PackageDefinitionsPublicationRequestedEvent =
    void (*)(PackageDefinitionsPublicationRequest&);

struct IvPackageCatalogChanged {};
using IvPackageCatalogChangedEvent = void (*)(IvPackageCatalogChanged const&);

IV_DECLARE_LINKER_EVENT(
    PackageWatcherRefreshRequestedEvent,
    iv_runtime_package_watcher_refresh_requested_event);
IV_DECLARE_LINKER_EVENT(
    PackageJitBatchRequestedEvent,
    iv_runtime_package_jit_batch_requested_event);
IV_DECLARE_LINKER_EVENT(
    PackageRefreshEvent,
    iv_runtime_package_refresh_event);
IV_DECLARE_LINKER_EVENT(
    PackageDefinitionsPublicationRequestedEvent,
    iv_runtime_package_definitions_publication_requested_event);
IV_DECLARE_LINKER_EVENT(
    IvPackageCatalogChangedEvent,
    iv_runtime_iv_package_catalog_changed_event);
} // namespace iv
