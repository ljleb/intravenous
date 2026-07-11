#pragma once

#include <filesystem>

namespace iv {
class Timeline;
class TimelineExecution;

void bind_runtime_project_timeline_execution_bridge(
    Timeline &timeline,
    TimelineExecution &timeline_execution,
    std::filesystem::path workspace_root);
void unbind_runtime_project_timeline_execution_bridge(
    TimelineExecution const &timeline_execution);
} // namespace iv
