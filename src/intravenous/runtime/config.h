#pragma once

#include <intravenous/module/loader.h>

#include <filesystem>

namespace iv {
    struct RuntimeExecutionConfig {
        size_t sample_rate = 48000;
        size_t block_size = 256;
        size_t compiled_sample_cache_chunk_size_multiplier = 16;
    };

    struct ProjectConfig {
        std::filesystem::path workspace_root {};
        ModuleLoader::ToolchainConfig toolchain {};
        RuntimeExecutionConfig execution {};
    };

    ProjectConfig load_runtime_project_config(std::filesystem::path const& workspace_root);
    void set_runtime_project_compiled_sample_cache_chunk_size_multiplier(
        std::filesystem::path const& workspace_root,
        size_t compiled_sample_cache_chunk_size_multiplier);
}
