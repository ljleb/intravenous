#include <intravenous/runtime/startup_config.h>

#include <intravenous/module/search_paths.h>
#include <intravenous/runtime/config.h>

#include <algorithm>
#include <stdexcept>

namespace iv {
namespace {
std::filesystem::path normalize_path(std::filesystem::path const& path)
{
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error) {
        error.clear();
        normalized = std::filesystem::absolute(path, error);
    }
    return (error ? path : normalized).lexically_normal();
}

int provenance_priority(PackageProvenance provenance) noexcept
{
    switch (provenance) {
    case PackageProvenance::project_local:
        return 0;
    case PackageProvenance::common:
        return 1;
    case PackageProvenance::built_in:
        return 2;
    }
    return 0;
}

void add_package_root(
    std::vector<PackageDiscoveryRoot>& roots,
    PackageDiscoveryRoot root)
{
    root.root = normalize_path(root.root);
    if (root.artifact_root) {
        root.artifact_root = normalize_path(*root.artifact_root);
    }
    auto const existing = std::ranges::find(roots, root.root, &PackageDiscoveryRoot::root);
    if (existing == roots.end()) {
        roots.push_back(std::move(root));
        return;
    }
    if (provenance_priority(root.provenance)
        > provenance_priority(existing->provenance)) {
        *existing = std::move(root);
        return;
    }
    if (root.provenance == existing->provenance
        && root.artifact_root != existing->artifact_root) {
        throw std::runtime_error(
            "package discovery root has conflicting artifact owners: "
            + root.root.string());
    }
}
} // namespace

StartupConfig::StartupConfig(
    std::filesystem::path workspace_root_,
    std::filesystem::path discovery_start_,
    std::vector<std::filesystem::path> extra_common_package_roots_)
    : workspace_root(std::filesystem::weakly_canonical(workspace_root_).lexically_normal()),
      discovery_start(std::move(discovery_start_)),
      extra_common_package_roots(std::move(extra_common_package_roots_))
{
}

StartupConfigState StartupConfig::initialize() const
{
    auto project_config = load_runtime_installation_config(workspace_root);
    if (!project_config.builtin_package_roots.empty()
        && !project_config.builtin_package_artifact_root) {
        throw std::runtime_error(
            "built-in package roots require iv_builtin_package_artifact_root");
    }

    std::vector<PackageDiscoveryRoot> package_roots;
    for (auto& root : project_config.builtin_package_roots) {
        add_package_root(package_roots, PackageDiscoveryRoot{
            .root = std::move(root),
            .provenance = PackageProvenance::built_in,
            .artifact_root = project_config.builtin_package_artifact_root,
        });
    }
    auto common_roots = parse_search_path_env();
    common_roots.insert(
        common_roots.end(),
        extra_common_package_roots.begin(),
        extra_common_package_roots.end());
    common_roots.insert(
        common_roots.end(),
        project_config.common_package_roots.begin(),
        project_config.common_package_roots.end());
    for (auto& root : common_roots) {
        add_package_root(package_roots, PackageDiscoveryRoot{
            .root = std::move(root),
            .provenance = PackageProvenance::common,
        });
    }

    return StartupConfigState{
        .workspace_root = std::move(project_config.workspace_root),
        .discovery_start = discovery_start,
        .package_roots = std::move(package_roots),
        .toolchain = std::move(project_config.toolchain),
        .execution = project_config.execution,
        .output_device_id = std::move(project_config.output_device_id),
        .input_device_id = std::move(project_config.input_device_id),
    };
}
} // namespace iv
