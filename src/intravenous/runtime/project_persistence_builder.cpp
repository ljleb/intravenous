#include <intravenous/runtime/project_persistence_builder.h>

#include <algorithm>
#include <stdexcept>
#include <tuple>

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

void ProjectPersistenceBuilder::add_project_compiled_sample_cache_chunk_size_multiplier(
    size_t multiplier)
{
    compiled_sample_cache_chunk_size_multiplier_ = multiplier;
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

void ProjectPersistenceBuilder::add_audio_device_lane_ids(
    std::string output_lane_id,
    std::string input_lane_id)
{
    audio_output_lane_id_ = std::move(output_lane_id);
    audio_input_lane_id_ = std::move(input_lane_id);
}

void ProjectPersistenceBuilder::add_iv_module_instances(
    std::vector<IvModuleInstanceInfo> instances)
{
    iv_module_instances_ = std::move(instances);
}

void ProjectPersistenceBuilder::add_graph_input_authored_state(
    GraphInputLanes::AuthoredStateSnapshot const &state)
{
    sample_input_values_ = state.sample_input_values;
    sample_input_states_ = state.sample_input_states;
    event_input_states_ = state.event_input_states;
    sample_output_states_ = state.sample_output_states;
    event_output_states_ = state.event_output_states;
}

void ProjectPersistenceBuilder::add_lane_sample_channel_types(
    std::vector<ProjectSetTimelineLaneSampleChannelTypeRequest> requests)
{
    lane_sample_channel_types_ = std::move(requests);
}

void ProjectPersistenceBuilder::add_lane_connections(
    std::vector<ProjectConnectTimelineLanesRequest> connections)
{
    lane_connections_ = std::move(connections);
}

void ProjectPersistenceBuilder::add_authored_lane_connections(std::vector<AuthoredLaneConnection> connections)
{
    authored_lane_connections_ = std::move(connections);
}

void ProjectPersistenceBuilder::add_authored_lanes(std::vector<AuthoredLaneRecord> lanes)
{
    authored_lanes_ = std::move(lanes);
}

std::string ProjectPersistenceBuilder::relativize_path(std::filesystem::path const &path) const
{
    auto const normalized = std::filesystem::weakly_canonical(path).lexically_normal();
    std::error_code ec;
    auto relative = std::filesystem::relative(normalized, project_root_, ec);
    if (ec || relative.empty()) {
        return normalized.generic_string();
    }
    return relative.lexically_normal().generic_string();
}

char const *ProjectPersistenceBuilder::sample_input_state_name(ProjectSampleInputState state)
{
    switch (state) {
    case ProjectSampleInputState::default_:
        return "default";
    case ProjectSampleInputState::overridden:
        return "overridden";
    case ProjectSampleInputState::logical_follow:
        return "logicalFollow";
    case ProjectSampleInputState::timeline_lane:
        return "timelineLane";
    case ProjectSampleInputState::disconnected:
        return "disconnected";
    }
    throw std::runtime_error("unknown project sample input state");
}

char const *ProjectPersistenceBuilder::event_input_state_name(ProjectEventInputState state)
{
    switch (state) {
    case ProjectEventInputState::default_:
        return "default";
    case ProjectEventInputState::logical_follow:
        return "logicalFollow";
    case ProjectEventInputState::timeline_lane:
        return "timelineLane";
    case ProjectEventInputState::disconnected:
        return "disconnected";
    }
    throw std::runtime_error("unknown project event input state");
}

char const *ProjectPersistenceBuilder::sample_output_state_name(ProjectSampleOutputState state)
{
    switch (state) {
    case ProjectSampleOutputState::disconnected:
        return "disconnected";
    case ProjectSampleOutputState::logical:
        return "logical";
    case ProjectSampleOutputState::timeline_lane:
        return "timelineLane";
    }
    throw std::runtime_error("unknown project sample output state");
}

char const *ProjectPersistenceBuilder::event_output_state_name(ProjectEventOutputState state)
{
    switch (state) {
    case ProjectEventOutputState::disconnected:
        return "disconnected";
    case ProjectEventOutputState::logical:
        return "logical";
    case ProjectEventOutputState::timeline_lane:
        return "timelineLane";
    }
    throw std::runtime_error("unknown project event output state");
}

std::vector<ProjectCommand> ProjectPersistenceBuilder::build() const
{
    std::vector<ProjectCommand> commands;

    Json settings = Json::object();
    if (toolchain_config_.has_value()) {
        auto const &toolchain = *toolchain_config_;
        if (!normalized_path_equal(toolchain.c_compiler, startup_.toolchain.c_compiler)) {
            settings["c_compiler"] = toolchain.c_compiler.has_value()
                ? Json(relativize_path(*toolchain.c_compiler))
                : Json(nullptr);
        }
        if (!normalized_path_equal(toolchain.cxx_compiler, startup_.toolchain.cxx_compiler)) {
            settings["cxx_compiler"] = toolchain.cxx_compiler.has_value()
                ? Json(relativize_path(*toolchain.cxx_compiler))
                : Json(nullptr);
        }
        if (!normalized_path_equal(toolchain.cmake_program, startup_.toolchain.cmake_program)) {
            settings["cmake_program"] = toolchain.cmake_program.has_value()
                ? Json(relativize_path(*toolchain.cmake_program))
                : Json(nullptr);
        }
        if (!optional_equal(toolchain.cmake_generator, startup_.toolchain.cmake_generator)) {
            settings["cmake_generator"] = toolchain.cmake_generator.has_value()
                ? Json(*toolchain.cmake_generator)
                : Json(nullptr);
        }
        if (!normalized_path_equal(toolchain.make_program, startup_.toolchain.make_program)) {
            settings["make_program"] = toolchain.make_program.has_value()
                ? Json(relativize_path(*toolchain.make_program))
                : Json(nullptr);
        }
        if (!normalized_path_equal(toolchain.juce_dir, startup_.toolchain.juce_dir)) {
            settings["juce_dir"] = toolchain.juce_dir.has_value()
                ? Json(relativize_path(*toolchain.juce_dir))
                : Json(nullptr);
        }
    }
    if (compiled_sample_cache_chunk_size_multiplier_.has_value() &&
        *compiled_sample_cache_chunk_size_multiplier_
            != startup_.execution.compiled_sample_cache_chunk_size_multiplier) {
        settings["compiled_sample_cache_chunk_size_multiplier"] =
            *compiled_sample_cache_chunk_size_multiplier_;
    }
    if (has_output_device_id_ && output_device_id_ != startup_.output_device_id) {
        settings["output_device_id"] = output_device_id_.has_value()
            ? Json(*output_device_id_)
            : Json(nullptr);
    }
    if (has_input_device_id_ && input_device_id_ != startup_.input_device_id) {
        settings["input_device_id"] = input_device_id_.has_value()
            ? Json(*input_device_id_)
            : Json(nullptr);
    }

    if (!settings.empty()) {
        commands.push_back(ProjectCommand{
            .command = "project.overrideSettings",
            .args = settings,
        });
    }

    auto authored_lanes = authored_lanes_;
    std::ranges::sort(authored_lanes, {}, &AuthoredLaneRecord::lane_id);
    for (auto const& lane : authored_lanes) {
        commands.push_back(ProjectCommand{
            .command = "timeline.createAuthoredLane",
            .args = Json{{"lane_id", lane.lane_id.str()}, {"type_id", lane.type_id},
                         {"serialized_state", lane.serialized_state}},
        });
    }

    if (audio_output_lane_id_.has_value() && audio_input_lane_id_.has_value()) {
        commands.push_back(ProjectCommand{
            .command = "audioDevices.setLaneIds",
            .args = nlohmann::ordered_json{
                {"output_lane_id", *audio_output_lane_id_},
                {"input_lane_id", *audio_input_lane_id_},
            },
        });
    }

    auto instances = iv_module_instances_;
    std::ranges::sort(instances, {}, &IvModuleInstanceInfo::instance_id);
    for (auto const &instance : instances) {
        auto const &display_name =
            instance.display_name.empty() ? instance.definition_id : instance.display_name;
        commands.push_back(ProjectCommand{
            .command = "ivModuleInstances.create",
            .args = nlohmann::ordered_json{
                {"instance_id", instance.instance_id},
                {"module_id", instance.definition_id},
                {"display_name", display_name != instance.definition_id
                    ? nlohmann::ordered_json(display_name)
                    : nlohmann::ordered_json(nullptr)},
            },
        });
        if (instance.default_silence_ttl_samples.has_value()) {
            commands.push_back(ProjectCommand{
                .command = "ivModuleInstances.update",
                .args = nlohmann::ordered_json{
                    {"updates", nlohmann::ordered_json::array({
                        nlohmann::ordered_json{
                            {"instance_id", instance.instance_id},
                            {"display_name", nullptr},
                            {"default_silence_ttl_samples", *instance.default_silence_ttl_samples},
                        },
                    })},
                },
            });
        }
    }

    for (auto const &request : sample_input_values_) {
        commands.push_back(ProjectCommand{
            .command = "graph.setSampleInputValue",
            .args = nlohmann::ordered_json{
                {"node_id", request.node_id},
                {"member_ordinal", request.member_ordinal.has_value()
                    ? nlohmann::ordered_json(*request.member_ordinal)
                    : nlohmann::ordered_json(nullptr)},
                {"input_ordinal", request.input_ordinal},
                {"value", request.value.value},
            },
        });
    }

    for (auto const &request : sample_input_states_) {
        commands.push_back(ProjectCommand{
            .command = "graph.setSampleInputState",
            .args = nlohmann::ordered_json{
                {"node_id", request.node_id},
                {"member_ordinal", request.member_ordinal.has_value()
                    ? nlohmann::ordered_json(*request.member_ordinal)
                    : nlohmann::ordered_json(nullptr)},
                {"input_ordinal", request.input_ordinal},
                {"state", sample_input_state_name(request.state)},
                {"lane_id", request.lane_id.has_value()
                    ? nlohmann::ordered_json(request.lane_id->str())
                    : nlohmann::ordered_json(nullptr)},
            },
        });
    }

    for (auto const &request : event_input_states_) {
        commands.push_back(ProjectCommand{
            .command = "graph.setEventInputState",
            .args = nlohmann::ordered_json{
                {"node_id", request.node_id},
                {"member_ordinal", request.member_ordinal.has_value()
                    ? nlohmann::ordered_json(*request.member_ordinal)
                    : nlohmann::ordered_json(nullptr)},
                {"input_ordinal", request.input_ordinal},
                {"state", event_input_state_name(request.state)},
                {"lane_id", request.lane_id.has_value()
                    ? nlohmann::ordered_json(request.lane_id->str())
                    : nlohmann::ordered_json(nullptr)},
            },
        });
    }

    for (auto const &request : sample_output_states_) {
        commands.push_back(ProjectCommand{
            .command = "graph.setSampleOutputState",
            .args = nlohmann::ordered_json{
                {"node_id", request.node_id},
                {"member_ordinal", request.member_ordinal.has_value()
                    ? nlohmann::ordered_json(*request.member_ordinal)
                    : nlohmann::ordered_json(nullptr)},
                {"output_ordinal", request.output_ordinal},
                {"state", sample_output_state_name(request.state)},
                {"lane_id", request.lane_id.has_value()
                    ? nlohmann::ordered_json(request.lane_id->str())
                    : nlohmann::ordered_json(nullptr)},
            },
        });
    }

    for (auto const &request : event_output_states_) {
        commands.push_back(ProjectCommand{
            .command = "graph.setEventOutputState",
            .args = nlohmann::ordered_json{
                {"node_id", request.node_id},
                {"member_ordinal", request.member_ordinal.has_value()
                    ? nlohmann::ordered_json(*request.member_ordinal)
                    : nlohmann::ordered_json(nullptr)},
                {"output_ordinal", request.output_ordinal},
                {"state", event_output_state_name(request.state)},
                {"lane_id", request.lane_id.has_value()
                    ? nlohmann::ordered_json(request.lane_id->str())
                    : nlohmann::ordered_json(nullptr)},
            },
        });
    }

    auto lane_sample_channel_types = lane_sample_channel_types_;
    std::ranges::sort(
        lane_sample_channel_types,
        {},
        &ProjectSetTimelineLaneSampleChannelTypeRequest::lane_id);
    for (auto const &request : lane_sample_channel_types) {
        commands.push_back(ProjectCommand{
            .command = "timeline.setLaneSampleChannelType",
            .args = nlohmann::ordered_json{
                {"lane_id", request.lane_id.str()},
                {"sample_channel_type", request.sample_channel_type == ChannelTypeId::mono ? "mono" : "stereo"},
            },
        });
    }

    auto lane_connections = lane_connections_;
    std::ranges::sort(lane_connections, [](auto const &a, auto const &b) {
        return std::tie(a.source_lane_id, a.target_lane_id, a.port_domain, a.port_kind, a.port_ordinal)
            < std::tie(b.source_lane_id, b.target_lane_id, b.port_domain, b.port_kind, b.port_ordinal);
    });
    for (auto const &connection : lane_connections) {
        commands.push_back(ProjectCommand{
            .command = "timeline.connectLanes",
            .args = nlohmann::ordered_json{
                {"source_lane_id", connection.source_lane_id.str()},
                {"target_lane_id", connection.target_lane_id.str()},
                {"port_domain", connection.port_domain == LanePortDomain::compiled ? "compiled" : "realtime"},
                {"port_kind", connection.port_kind == PortKind::sample ? "sample" : "event"},
                {"port_ordinal", connection.port_ordinal},
            },
        });
    }

    auto connections = authored_lane_connections_;
    std::ranges::sort(connections, [](auto const &a, auto const &b) {
        if (a.source_lane_id != b.source_lane_id) {
            return a.source_lane_id < b.source_lane_id;
        }
        if (a.target_lane_id != b.target_lane_id) {
            return a.target_lane_id < b.target_lane_id;
        }
        if (a.input.domain != b.input.domain) {
            return static_cast<int>(a.input.domain) < static_cast<int>(b.input.domain);
        }
        if (a.input.kind != b.input.kind) {
            return static_cast<int>(a.input.kind) < static_cast<int>(b.input.kind);
        }
        return a.input.ordinal < b.input.ordinal;
    });
    for (auto const &connection : connections) {
        commands.push_back(ProjectCommand{
            .command = "timeline.connectAuthoredLanes",
            .args = nlohmann::ordered_json{
                {"source_lane_id", connection.source_lane_id.str()},
                {"target_lane_id", connection.target_lane_id.str()},
                {"port_domain", connection.input.domain == LanePortDomain::compiled ? "compiled" : "realtime"},
                {"port_kind", connection.input.kind == PortKind::sample ? "sample" : "event"},
                {"port_ordinal", connection.input.ordinal},
            },
        });
    }

    return commands;
}
} // namespace iv
