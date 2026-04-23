#pragma once

#include "module/loader.h"

#include <filesystem>

namespace iv {
    struct RuntimeProjectConfig {
        std::filesystem::path workspace_root {};
        std::filesystem::path module_root {};
        ModuleLoader::ToolchainConfig toolchain {};
    };

    RuntimeProjectConfig load_runtime_project_config(std::filesystem::path const& workspace_root);
}
