#include <intravenous/runtime/project_persistence.h>

#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;

std::string require_string(Json const &args, std::string const &key)
{
    auto const it = args.find(key);
    if (it == args.end() || !it->is_string()) {
        throw std::runtime_error("command args are missing string key '" + key + "'");
    }
    return it->get<std::string>();
}

std::optional<std::string> optional_nullable_string(
    Json const &args,
    std::string const &key)
{
    auto const it = args.find(key);
    if (it == args.end() || it->is_null()) {
        return std::nullopt;
    }
    if (!it->is_string()) {
        throw std::runtime_error("command args key '" + key + "' must be a string or null");
    }
    return it->get<std::string>();
}

std::filesystem::path require_project_path(
    Json const &args,
    std::string const &key,
    std::filesystem::path const &workspace_root)
{
    auto path = std::filesystem::path(require_string(args, key));
    if (!path.is_absolute()) {
        path = workspace_root / path;
    }
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        throw std::runtime_error(
            "command args key '" + key + "' cannot be resolved: " + ec.message());
    }
    return normalized.lexically_normal();
}

std::optional<std::filesystem::path> parse_optional_path_override_value(
    Json const &value,
    std::filesystem::path const &workspace_root)
{
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_string()) {
        throw std::runtime_error("project override toolchain settings must be strings or null");
    }
    if (value.get<std::string>() == "default") {
        return std::nullopt;
    }
    auto path = std::filesystem::path(value.get<std::string>());
    if (!path.is_absolute()) {
        path = workspace_root / path;
    }
    return std::filesystem::weakly_canonical(path).lexically_normal();
}

template<typename T>
void assign_if_present(
    std::optional<T> &target,
    bool &has_any,
    std::optional<T> value)
{
    target = std::move(value);
    has_any = true;
}

bool is_recognized_project_override_key(std::string const &key)
{
    return key == "c_compiler"
        || key == "cxx_compiler"
        || key == "cmake_program"
        || key == "cmake_generator"
        || key == "make_program"
        || key == "juce_dir"
        || key == "iv_package_pch"
        || key == "output_device_id"
        || key == "input_device_id";
}

std::optional<ProjectOverrideSettingsRequest> parse_project_override_settings_request(
    Json const &args,
    std::filesystem::path const &workspace_root,
    auto const &emit_warning)
{
    ProjectOverrideSettingsRequest request;
    bool has_any = false;

    for (auto const &[key, value] : args.items()) {
        if (!is_recognized_project_override_key(key)) {
            emit_warning(
                "project override setting key is not recognized and will be ignored: " + key);
            continue;
        }

        if (key == "cmake_generator") {
            if (!value.is_null() && !value.is_string()) {
                throw std::runtime_error(
                    "project override toolchain settings must be strings or null");
            }
            assign_if_present(
                request.cmake_generator,
                has_any,
                (value.is_string() && value.get<std::string>() != "default")
                    ? std::optional<std::string>(value.get<std::string>())
                    : std::optional<std::string>{});
            continue;
        }

        if (key == "output_device_id" || key == "input_device_id") {
            if (!value.is_null() && !value.is_string()) {
                throw std::runtime_error(
                    "project audio device id override settings must be strings or null");
            }
            auto parsed = (value.is_string() && value.get<std::string>() != "default")
                ? std::optional<std::string>(value.get<std::string>())
                : std::optional<std::string>{};
            if (key == "output_device_id") {
                assign_if_present(request.output_device_id, has_any, std::move(parsed));
            } else {
                assign_if_present(request.input_device_id, has_any, std::move(parsed));
            }
            continue;
        }

        auto parsed = parse_optional_path_override_value(value, workspace_root);
        if (key == "c_compiler") {
            assign_if_present(request.c_compiler, has_any, std::move(parsed));
        } else if (key == "cxx_compiler") {
            assign_if_present(request.cxx_compiler, has_any, std::move(parsed));
        } else if (key == "cmake_program") {
            assign_if_present(request.cmake_program, has_any, std::move(parsed));
        } else if (key == "make_program") {
            assign_if_present(request.make_program, has_any, std::move(parsed));
        } else if (key == "juce_dir") {
            assign_if_present(request.juce_dir, has_any, std::move(parsed));
        } else if (key == "iv_package_pch") {
            assign_if_present(request.iv_package_pch, has_any, std::move(parsed));
        }
    }

    if (!has_any) {
        return std::nullopt;
    }
    return request;
}
} // namespace

ProjectPersistence::ProjectPersistence(
    std::filesystem::path workspace_root,
    StartupConfigState startup)
    : workspace_root_(std::filesystem::weakly_canonical(std::move(workspace_root)).lexically_normal())
    , startup_(std::move(startup))
{
}

