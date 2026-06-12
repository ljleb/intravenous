#pragma once

#include <filesystem>

namespace iv {
class TimelineExecution;

void bind_socket_rpc_timeline_execution_bridge(
    TimelineExecution &timeline_execution,
    std::filesystem::path workspace_root);
void unbind_socket_rpc_timeline_execution_bridge(TimelineExecution const &timeline_execution);
} // namespace iv
