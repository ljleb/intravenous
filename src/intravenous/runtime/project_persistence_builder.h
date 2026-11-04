#pragma once

#include <intravenous/runtime/project_connection_types.h>
#include <intravenous/runtime/project_node_instance_types.h>
#include <intravenous/runtime/project_node_replacement_types.h>
#include <intravenous/runtime/startup_config.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace iv {
struct ProjectCommand {
    std::string command{};
    nlohmann::ordered_json args = nlohmann::ordered_json::object();
};

class ProjectPersistenceBuilder {
    std::filesystem::path project_root_;
    StartupConfigState startup_;
    std::optional<ModuleLoaderToolchainConfig> toolchain_config_{};
    bool has_output_device_id_ = false;
    bool has_input_device_id_ = false;
    std::optional<std::string> output_device_id_{};
    std::optional<std::string> input_device_id_{};
    std::vector<ProjectNodeInstanceDeclaration> node_instances_{};
    std::vector<ProjectNodeReplacement> node_replacements_{};
    std::vector<ProjectConnection> graph_connections_{};

    [[nodiscard]] std::string relativize_path(std::filesystem::path const &path) const;

public:
    ProjectPersistenceBuilder(
        std::filesystem::path project_root,
        StartupConfigState startup);

    void add_project_toolchain_config(ModuleLoaderToolchainConfig toolchain_config);
    void add_project_audio_device_selection(
        std::optional<std::string> output_device_id,
        std::optional<std::string> input_device_id);
    void add_node_instances(std::vector<ProjectNodeInstanceDeclaration> instances);
    void add_node_replacements(std::vector<ProjectNodeReplacement> replacements);
    void add_graph_connections(std::vector<ProjectConnection> connections);

    [[nodiscard]] std::vector<ProjectCommand> build() const;
};
} // namespace iv
