#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace iv {
struct IvModuleInstanceInfo {
    std::string instance_id{};
    std::string definition_id{};
    std::filesystem::path module_root{};
    std::optional<size_t> default_silence_ttl_samples{};
    bool realized = false;
    std::string module_id{};
};
} // namespace iv
