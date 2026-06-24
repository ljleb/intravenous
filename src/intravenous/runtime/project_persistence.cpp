#include <intravenous/runtime/project_persistence.h>

#include <intravenous/runtime/runtime_project_events.h>

#include <fstream>
#include <nlohmann/json.hpp>

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

std::optional<InternedString> optional_interned_string(Json const &args, std::string const &key)
{
    auto const it = args.find(key);
    if (it == args.end() || it->is_null()) {
        return std::nullopt;
    }
    if (!it->is_string()) {
        throw std::runtime_error("command args key '" + key + "' must be a string or null");
    }
    return InternedString::from_string(it->get<std::string>());
}

std::optional<std::filesystem::path> parse_optional_path_override_value(
    Json const &value,
    std::filesystem::path const &workspace_root)
{
    if (value.is_null()) {
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

size_t require_size(Json const &args, std::string const &key)
{
    auto const it = args.find(key);
    if (it == args.end() || !it->is_number_integer()) {
        throw std::runtime_error("command args are missing non-negative integer key '" + key + "'");
    }
    auto const value = it->get<std::int64_t>();
    if (value < 0) {
        throw std::runtime_error("command args key '" + key + "' must be non-negative");
    }
    return static_cast<size_t>(value);
}

PortKind require_port_kind(Json const &args, std::string const &key)
{
    auto const value = require_string(args, key);
    if (value == "sample") {
        return PortKind::sample;
    }
    if (value == "event") {
        return PortKind::event;
    }
    throw std::runtime_error("unknown port kind: " + value);
}

LanePortDomain require_port_domain(Json const &args, std::string const &key)
{
    auto const value = require_string(args, key);
    if (value == "compiled") {
        return LanePortDomain::compiled;
    }
    if (value == "realtime") {
        return LanePortDomain::realtime;
    }
    throw std::runtime_error("unknown port domain: " + value);
}

ChannelTypeId require_channel_type(Json const &args, std::string const &key)
{
    auto const value = require_string(args, key);
    if (value == "mono") {
        return ChannelTypeId::mono;
    }
    if (value == "stereo") {
        return ChannelTypeId::stereo;
    }
    throw std::runtime_error("unknown channel type: " + value);
}

ProjectSampleInputState parse_project_sample_input_state(std::string const &state)
{
    if (state == "default") {
        return ProjectSampleInputState::default_;
    }
    if (state == "overridden") {
        return ProjectSampleInputState::overridden;
    }
    if (state == "logicalFollow") {
        return ProjectSampleInputState::logical_follow;
    }
    if (state == "timelineLane") {
        return ProjectSampleInputState::timeline_lane;
    }
    if (state == "disconnected") {
        return ProjectSampleInputState::disconnected;
    }
    throw std::runtime_error("unknown project sample input state: " + state);
}

ProjectEventInputState parse_project_event_input_state(std::string const &state)
{
    if (state == "default") {
        return ProjectEventInputState::default_;
    }
    if (state == "logicalFollow") {
        return ProjectEventInputState::logical_follow;
    }
    if (state == "timelineLane") {
        return ProjectEventInputState::timeline_lane;
    }
    if (state == "disconnected") {
        return ProjectEventInputState::disconnected;
    }
    throw std::runtime_error("unknown project event input state: " + state);
}

ProjectSampleOutputState parse_project_sample_output_state(std::string const &state)
{
    if (state == "disconnected") {
        return ProjectSampleOutputState::disconnected;
    }
    if (state == "logical") {
        return ProjectSampleOutputState::logical;
    }
    if (state == "timelineLane") {
        return ProjectSampleOutputState::timeline_lane;
    }
    throw std::runtime_error("unknown project sample output state: " + state);
}

ProjectEventOutputState parse_project_event_output_state(std::string const &state)
{
    if (state == "disconnected") {
        return ProjectEventOutputState::disconnected;
    }
    if (state == "logical") {
        return ProjectEventOutputState::logical;
    }
    if (state == "timelineLane") {
        return ProjectEventOutputState::timeline_lane;
    }
    throw std::runtime_error("unknown project event output state: " + state);
}

ProjectPersistence *bound_project_persistence = nullptr;

bool is_recognized_project_override_key(std::string const &key)
{
    return key == "c_compiler"
        || key == "cxx_compiler"
        || key == "cmake_program"
        || key == "cmake_generator"
        || key == "make_program"
        || key == "juce_dir"
        || key == "compiled_sample_cache_chunk_size_multiplier"
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
        if (key == "c_compiler" || key == "cxx_compiler" || key == "cmake_program" ||
            key == "make_program" || key == "juce_dir") {
            if (!value.is_null() && !value.is_string()) {
                throw std::runtime_error("project override toolchain settings must be strings or null");
            }
            auto const parsed_value = (value.is_string() && value.get<std::string>() == "default")
                ? std::optional<std::filesystem::path>{}
                : parse_optional_path_override_value(value, workspace_root);
            if (key == "c_compiler") {
                assign_if_present(request.c_compiler, has_any, std::move(parsed_value));
            } else if (key == "cxx_compiler") {
                assign_if_present(request.cxx_compiler, has_any, std::move(parsed_value));
            } else if (key == "cmake_program") {
                assign_if_present(request.cmake_program, has_any, std::move(parsed_value));
            } else if (key == "make_program") {
                assign_if_present(request.make_program, has_any, std::move(parsed_value));
            } else {
                assign_if_present(request.juce_dir, has_any, std::move(parsed_value));
            }
            continue;
        }
        if (key == "cmake_generator") {
            if (!value.is_null() && !value.is_string()) {
                throw std::runtime_error("project override toolchain settings must be strings or null");
            }
            assign_if_present(
                request.cmake_generator,
                has_any,
                (value.is_string() && value.get<std::string>() != "default")
                    ? std::optional<std::string>(value.get<std::string>())
                    : std::optional<std::string>{});
            continue;
        }
        if (key == "compiled_sample_cache_chunk_size_multiplier") {
            if (!value.is_string()
                && (!value.is_number_integer() || value.get<std::int64_t>() < 0)) {
                throw std::runtime_error(
                    "project override setting 'compiled_sample_cache_chunk_size_multiplier' must be a non-negative integer or 'default'");
            }
            if (value.is_string()) {
                if (value.get<std::string>() != "default") {
                    throw std::runtime_error(
                        "project override setting 'compiled_sample_cache_chunk_size_multiplier' must be a non-negative integer or 'default'");
                }
                assign_if_present(
                    request.compiled_sample_cache_chunk_size_multiplier,
                    has_any,
                    std::optional<size_t>{});
            } else {
                assign_if_present(
                    request.compiled_sample_cache_chunk_size_multiplier,
                    has_any,
                    std::optional<size_t>(static_cast<size_t>(value.get<std::int64_t>())));
            }
            continue;
        }
        if (key == "output_device_id" || key == "input_device_id") {
            if (!value.is_null() && !value.is_string()) {
                throw std::runtime_error("project audio device id override settings must be strings or null");
            }
            auto const parsed_value = (value.is_string() && value.get<std::string>() != "default")
                ? std::optional<std::string>(value.get<std::string>())
                : std::optional<std::string>{};
            if (key == "output_device_id") {
                assign_if_present(request.output_device_id, has_any, parsed_value);
            } else {
                assign_if_present(request.input_device_id, has_any, parsed_value);
            }
            continue;
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
    return workspace_root_ / "project.intravenous";
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
        auto trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
        if (trimmed.empty() || trimmed.starts_with('#')) {
            continue;
        }
        auto json = Json::parse(trimmed);
        if (!json.is_object()) {
            throw std::runtime_error("project command line must be an object");
        }
        auto const command_it = json.find("command");
        auto const args_it = json.find("args");
        if (command_it == json.end() || !command_it->is_string()) {
            throw std::runtime_error("project command line is missing string 'command'");
        }
        if (args_it == json.end() || !args_it->is_object()) {
            throw std::runtime_error("project command line is missing object 'args'");
        }
        commands.push_back(ProjectCommand{
            .command = command_it->get<std::string>(),
            .args = *args_it,
        });
    }
    return commands;
}

void ProjectPersistence::apply_command(ProjectCommand const &command)
{
    auto const &args = command.args;

    if (command.command == "project.overrideSettings") {
        auto const request = parse_project_override_settings_request(
            args,
            workspace_root_,
            [&](std::string const &message) {
                emit_message("warning", message);
            });
        if (!request.has_value()) {
            return;
        }
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_override_settings_requested_event,
            *request);
        return;
    }

    if (command.command == "ivModuleInstances.create") {
        ProjectStringBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_create_iv_module_instance_requested_event,
            ProjectCreateIvModuleInstanceRequest{
                .instance_id = require_string(args, "instance_id"),
                .module_root = workspace_root_ / require_string(args, "module_root"),
            },
            builder);
        (void)builder.build();
        return;
    }

    if (command.command == "ivModuleInstances.setDefaultSilenceTtlSamples") {
        ProjectAckBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_iv_module_instance_default_silence_ttl_samples_requested_event,
            ProjectSetIvModuleInstanceDefaultSilenceTtlSamplesRequest{
                .instance_id = require_string(args, "instance_id"),
                .default_silence_ttl_samples = require_size(args, "default_silence_ttl_samples"),
            },
            builder);
        builder.build();
        return;
    }

    if (command.command == "audioDevices.setLaneIds") {
        ProjectAckBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_audio_device_lane_ids_requested_event,
            ProjectSetAudioDeviceLaneIdsRequest{
                .output_lane_id = InternedString::from_string(require_string(args, "output_lane_id")),
                .input_lane_id = InternedString::from_string(require_string(args, "input_lane_id")),
            },
            builder);
        builder.build();
        return;
    }

    if (command.command == "graph.setSampleInputValue") {
        ProjectAckBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_sample_input_value_requested_event,
            ProjectSetSampleInputValueRequest{
                .node_id = require_string(args, "node_id"),
                .member_ordinal = args["member_ordinal"].is_null()
                    ? std::nullopt
                    : std::optional<size_t>(args["member_ordinal"].get<size_t>()),
                .input_ordinal = require_size(args, "input_ordinal"),
                .value = Sample{args["value"].get<Sample::storage>()},
            },
            builder);
        builder.build();
        return;
    }

    if (command.command == "graph.setSampleInputState") {
        ProjectAckBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_sample_input_state_requested_event,
            ProjectSetSampleInputStateRequest{
                .node_id = require_string(args, "node_id"),
                .member_ordinal = args["member_ordinal"].is_null()
                    ? std::nullopt
                    : std::optional<size_t>(args["member_ordinal"].get<size_t>()),
                .input_ordinal = require_size(args, "input_ordinal"),
                .state = parse_project_sample_input_state(require_string(args, "state")),
                .lane_id = optional_interned_string(args, "lane_id"),
            },
            builder);
        builder.build();
        return;
    }

    if (command.command == "graph.setEventInputState") {
        ProjectAckBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_event_input_state_requested_event,
            ProjectSetEventInputStateRequest{
                .node_id = require_string(args, "node_id"),
                .member_ordinal = args["member_ordinal"].is_null()
                    ? std::nullopt
                    : std::optional<size_t>(args["member_ordinal"].get<size_t>()),
                .input_ordinal = require_size(args, "input_ordinal"),
                .state = parse_project_event_input_state(require_string(args, "state")),
                .lane_id = optional_interned_string(args, "lane_id"),
            },
            builder);
        builder.build();
        return;
    }

    if (command.command == "graph.setSampleOutputState") {
        ProjectAckBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_sample_output_state_requested_event,
            ProjectSetSampleOutputStateRequest{
                .node_id = require_string(args, "node_id"),
                .member_ordinal = args["member_ordinal"].is_null()
                    ? std::nullopt
                    : std::optional<size_t>(args["member_ordinal"].get<size_t>()),
                .output_ordinal = require_size(args, "output_ordinal"),
                .state = parse_project_sample_output_state(require_string(args, "state")),
                .lane_id = optional_interned_string(args, "lane_id"),
            },
            builder);
        builder.build();
        return;
    }

    if (command.command == "graph.setEventOutputState") {
        ProjectAckBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_event_output_state_requested_event,
            ProjectSetEventOutputStateRequest{
                .node_id = require_string(args, "node_id"),
                .member_ordinal = args["member_ordinal"].is_null()
                    ? std::nullopt
                    : std::optional<size_t>(args["member_ordinal"].get<size_t>()),
                .output_ordinal = require_size(args, "output_ordinal"),
                .state = parse_project_event_output_state(require_string(args, "state")),
                .lane_id = optional_interned_string(args, "lane_id"),
            },
            builder);
        builder.build();
        return;
    }

    if (command.command == "timeline.openLaneView") {
        ProjectLaneViewBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_open_lane_view_requested_event,
            ProjectOpenLaneViewRequest{
                .request = LaneViewRequest{
                    .view_id = InternedString::from_string(require_string(args, "view_id")),
                    .query = LaneQuery{
                        .filter = LaneQueryFilter{
                            .source = require_string(args, "query_source"),
                        },
                    },
                    .start_index = require_size(args, "start_index"),
                    .visible_lane_count = require_size(args, "visible_lane_count"),
                    .first_sample_index = require_size(args, "first_sample_index"),
                    .last_sample_index = require_size(args, "last_sample_index"),
                    .display_sample_count = require_size(args, "display_sample_count"),
                },
            },
            builder);
        (void)builder.build();
        return;
    }

    if (command.command == "timeline.setLaneSampleChannelType") {
        ProjectAckBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_set_timeline_lane_sample_channel_type_requested_event,
            ProjectSetTimelineLaneSampleChannelTypeRequest{
                .lane_id = InternedString::from_string(require_string(args, "lane_id")),
                .sample_channel_type = require_channel_type(args, "sample_channel_type"),
            },
            builder);
        builder.build();
        return;
    }

    if (command.command == "timeline.connectLanes") {
        ProjectAckBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_connect_timeline_lanes_requested_event,
            ProjectConnectTimelineLanesRequest{
                .source_lane_id = InternedString::from_string(require_string(args, "source_lane_id")),
                .target_lane_id = InternedString::from_string(require_string(args, "target_lane_id")),
                .port_domain = require_port_domain(args, "port_domain"),
                .port_kind = require_port_kind(args, "port_kind"),
                .port_ordinal = require_size(args, "port_ordinal"),
            },
            builder);
        builder.build();
        return;
    }

    throw std::runtime_error("unknown project command: " + command.command);
}

