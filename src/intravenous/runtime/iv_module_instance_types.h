#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace iv {
struct IvModuleInstanceInfo {
    std::string instance_id{};
    std::string definition_id{};
    std::string display_name{};
    std::filesystem::path package_root{};
    bool realized = false;
    std::string module_id{};
};
} // namespace iv
