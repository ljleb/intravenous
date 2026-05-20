#pragma once

#include <filesystem>
#include <string>

namespace iv {
    inline std::filesystem::path normalize_path(std::filesystem::path const& path)
    {
        return std::filesystem::weakly_canonical(path).lexically_normal();
    }

    inline std::string normalized_path_string(std::filesystem::path const& path)
    {
        return normalize_path(path).generic_string();
    }
}
