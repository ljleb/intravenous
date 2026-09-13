#pragma once

#include <intravenous/module/loader.h>

#include <filesystem>
#include <vector>

namespace iv {
    struct RuntimeExecutionConfig {
        size_t sample_rate = 48000;
        size_t block_size = 256;
        size_t compiled_sample_cache_chunk_size_multiplier = 16;
    };

    struct ProjectConfig {
        std::filesystem::path workspace_root {};
        // Installation-provided package roots. These are ordinary discovery
        // roots: the package catalog decides what they provide.
        std::vector<std::filesystem::path> default_package_roots {};
        ModuleLoaderToolchainConfig toolchain {};
        RuntimeExecutionConfig execution {};
        std::optional<std::string> output_device_id {std::optional<std::string>("default")};
        std::optional<std::string> input_device_id {std::optional<std::string>("default")};
    };

    ProjectConfig load_runtime_installation_config(std::filesystem::path const& workspace_root);
}
