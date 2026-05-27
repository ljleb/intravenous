#pragma once

#include "module/loader.h"

#include <filesystem>

namespace iv {
    struct ProjectConfig {
        std::filesystem::path workspace_root {};
        ModuleLoader::ToolchainConfig toolchain {};
    };

    ProjectConfig load_runtime_project_config(std::filesystem::path const& workspace_root);
}
