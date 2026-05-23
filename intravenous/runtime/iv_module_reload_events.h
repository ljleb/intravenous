#pragma once

#include "linker_event.h"
#include "runtime/iv_module_reload.h"

namespace iv {
using RuntimeIvModuleReloadResultsEvent =
    void (*)(RuntimeIvModuleReloadResults const &);

IV_DECLARE_LINKER_EVENT(
    RuntimeIvModuleReloadResultsEvent,
    iv_runtime_iv_module_reload_results_event);
} // namespace iv