std::filesystem::path ProjectPersistence::project_file_path() const
{
    return workspace_root_ / "iv_project.jsonl";
}

void ProjectPersistence::emit_message(std::string level, std::string message) const
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectMessageNotification{
            .level = std::move(level),
            .message = std::move(message),
        }));
}

std::vector<ProjectCommand> ProjectPersistence::read_commands() const
{
    std::ifstream in(project_file_path());
    if (!in) {
        throw std::runtime_error("failed to open " + project_file_path().string());
    }

    std::vector<ProjectCommand> commands;
    for (std::string line; std::getline(in, line);) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        auto json = Json::parse(line);
        if (!json.is_object()) {
            throw std::runtime_error("project command line must be an object");
        }
        auto const command = json.find("command");
        auto const args = json.find("args");
        if (command == json.end() || !command->is_string()) {
            throw std::runtime_error("project command line is missing string 'command'");
        }
        if (args == json.end() || !args->is_object()) {
            throw std::runtime_error("project command line is missing object 'args'");
        }
        commands.push_back(ProjectCommand{
            .command = command->get<std::string>(),
            .args = *args,
        });
    }
    return commands;
}

void ProjectPersistence::apply_command(ProjectCommand const &command)
{
    auto const &args = command.args;
    if (command.command == "project.overrideSettings") {
        auto request = parse_project_override_settings_request(
            args,
            workspace_root_,
            [&](std::string const &message) { emit_message("warning", message); });
        if (request) {
            IV_INVOKE_LINKER_EVENT(
                iv_runtime_project_override_settings_requested_event,
                *request);
        }
        return;
    }

    if (command.command == "ivModuleInstances.create") {
        ProjectStringBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_create_iv_module_instance_requested_event,
            ProjectCreateIvModuleInstanceRequest{
                .instance_id = require_string(args, "instance_id"),
                .module_id = require_string(args, "module_id"),
                .package_root = require_project_path(args, "package_root", workspace_root_),
                .display_name = optional_nullable_string(args, "display_name"),
            },
            builder);
        (void)builder.build();
        return;
    }

    if (command.command == "ivModuleInstances.update") {
        auto const updates_it = args.find("updates");
        if (updates_it == args.end() || !updates_it->is_array()) {
            throw std::runtime_error("command args are missing array key 'updates'");
        }
        std::vector<ProjectUpdateIvModuleInstance> updates;
        updates.reserve(updates_it->size());
        for (auto const &update : *updates_it) {
            if (!update.is_object()) {
                throw std::runtime_error("command update entry must be an object");
            }
            updates.push_back(ProjectUpdateIvModuleInstance{
                .instance_id = require_string(update, "instance_id"),
                .display_name = optional_nullable_string(update, "display_name"),
            });
        }
        ProjectAckBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_update_iv_module_instances_requested_event,
            ProjectUpdateIvModuleInstancesRequest{.updates = std::move(updates)},
            builder);
        builder.build();
        return;
    }

    throw std::runtime_error("unknown project command: " + command.command);
}

void ProjectPersistence::load()
{
    for (auto const &command : read_commands()) {
        try {
            apply_command(command);
        } catch (std::exception const &error) {
            emit_message(
                "error",
                "project load command failed for '" + command.command + "' args="
                    + command.args.dump() + ": " + error.what());
        }
    }
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_loaded_event);
}

void ProjectPersistence::save() const
{
    std::lock_guard lock(save_mutex_);
    ProjectPersistenceBuilder builder(workspace_root_, startup_);
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_persistence_collect_state_event, builder);

    auto const project_file = project_file_path();
    auto const temporary_file = project_file.parent_path()
        / (project_file.filename().string() + ".tmp");
    std::ofstream out(temporary_file, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write " + temporary_file.string());
    }
    for (auto const &command : builder.build()) {
        out << Json{{"command", command.command}, {"args", command.args}}.dump() << '\n';
    }
    out.close();
    if (!out) {
        throw std::runtime_error("failed to write " + temporary_file.string());
    }

    std::error_code error;
    std::filesystem::rename(temporary_file, project_file, error);
    if (error) {
        std::filesystem::remove(temporary_file, error);
        throw std::runtime_error("failed to replace " + project_file.string());
    }
}

void ProjectPersistence::report_autosave_failure(std::string message) const
{
    emit_message("error", "project autosave failed: " + std::move(message));
}

void ProjectPersistence::handle_socket_rpc_save_project(
    SaveProjectRequest const &,
    SocketRpcAckResponseBuilder &builder) const
{
    try {
        save();
        builder.succeed();
    } catch (std::exception const &error) {
        builder.fail(error.what());
    }
}


} // namespace iv
