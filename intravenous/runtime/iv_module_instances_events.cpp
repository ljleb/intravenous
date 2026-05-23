#include "runtime/iv_module_instances_events.h"

namespace iv {
IV_DEFINE_LINKER_EVENT(
    RuntimeIvModuleInstancesChangedEvent,
    iv_runtime_iv_module_instances_changed_event);
IV_DEFINE_LINKER_EVENT(
    RuntimeIvModuleSampleInputResolutionRequestedEvent,
    iv_runtime_iv_module_sample_input_resolution_requested_event);
} // namespace iv
