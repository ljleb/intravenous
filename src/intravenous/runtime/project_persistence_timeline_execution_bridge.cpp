#include <intravenous/runtime/project_persistence_timeline_execution_bridge.h>

#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/timeline_execution.h>

namespace iv {
IV_DEFINE_BRIDGE(project_persistence_timeline_execution_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_timeline_execution_bridge,
    iv_runtime_project_set_timeline_compiled_sample_cache_chunk_size_multiplier_requested_event,
    &TimelineExecution::handle_project_set_timeline_compiled_sample_cache_chunk_size_multiplier)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_timeline_execution_bridge,
    iv_runtime_project_override_settings_requested_event,
    &TimelineExecution::handle_project_override_settings)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_timeline_execution_bridge,
    iv_runtime_project_persistence_collect_state_event,
    &TimelineExecution::handle_project_persistence_collect_state)
} // namespace iv
