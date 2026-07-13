#pragma once

#include <filesystem>

namespace iv {
class Timeline;
class TimelineExecution;
class AuthoredLanes;

void bind_runtime_project_timeline_execution_bridge(
    Timeline &timeline,
    TimelineExecution &timeline_execution,
    AuthoredLanes &authored_lanes,
    std::filesystem::path workspace_root);
// Retained for focused users of the generic timeline bridge that do not host
// authored lanes (notably older embedders and unit fixtures).
void bind_runtime_project_timeline_execution_bridge(
    Timeline &timeline,
    TimelineExecution &timeline_execution,
    std::filesystem::path workspace_root);
void unbind_runtime_project_timeline_execution_bridge(
    TimelineExecution const &timeline_execution);
} // namespace iv
