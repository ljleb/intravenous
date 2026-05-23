#pragma once

#include <filesystem>
#include <string>

namespace iv {
struct RuntimeIvModuleInstanceInfo {
    std::string instance_id{};
    std::string definition_id{};
    std::filesystem::path module_root{};
    bool realized = false;
    std::string module_id{};
};
} // namespace iv
