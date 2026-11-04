#pragma once

#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/startup_config.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace iv {
class SocketRpcAckResponseBuilder;
class SocketRpcSessionProjectStateResultBuilder;
struct SaveProjectRequest;
struct SessionSnapshotRequest;

class ProjectPersistence {
    std::filesystem::path workspace_root_;
    StartupConfigState startup_;
    mutable std::mutex save_mutex_;

    [[nodiscard]] std::filesystem::path project_file_path() const;
    [[nodiscard]] std::vector<ProjectCommand> read_commands() const;
    [[nodiscard]] std::vector<ProjectCommand> collect_commands() const;
    void emit_message(std::string level, std::string message) const;

public:
    ProjectPersistence(
        std::filesystem::path workspace_root,
        StartupConfigState startup);

    void load();
    void save() const;
    [[nodiscard]] std::vector<ProjectCommand> snapshot() const;
    void handle_socket_rpc_save_project(
        SaveProjectRequest const &request,
        SocketRpcAckResponseBuilder &builder) const;
    void handle_socket_rpc_get_session_project_state(
        SessionSnapshotRequest const& request,
        SocketRpcSessionProjectStateResultBuilder& builder) const;
    void report_autosave_failure(std::string message) const;
};
} // namespace iv
