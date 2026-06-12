#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/lane_graph.h>

#include <string>
#include <vector>

namespace iv {
struct GraphInputLanesDspTaskDependencies {
    std::string instance_id {};
    std::vector<LaneId> prerequisite_lanes {};
};

struct GraphInputLanesDspTaskDependenciesChanged {
    std::vector<GraphInputLanesDspTaskDependencies> created_or_updated {};
    std::vector<std::string> deleted_instance_ids {};
};

using GraphInputLanesDspTaskDependenciesChangedEvent =
    void (*)(GraphInputLanesDspTaskDependenciesChanged const &);

IV_DECLARE_LINKER_EVENT(
    GraphInputLanesDspTaskDependenciesChangedEvent,
    iv_runtime_graph_input_lanes_dsp_task_dependencies_changed_event);
}
