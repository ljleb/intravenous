#include <intravenous/runtime/iv_package_reload_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    IvPackageReloadResultsEvent,
    iv_runtime_iv_package_reload_results_event);
IV_DEFINE_LINKER_EVENT(
    IvPackageBuildStatusesChangedEvent,
    iv_runtime_iv_package_build_statuses_changed_event);
} // namespace iv
