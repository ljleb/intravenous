#include "runtime/iv_modules_graph_input_lanes_bridge.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/iv_modules_events.h"

namespace iv {
namespace {
RuntimeGraphInputLanes *bound_lanes = nullptr;

void handle_definitions_changed(RuntimeIvModuleDefinitionsChanged const &diff)
{
    if (bound_lanes == nullptr) {
        return;
    }
    bound_lanes->handle_iv_modules_definitions_changed(diff);
}

IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeIvModulesDefinitionsChangedEvent,
    iv_runtime_iv_modules_definitions_changed_event,
    handle_definitions_changed);
} // namespace

void bind_iv_modules_graph_input_lanes_bridge(RuntimeGraphInputLanes &lanes)
{
    bound_lanes = &lanes;
}

void unbind_iv_modules_graph_input_lanes_bridge(
    RuntimeGraphInputLanes const &lanes)
{
    if (bound_lanes == &lanes) {
        bound_lanes = nullptr;
    }
}
} // namespace iv
