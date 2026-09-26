#include <intravenous/runtime/project_persistence_builder.h>

#include <algorithm>
#include <ranges>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;

template<typename T>
bool optional_equal(std::optional<T> const &a, std::optional<T> const &b)
{
    return a == b;
}

bool normalized_path_equal(
    std::optional<std::filesystem::path> const &a,
    std::optional<std::filesystem::path> const &b)
{
    if (!a.has_value() || !b.has_value()) {
        return a.has_value() == b.has_value();
    }
    return std::filesystem::weakly_canonical(*a).lexically_normal()
        == std::filesystem::weakly_canonical(*b).lexically_normal();
}
} // namespace

ProjectPersistenceBuilder::ProjectPersistenceBuilder(
    std::filesystem::path project_root,
    StartupConfigState startup)
    : project_root_(std::filesystem::weakly_canonical(std::move(project_root)).lexically_normal())
    , startup_(std::move(startup))
{
}

void ProjectPersistenceBuilder::add_project_toolchain_config(
    ModuleLoaderToolchainConfig toolchain_config)
{
    toolchain_config_ = std::move(toolchain_config);
}

void ProjectPersistenceBuilder::add_project_audio_device_selection(
    std::optional<std::string> output_device_id,
    std::optional<std::string> input_device_id)
{
    has_output_device_id_ = true;
    has_input_device_id_ = true;
    output_device_id_ = std::move(output_device_id);
    input_device_id_ = std::move(input_device_id);
}

void ProjectPersistenceBuilder::add_iv_module_instances(
    std::vector<IvModuleInstanceInfo> instances)
{
    iv_module_instances_ = std::move(instances);
}

std::string ProjectPersistenceBuilder::relativize_path(
    std::filesystem::path const &path) const
{
    auto const normalized = std::filesystem::weakly_canonical(path).lexically_normal();
    std::error_code ec;
    auto relative = std::filesystem::relative(normalized, project_root_, ec);
    if (ec || relative.empty()) {
        return normalized.generic_string();
    }
    return relative.lexically_normal().generic_string();
}

std::vector<ProjectCommand> ProjectPersistenceBuilder::build() const
{
    std::vector<ProjectCommand> commands;
    Json settings = Json::object();

    if (toolchain_config_.has_value()) {
        auto const &toolchain = *toolchain_config_;
        if (!normalized_path_equal(toolchain.c_compiler, startup_.toolchain.c_compiler)) {
            settings["c_compiler"] = toolchain.c_compiler
                ? Json(relativize_path(*toolchain.c_compiler)) : Json(nullptr);
        }
        if (!normalized_path_equal(toolchain.cxx_compiler, startup_.toolchain.cxx_compiler)) {
            settings["cxx_compiler"] = toolchain.cxx_compiler
                ? Json(relativize_path(*toolchain.cxx_compiler)) : Json(nullptr);
        }
        if (!normalized_path_equal(toolchain.cmake_program, startup_.toolchain.cmake_program)) {
            settings["cmake_program"] = toolchain.cmake_program
                ? Json(relativize_path(*toolchain.cmake_program)) : Json(nullptr);
        }
        if (!optional_equal(toolchain.cmake_generator, startup_.toolchain.cmake_generator)) {
            settings["cmake_generator"] = toolchain.cmake_generator
                ? Json(*toolchain.cmake_generator) : Json(nullptr);
        }
        if (!normalized_path_equal(toolchain.make_program, startup_.toolchain.make_program)) {
            settings["make_program"] = toolchain.make_program
                ? Json(relativize_path(*toolchain.make_program)) : Json(nullptr);
        }
        if (!normalized_path_equal(toolchain.juce_dir, startup_.toolchain.juce_dir)) {
            settings["juce_dir"] = toolchain.juce_dir
                ? Json(relativize_path(*toolchain.juce_dir)) : Json(nullptr);
        }
        if (!normalized_path_equal(toolchain.iv_package_pch, startup_.toolchain.iv_package_pch)) {
            settings["iv_package_pch"] = toolchain.iv_package_pch
                ? Json(relativize_path(*toolchain.iv_package_pch)) : Json(nullptr);
        }
    }

    if (has_output_device_id_ && output_device_id_ != startup_.output_device_id) {
        settings["output_device_id"] = output_device_id_
            ? Json(*output_device_id_) : Json(nullptr);
    }
    if (has_input_device_id_ && input_device_id_ != startup_.input_device_id) {
        settings["input_device_id"] = input_device_id_
            ? Json(*input_device_id_) : Json(nullptr);
    }
    if (!settings.empty()) {
        commands.push_back(ProjectCommand{
            .command = "project.overrideSettings",
            .args = std::move(settings),
        });
    }

    auto instances = iv_module_instances_;
    std::ranges::sort(instances, {}, &IvModuleInstanceInfo::instance_id);
    for (auto const &instance : instances) {
        auto const &display_name =
            instance.display_name.empty() ? instance.definition_id : instance.display_name;
        commands.push_back(ProjectCommand{
            .command = "ivModuleInstances.create",
            .args = Json{
                {"instance_id", instance.instance_id},
                {"module_id", instance.definition_id},
                {"package_root", relativize_path(instance.package_root)},
                {"display_name", display_name != instance.definition_id
                    ? Json(display_name) : Json(nullptr)},
            },
        });
    }

    return commands;
}
} // namespace iv
