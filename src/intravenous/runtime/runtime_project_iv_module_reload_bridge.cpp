#include <intravenous/runtime/runtime_project_iv_module_reload_bridge.h>

#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
namespace {
IvModuleReload *bound_iv_module_reload = nullptr;

std::optional<std::filesystem::path> resolve_path_override(
    ProjectOverrideSettingsRequest const &request,
    char const *key,
    std::optional<std::filesystem::path> const &startup_default,
    std::optional<std::filesystem::path> const &current_value)
{
    auto const it = request.args.find(key);
    if (it == request.args.end()) {
        return current_value;
    }
    if (it->is_null()) {
        return std::optional<std::filesystem::path>{};
    }
    if (!it->is_string()) {
        throw std::runtime_error(std::string("project override setting '") + key + "' must be a string or null");
    }
    auto const value = it->get<std::string>();
    if (value == "default") {
        return startup_default;
    }
    std::filesystem::path path = value;
    if (!path.is_absolute()) {
        path = request.workspace_root / path;
    }
    return std::filesystem::weakly_canonical(path).lexically_normal();
}

std::optional<std::string> resolve_string_override(
    ProjectOverrideSettingsRequest const &request,
    char const *key,
    std::optional<std::string> const &startup_default,
    std::optional<std::string> const &current_value)
{
    auto const it = request.args.find(key);
    if (it == request.args.end()) {
        return current_value;
    }
    if (it->is_null()) {
        return std::optional<std::string>{};
    }
    if (!it->is_string()) {
        throw std::runtime_error(std::string("project override setting '") + key + "' must be a string or null");
    }
    auto const value = it->get<std::string>();
    if (value == "default") {
        return startup_default;
    }
    return value;
}

void handle_set_iv_module_toolchain_config(
    ProjectSetIvModuleToolchainConfigRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_iv_module_reload == nullptr) {
        return;
    }
    bound_iv_module_reload->set_toolchain_config(request.toolchain);
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_override_settings(ProjectOverrideSettingsRequest const &request)
{
    if (bound_iv_module_reload == nullptr) {
        return;
    }

    bool touched = false;
    auto toolchain = bound_iv_module_reload->toolchain_config();
    auto const assign_path = [&](char const *key, std::optional<std::filesystem::path> ModuleLoader::ToolchainConfig::*field) {
        auto const next = resolve_path_override(
            request,
            key,
            request.startup.toolchain.*field,
            toolchain.*field);
        if (next != toolchain.*field) {
            toolchain.*field = next;
            touched = true;
        }
    };
    auto const assign_string = [&](char const *key, std::optional<std::string> ModuleLoader::ToolchainConfig::*field) {
        auto const next = resolve_string_override(
            request,
            key,
            request.startup.toolchain.*field,
            toolchain.*field);
        if (next != toolchain.*field) {
            toolchain.*field = next;
            touched = true;
        }
    };

    assign_path("c_compiler", &ModuleLoader::ToolchainConfig::c_compiler);
    assign_path("cxx_compiler", &ModuleLoader::ToolchainConfig::cxx_compiler);
    assign_path("cmake_program", &ModuleLoader::ToolchainConfig::cmake_program);
    assign_string("cmake_generator", &ModuleLoader::ToolchainConfig::cmake_generator);
    assign_path("make_program", &ModuleLoader::ToolchainConfig::make_program);
    assign_path("juce_dir", &ModuleLoader::ToolchainConfig::juce_dir);
    if (!touched) {
        return;
    }
    bound_iv_module_reload->set_toolchain_config(toolchain);
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_collect_state(ProjectPersistenceBuilder &builder)
{
    if (bound_iv_module_reload == nullptr) {
        return;
    }
    builder.add_project_toolchain_config(bound_iv_module_reload->toolchain_config());
}

IV_SUBSCRIBE_LINKER_EVENT(
    ProjectSetIvModuleToolchainConfigRequestedEvent,
    iv_runtime_project_set_iv_module_toolchain_config_requested_event,
    handle_set_iv_module_toolchain_config);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectOverrideSettingsRequestedEvent,
    iv_runtime_project_override_settings_requested_event,
    handle_override_settings);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectPersistenceCollectStateEvent,
    iv_runtime_project_persistence_collect_state_event,
    handle_collect_state);
} // namespace

void bind_runtime_project_iv_module_reload_bridge(IvModuleReload &iv_module_reload)
{
    bound_iv_module_reload = &iv_module_reload;
}

void unbind_runtime_project_iv_module_reload_bridge(IvModuleReload const &iv_module_reload)
{
    if (bound_iv_module_reload == &iv_module_reload) {
        bound_iv_module_reload = nullptr;
    }
}
} // namespace iv
