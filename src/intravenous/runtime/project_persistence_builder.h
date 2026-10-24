#pragma once

#include <intravenous/runtime/iv_module_instance_types.h>
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
    std::vector<IvModuleInstanceInfo> iv_module_instances_{};

    [[nodiscard]] std::string relativize_path(std::filesystem::path const &path) const;

public:
    ProjectPersistenceBuilder(
        std::filesystem::path project_root,
        StartupConfigState startup);

    void add_project_toolchain_config(ModuleLoaderToolchainConfig toolchain_config);
    void add_project_audio_device_selection(
        std::optional<std::string> output_device_id,
        std::optional<std::string> input_device_id);
    void add_iv_module_instances(std::vector<IvModuleInstanceInfo> instances);

    [[nodiscard]] std::vector<ProjectCommand> build() const;
};
} // namespace iv