void ProjectPersistence::load()
{
    auto const commands = read_commands();
    for (auto const &command : commands) {
        try {
            apply_command(command);
        } catch (std::exception const &e) {
            emit_message(
                "error",
                "project load command failed for '" + command.command + "': " + e.what());
        }
    }
}

void ProjectPersistence::save() const
{
    ProjectPersistenceBuilder builder(workspace_root_, startup_);
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_persistence_collect_state_event,
        builder);

    auto const commands = builder.build();
    std::ofstream out(project_file_path(), std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write " + project_file_path().string());
    }
    for (auto const &command : commands) {
        out << Json{
            {"command", command.command},
            {"args", command.args},
        }.dump() << '\n';
    }
}

void bind_project_persistence_bridge(ProjectPersistence &project_persistence)
{
    bound_project_persistence = &project_persistence;
}

void unbind_project_persistence_bridge(ProjectPersistence const &project_persistence)
{
    if (bound_project_persistence == &project_persistence) {
        bound_project_persistence = nullptr;
    }
}

namespace {
void handle_project_persistence_save_requested(ProjectAckBuilder &builder)
{
    if (bound_project_persistence == nullptr) {
        return;
    }
    bound_project_persistence->save();
    builder.succeed();
}

IV_SUBSCRIBE_LINKER_EVENT(
    ProjectPersistenceSaveRequestedEvent,
    iv_runtime_project_persistence_save_requested_event,
    handle_project_persistence_save_requested);
}
} // namespace iv
