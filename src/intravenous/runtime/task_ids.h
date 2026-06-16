#pragma once

#include <intravenous/runtime/lane_graph.h>

#include <optional>
#include <string>

namespace iv {
inline std::string timeline_lane_task_id(LaneId lane)
{
    return "timeline:lane:" + std::to_string(lane.value);
}

inline std::string iv_module_instance_dsp_task_id(std::string const& instance_id)
{
    return "iv_module_instance:dsp:" + instance_id;
}

inline std::optional<std::string> task_id_to_iv_module_instance_id(std::string const& task_id)
{
    std::string const prefix = "iv_module_instance:dsp:";
    if (!task_id.starts_with(prefix)) {
        return std::nullopt;
    }
    return task_id.substr(prefix.size());
}
}
