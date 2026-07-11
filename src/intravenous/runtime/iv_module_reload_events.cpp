#include <intravenous/runtime/iv_module_reload_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    IvModuleReloadResultsEvent,
    iv_runtime_iv_module_reload_results_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleReloadStatusEvent,
    iv_runtime_iv_module_reload_status_event);
} // namespace iv
