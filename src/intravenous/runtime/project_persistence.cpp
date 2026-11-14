#include <intravenous/runtime/project_persistence.h>

#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/project_connection_json.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <fstream>
#include <optional>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <ranges>
#include <system_error>
#include <tuple>
#include <unordered_map>
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

std::optional<std::filesystem::path> optional_project_path(
    Json const& args,
    std::string const& key,
    std::filesystem::path const& workspace_root)
{
    auto const found = args.find(key);
    if (found == args.end() || found->is_null()) return std::nullopt;
    if (!found->is_string()) {
        throw std::runtime_error(
            "command args key '" + key + "' must be a string or null");
    }
    return require_project_path(args, key, workspace_root);
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

std::optional<ProjectSettingField> project_override_field(std::string const& key)
{
    if (key == "c_compiler") return ProjectSettingField::c_compiler;
    if (key == "cxx_compiler") return ProjectSettingField::cxx_compiler;
    if (key == "cmake_program") return ProjectSettingField::cmake_program;
    if (key == "cmake_generator") return ProjectSettingField::cmake_generator;
    if (key == "make_program") return ProjectSettingField::make_program;
    if (key == "juce_dir") return ProjectSettingField::juce_dir;
    if (key == "iv_package_pch") return ProjectSettingField::iv_package_pch;
    if (key == "output_device_id") return ProjectSettingField::output_device_id;
    if (key == "input_device_id") return ProjectSettingField::input_device_id;
    return std::nullopt;
}

std::optional<ProjectOverrideSettingsRequest> parse_project_override_settings_request(
    Json const &args,
    std::filesystem::path const &workspace_root,
    auto const &emit_warning)
{
    ProjectOverrideSettingsRequest request;
    bool has_any = false;

    for (auto const &[key, value] : args.items()) {
        auto field = project_override_field(key);
        if (!field) {
            emit_warning(
                "project override setting key is not recognized and will be ignored: " + key);
            continue;
        }
        request.specified_fields.push_back(*field);

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
            auto parsed = value.is_string()
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

std::vector<ProjectMessageNotification> ProjectPersistence::load()
{
    std::vector<ProjectMessageNotification> messages;
    IV_EVENT_SOURCE(ProjectPersistence, "project replay") {
        replay(messages);
    }
    return messages;
}

void ProjectPersistence::replay(std::vector<ProjectMessageNotification>& messages)
{
    auto const report = [&](std::string level, std::string message) {
        messages.push_back(ProjectMessageNotification{
            .level = std::move(level),
            .message = std::move(message),
        });
    };
    // Every project.overrideSettings command joins one request, raised once:
    // a later command's setting replaces an earlier one.
    std::optional<ProjectOverrideSettingsRequest> override_settings;
    std::unordered_map<std::string, ProjectNodeInstanceDeclaration> instances;
    std::unordered_map<std::string, ProjectNodeReplacement> node_replacements;
    std::unordered_map<std::string, ProjectConnection> connections;
    std::unordered_map<std::string, ProjectManualControl> manual_controls;
    for (auto const &command : read_commands()) {
        try {
            auto const& args = command.args;
            if (command.command == "project.overrideSettings") {
                auto request = parse_project_override_settings_request(
                    args,
                    workspace_root_,
                    [&](std::string const& message) {
                        report("warning", message);
                    });
                if (request) {
                    if (!override_settings) override_settings.emplace();
                    auto const merge = [&](auto& into, auto const& from, ProjectSettingField field) {
                        if (from || request->specifies(field)) {
                            into = from;
                            if (!override_settings->specifies(field)) override_settings->specified_fields.push_back(field);
                        }
                    };
                    merge(override_settings->c_compiler, request->c_compiler, ProjectSettingField::c_compiler);
                    merge(override_settings->cxx_compiler, request->cxx_compiler, ProjectSettingField::cxx_compiler);
                    merge(override_settings->cmake_program, request->cmake_program, ProjectSettingField::cmake_program);
                    merge(override_settings->cmake_generator, request->cmake_generator, ProjectSettingField::cmake_generator);
                    merge(override_settings->make_program, request->make_program, ProjectSettingField::make_program);
                    merge(override_settings->juce_dir, request->juce_dir, ProjectSettingField::juce_dir);
                    merge(override_settings->iv_package_pch, request->iv_package_pch, ProjectSettingField::iv_package_pch);
                    merge(override_settings->output_device_id, request->output_device_id, ProjectSettingField::output_device_id);
                    merge(override_settings->input_device_id, request->input_device_id, ProjectSettingField::input_device_id);
                }
            } else if (command.command == "nodeInstances.upsert") {
                ProjectNodeInstanceDeclaration declaration{
                    .instance_id = require_string(args, "instance_id"),
                    .definition_id = require_string(args, "definition_id"),
                    .package_root = optional_project_path(
                        args, "package_root", workspace_root_),
                    .display_name = optional_nullable_string(args, "display_name"),
                    .configuration_source = optional_nullable_string(
                        args, "configuration_source").value_or(std::string{}),
                };
                instances[declaration.instance_id] = std::move(declaration);
            } else if (command.command == "nodeInstances.delete") {
                instances.erase(require_string(args, "instance_id"));
            } else if (command.command == "nodeReplacements.upsert") {
                auto const address = args.find("address");
                if (address == args.end()) {
                    throw std::runtime_error(
                        "node replacement args are missing object key 'address'");
                }
                ProjectNodeReplacement replacement{
                    .address = parse_node_address_json(
                        *address, ProjectConnectionJsonNames::project_file),
                    .definition_id = require_string(args, "definition_id"),
                    .package_root = optional_project_path(
                        args, "package_root", workspace_root_),
                    .configuration_arguments_json = [&] {
                        auto const arguments = args.find(
                            "configuration_arguments");
                        if (arguments == args.end() || !arguments->is_array()) {
                            throw std::runtime_error(
                                "node replacement args require array key "
                                "'configuration_arguments'");
                        }
                        return arguments->dump();
                    }(),
                };
                if (replacement.definition_id.empty()) {
                    throw std::runtime_error(
                        "node replacement definition id must not be empty");
                }
                auto key = node_address_json(
                    replacement.address,
                    ProjectConnectionJsonNames::project_file).dump();
                node_replacements[std::move(key)] = std::move(replacement);
            } else if (command.command == "nodeReplacements.delete") {
                auto const address = args.find("address");
                if (address == args.end()) {
                    throw std::runtime_error(
                        "node replacement args are missing object key 'address'");
                }
                node_replacements.erase(node_address_json(
                    parse_node_address_json(
                        *address, ProjectConnectionJsonNames::project_file),
                    ProjectConnectionJsonNames::project_file).dump());
            } else if (command.command == "graphConnections.upsert") {
                auto connection = parse_project_connection_json(
                    args, ProjectConnectionJsonNames::project_file);
                connections[project_connection_id(connection)] = std::move(connection);
            } else if (command.command == "graphConnections.delete") {
                connections.erase(require_string(args, "connection_id"));
            } else if (command.command == "manualControls.upsert") {
                auto const node = args.find("node");
                auto const value = args.find("value");
                auto const enabled = args.find("enabled");
                auto const participation = require_string(args, "participation");
                if (node == args.end()) {
                    throw std::runtime_error(
                        "manual control args are missing object key 'node'");
                }
                if (value == args.end() || !value->is_number()) {
                    throw std::runtime_error(
                        "manual control args are missing numeric key 'value'");
                }
                if (enabled != args.end() && !enabled->is_boolean()) {
                    throw std::runtime_error(
                        "manual control args key 'enabled' must be boolean");
                }
                ManualControlParticipation parsed_participation{};
                if (participation == "disconnected_only") {
                    parsed_participation =
                        ManualControlParticipation::disconnected_only;
                } else if (participation == "participate_with_connections") {
                    parsed_participation =
                        ManualControlParticipation::participate_with_connections;
                } else {
                    throw std::runtime_error(
                        "manual control participation is not recognized");
                }
                ProjectManualControl control{
                    .id = {
                        .node = parse_node_address_json(
                            *node, ProjectConnectionJsonNames::project_file),
                        .input_name = require_string(args, "input_name"),
                    },
                    .stored_value = value->get<float>(),
                    .participation = parsed_participation,
                    .enabled = enabled == args.end() || enabled->get<bool>(),
                };
                auto key = node_address_json(
                    control.id.node,
                    ProjectConnectionJsonNames::project_file).dump()
                    + "\n" + control.id.input_name;
                manual_controls[std::move(key)] = std::move(control);
            } else if (command.command == "manualControls.delete") {
                auto const node = args.find("node");
                if (node == args.end()) {
                    throw std::runtime_error(
                        "manual control args are missing object key 'node'");
                }
                auto key = node_address_json(
                    parse_node_address_json(
                        *node, ProjectConnectionJsonNames::project_file),
                    ProjectConnectionJsonNames::project_file).dump()
                    + "\n" + require_string(args, "input_name");
                manual_controls.erase(key);
            } else {
                throw std::runtime_error("unknown project command: " + command.command);
            }
        } catch (std::exception const &error) {
            report(
                "error",
                "project load command failed for '" + command.command + "' args="
                    + command.args.dump() + ": " + error.what());
        }
    }

    if (override_settings) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_override_settings_requested_event,
            *override_settings);
    }

    ProjectReplaceGraphRequest request;
    request.instances.reserve(instances.size());
    for (auto& [_, declaration] : instances) {
        request.instances.push_back(std::move(declaration));
    }
    request.connections.reserve(connections.size());
    for (auto& [_, connection] : connections) {
        request.connections.push_back(std::move(connection));
    }
    request.node_replacements.reserve(node_replacements.size());
    for (auto& [_, replacement] : node_replacements) {
        request.node_replacements.push_back(std::move(replacement));
    }
    request.manual_controls.reserve(manual_controls.size());
    for (auto& [_, control] : manual_controls) {
        request.manual_controls.push_back(std::move(control));
    }
    std::ranges::sort(
        request.instances, {}, &ProjectNodeInstanceDeclaration::instance_id);
    std::ranges::sort(request.connections, {}, [](ProjectConnection const& connection) {
        return project_connection_id(connection);
    });
    std::ranges::sort(
        request.node_replacements,
        {},
        [](ProjectNodeReplacement const& replacement) {
            return node_address_json(
                replacement.address,
                ProjectConnectionJsonNames::project_file).dump();
        });
    std::ranges::sort(
        request.manual_controls,
        [](ProjectManualControl const& lhs, ProjectManualControl const& rhs) {
            auto const lhs_address = node_address_json(
                lhs.id.node, ProjectConnectionJsonNames::project_file).dump();
            auto const rhs_address = node_address_json(
                rhs.id.node, ProjectConnectionJsonNames::project_file).dump();
            return std::tie(lhs_address, lhs.id.input_name)
                < std::tie(rhs_address, rhs.id.input_name);
        });
    try {
        // Startup replay runs before any client attaches; clients read the
        // replayed instances and virtual nodes when they connect, so the
        // rebuild's publication (builder.take_publication()) is not sent.
        ProjectAckBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_replace_graph_requested_event,
            request,
            builder);
        builder.build();
    } catch (std::exception const& error) {
        report("error", "project graph replay failed: " + std::string(error.what()));
    }
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_loaded_event);
}

std::vector<ProjectCommand> ProjectPersistence::collect_commands() const
{
    ProjectPersistenceBuilder builder(workspace_root_, startup_);
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_persistence_collect_state_event, builder);
    return builder.build();
}

std::vector<ProjectCommand> ProjectPersistence::snapshot() const
{
    std::lock_guard lock(save_mutex_);
    return collect_commands();
}

void ProjectPersistence::save() const
{
    std::lock_guard lock(save_mutex_);
    auto const commands = collect_commands();

    auto const project_file = project_file_path();
    auto const temporary_file = project_file.parent_path()
        / (project_file.filename().string() + ".tmp");
    std::ofstream out(temporary_file, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write " + temporary_file.string());
    }
    for (auto const &command : commands) {
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

void ProjectPersistence::handle_socket_rpc_get_session_project_state(
    SessionSnapshotRequest const&,
    SocketRpcSessionProjectStateResultBuilder& builder) const
{
    try {
        builder.succeed(snapshot());
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}


} // namespace iv
