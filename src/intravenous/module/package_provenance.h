#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace iv {
enum class PackageProvenance : std::uint8_t {
    project_local,
    common,
    built_in,
};

// One root searched by PackageWatcherService. `artifact_root` is normally set
// only for built-in packages, whose reusable LLVM belongs to the executable
// distribution rather than to a project or the shared common-package cache.
struct PackageDiscoveryRoot {
    std::filesystem::path root{};
    PackageProvenance provenance = PackageProvenance::project_local;
    std::optional<std::filesystem::path> artifact_root{};

    friend bool operator==(
        PackageDiscoveryRoot const&,
        PackageDiscoveryRoot const&) = default;
};

// Exact source and artifact ownership passed to ModuleLoader for one package.
// A missing artifact_root selects the provenance default: the platform common
// cache for common packages or the containing IV project's build/iv directory
// for project-local packages. Built-in packages require an explicit root.
struct PackageBuildRequest {
    std::filesystem::path package_root{};
    PackageProvenance provenance = PackageProvenance::project_local;
    std::optional<std::filesystem::path> artifact_root{};

    friend bool operator==(
        PackageBuildRequest const&,
        PackageBuildRequest const&) = default;
};
} // namespace iv
