#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/package_pipeline_types.h>
#include <intravenous/runtime/runtime_project_api_types.h>

#include <optional>

namespace iv {
using PackageWatcherRefreshRequestedEvent = void (*)(PackageWatcherRefreshRequest&);
using PackageJitBatchRequestedEvent = void (*)(PackageJitBatchRequest&);
using PackageRefreshEvent = void (*)(PackageRefreshTransaction&);
using PackageDefinitionsPublicationRequestedEvent =
    void (*)(PackageDefinitionsPublicationRequest&);

// The one report of a package refresh, sent by PackageWatcher after the
// refresh's whole propagation has returned: the build outcome, whether the
// package catalog changed, and the read-model changes of the project rebuild
// the refresh caused.
struct PackageRefreshPublication {
    std::optional<ProjectStatusNotification> build_status{};
    bool package_catalog_changed = false;
    ProjectGraphPublication project{};
};
using PackageRefreshPublishedEvent = void (*)(PackageRefreshPublication const&);

struct PackageWatcherWorkAvailable {};
using PackageWatcherWorkAvailableEvent = void (*)(PackageWatcherWorkAvailable const&);

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
    PackageRefreshPublishedEvent,
    iv_runtime_package_refresh_published_event);
IV_DECLARE_LINKER_EVENT(
    PackageWatcherWorkAvailableEvent,
    iv_runtime_package_watcher_work_available_event);
} // namespace iv
