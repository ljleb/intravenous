#pragma once

#include <intravenous/runtime/config.h>
#include <intravenous/module/loader.h>

#include <filesystem>
#include <vector>

namespace iv {
struct StartupConfigState {
    std::filesystem::path workspace_root{};
    std::filesystem::path discovery_start{};
    std::vector<std::filesystem::path> search_roots{};
    ModuleLoader::ToolchainConfig toolchain{};
    RuntimeExecutionConfig execution{};
    std::optional<std::string> output_device_id{std::optional<std::string>("default")};
    std::optional<std::string> input_device_id{std::optional<std::string>("default")};
};

class StartupConfig {
    std::filesystem::path workspace_root;
    std::filesystem::path discovery_start;
    std::vector<std::filesystem::path> extra_search_roots;

public:
    explicit StartupConfig(
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots = {});

    [[nodiscard]] StartupConfigState initialize() const;
};
} // namespace iv
