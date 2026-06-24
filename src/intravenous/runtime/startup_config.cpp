#include <intravenous/runtime/startup_config.h>

#include <intravenous/module/search_paths.h>
#include <intravenous/runtime/config.h>

namespace iv {
StartupConfig::StartupConfig(
    std::filesystem::path workspace_root_,
    std::filesystem::path discovery_start_,
    std::vector<std::filesystem::path> extra_search_roots_)
    : workspace_root(std::filesystem::weakly_canonical(workspace_root_).lexically_normal()),
      discovery_start(std::move(discovery_start_)),
      extra_search_roots(std::move(extra_search_roots_))
{
}

StartupConfigState StartupConfig::initialize() const
{
    auto project_config = load_runtime_installation_config(workspace_root);
    auto search_roots = parse_search_path_env();
    search_roots.insert(
        search_roots.end(),
        extra_search_roots.begin(),
        extra_search_roots.end());

    return StartupConfigState{
        .workspace_root = std::move(project_config.workspace_root),
        .discovery_start = discovery_start,
        .search_roots = std::move(search_roots),
        .toolchain = std::move(project_config.toolchain),
        .execution = project_config.execution,
        .output_device_id = std::move(project_config.output_device_id),
        .input_device_id = std::move(project_config.input_device_id),
    };
}
} // namespace iv
