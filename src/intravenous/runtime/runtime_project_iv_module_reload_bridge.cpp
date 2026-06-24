#include <intravenous/runtime/runtime_project_iv_module_reload_bridge.h>

#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
namespace {
IvModuleReload *bound_iv_module_reload = nullptr;

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
    auto const assign_path = [&](std::optional<std::filesystem::path> const &value, std::optional<std::filesystem::path> ModuleLoaderToolchainConfig::*field) {
        if (!value.has_value()) {
            return;
        }
        auto const next = value;
        if (next != toolchain.*field) {
            toolchain.*field = next;
            touched = true;
        }
    };
    auto const assign_string = [&](std::optional<std::string> const &value, std::optional<std::string> ModuleLoaderToolchainConfig::*field) {
        if (!value.has_value()) {
            return;
        }
        auto const next = value;
        if (next != toolchain.*field) {
            toolchain.*field = next;
            touched = true;
        }
    };

    assign_path(
        request.c_compiler,
        &ModuleLoaderToolchainConfig::c_compiler);
    assign_path(
        request.cxx_compiler,
        &ModuleLoaderToolchainConfig::cxx_compiler);
    assign_path(
        request.cmake_program,
        &ModuleLoaderToolchainConfig::cmake_program);
    assign_string(
        request.cmake_generator,
        &ModuleLoaderToolchainConfig::cmake_generator);
    assign_path(
        request.make_program,
        &ModuleLoaderToolchainConfig::make_program);
    assign_path(
        request.juce_dir,
        &ModuleLoaderToolchainConfig::juce_dir);
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
