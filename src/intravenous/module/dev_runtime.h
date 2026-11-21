#pragma once

#include <cstdlib>
#include <filesystem>

namespace iv::details {
inline std::filesystem::path dev_runtime_directory()
{
    auto const* value = std::getenv("INTRAVENOUS_DIR");
    if (!value) return {};
    std::filesystem::path directory(value);
    return directory.is_absolute() && std::filesystem::is_regular_file(directory / ".iv-dev-runtime")
        ? std::filesystem::canonical(directory) : std::filesystem::path{};
}

inline std::filesystem::path runtime_artifact(std::filesystem::path configured)
{
    auto directory = dev_runtime_directory();
    return directory.empty() || configured.empty() ? configured : directory / configured.filename();
}
}
