#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/package_reload_types.h>

namespace iv {
using PackageReloadResultsEvent =
    void (*)(PackageReloadResults const &);
using PackageBuildStatusesChangedEvent =
    void (*)(std::vector<PackageBuildStatus> const &);

IV_DECLARE_LINKER_EVENT(
    PackageReloadResultsEvent,
    iv_runtime_package_reload_results_event);
IV_DECLARE_LINKER_EVENT(
    PackageBuildStatusesChangedEvent,
    iv_runtime_package_build_statuses_changed_event);
} // namespace iv
