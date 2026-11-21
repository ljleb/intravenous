#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace iv {
// Durable desired-state record for one top-level project node. Runtime
// realization state is deliberately absent.
struct ProjectNodeInstanceDeclaration {
    std::string instance_id{};
    std::string definition_id{};
    std::optional<std::filesystem::path> package_root{};
    std::optional<std::string> display_name{};
    std::string configuration_source{};

    bool operator==(ProjectNodeInstanceDeclaration const&) const = default;
};
} // namespace iv
