#include <intravenous/runtime/iv_module_instances_iv_module_sources_bridge.h>

#include <intravenous/runtime/iv_module_sources.h>
#include <intravenous/runtime/iv_module_sources_events.h>

namespace iv {
IV_DEFINE_BRIDGE(iv_module_instances_iv_module_sources_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_instances_iv_module_sources_bridge,
    iv_runtime_iv_module_source_lookup_event,
    &IvModuleSources::handle_iv_module_source_lookup)
} // namespace iv
