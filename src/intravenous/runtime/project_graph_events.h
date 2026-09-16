#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/node_instances.h>

namespace iv {
using ProjectGraphNodeInstancesRequestedEvent =
    void (*)(NodeInstancesProjectGraphRequest&);

IV_DECLARE_LINKER_EVENT(
    ProjectGraphNodeInstancesRequestedEvent,
    iv_runtime_project_graph_node_instances_requested_event);
} // namespace iv
