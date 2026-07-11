#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace iv {
struct IvModuleSourceInfo {
    std::string module_id;
    std::filesystem::path module_root;
    bool project_local = false;
};

class IvModuleSources {
    std::filesystem::path project_root_;
    std::vector<std::filesystem::path> shared_roots_;
public:
    IvModuleSources(std::filesystem::path project_root, std::vector<std::filesystem::path> shared_roots);
    [[nodiscard]] std::vector<IvModuleSourceInfo> list_sources() const;
    [[nodiscard]] std::optional<IvModuleSourceInfo> find_source(
        std::string const& module_id) const;
    [[nodiscard]] IvModuleSourceInfo create_project_source(std::string const& name) const;
};
} // namespace iv
