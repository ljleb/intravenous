#include <intravenous/runtime/package_reload_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    PackageReloadResultsEvent,
    iv_runtime_package_reload_results_event);
IV_DEFINE_LINKER_EVENT(
    PackageBuildStatusesChangedEvent,
    iv_runtime_package_build_statuses_changed_event);
} // namespace iv
