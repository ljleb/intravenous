#pragma once

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/lane_views.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/startup_config.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_execution.h>

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace iv {
class ProjectPersistence {
    std::filesystem::path workspace_root_;
    StartupConfigState startup_;

    [[nodiscard]] std::filesystem::path project_file_path() const;
    [[nodiscard]] std::vector<ProjectCommand> read_commands() const;
    void apply_command(ProjectCommand const &command);
    void emit_message(std::string level, std::string message) const;

public:
    ProjectPersistence(
        std::filesystem::path workspace_root,
        StartupConfigState startup);

    void load();
    void save() const;
};

void bind_project_persistence_bridge(ProjectPersistence &project_persistence);
void unbind_project_persistence_bridge(ProjectPersistence const &project_persistence);
} // namespace iv
