#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/iv_package_reload.h>

namespace iv {
using IvPackageReloadResultsEvent =
    void (*)(IvPackageReloadResults const &);
using IvPackageBuildStatusesChangedEvent =
    void (*)(std::vector<IvPackageBuildStatus> const &);

IV_DECLARE_LINKER_EVENT(
    IvPackageReloadResultsEvent,
    iv_runtime_iv_package_reload_results_event);
IV_DECLARE_LINKER_EVENT(
    IvPackageBuildStatusesChangedEvent,
    iv_runtime_iv_package_build_statuses_changed_event);
} // namespace iv
