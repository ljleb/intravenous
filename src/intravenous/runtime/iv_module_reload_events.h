#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/iv_module_reload.h>

namespace iv {
using IvModuleReloadResultsEvent =
    void (*)(IvModuleReloadResults const &);
using IvModuleReloadStatusEvent =
    void (*)(IvModuleReloadStatus const &);

IV_DECLARE_LINKER_EVENT(
    IvModuleReloadResultsEvent,
    iv_runtime_iv_module_reload_results_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleReloadStatusEvent,
    iv_runtime_iv_module_reload_status_event);
} // namespace iv
