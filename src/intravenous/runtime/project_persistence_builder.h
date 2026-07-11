#pragma once

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/lane_view_service.h>
#include <intravenous/runtime/startup_config.h>
#include <intravenous/runtime/timeline_events.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace iv {
struct ProjectCommand {
    std::string command {};
    nlohmann::ordered_json args = nlohmann::ordered_json::object();
};

class ProjectPersistenceBuilder {
    std::filesystem::path project_root_;
    StartupConfigState startup_;
    std::optional<ModuleLoaderToolchainConfig> toolchain_config_ {};
    std::optional<size_t> compiled_sample_cache_chunk_size_multiplier_ {};
    bool has_output_device_id_ = false;
    bool has_input_device_id_ = false;
    std::optional<std::string> output_device_id_ {};
    std::optional<std::string> input_device_id_ {};
    std::optional<std::string> audio_output_lane_id_ {};
    std::optional<std::string> audio_input_lane_id_ {};
    std::vector<IvModuleInstanceInfo> iv_module_instances_ {};
    std::vector<ProjectSetSampleInputValueRequest> sample_input_values_ {};
    std::vector<ProjectSetSampleInputStateRequest> sample_input_states_ {};
    std::vector<ProjectSetEventInputStateRequest> event_input_states_ {};
    std::vector<ProjectSetSampleOutputStateRequest> sample_output_states_ {};
    std::vector<ProjectSetEventOutputStateRequest> event_output_states_ {};
    std::vector<LaneViewRequest> lane_views_ {};
    std::vector<ProjectSetTimelineLaneSampleChannelTypeRequest> lane_sample_channel_types_ {};
    std::vector<ProjectConnectTimelineLanesRequest> lane_connections_ {};

    [[nodiscard]] std::string relativize_path(std::filesystem::path const &path) const;
    [[nodiscard]] static char const *sample_input_state_name(ProjectSampleInputState state);
    [[nodiscard]] static char const *event_input_state_name(ProjectEventInputState state);
    [[nodiscard]] static char const *sample_output_state_name(ProjectSampleOutputState state);
    [[nodiscard]] static char const *event_output_state_name(ProjectEventOutputState state);

public:
    ProjectPersistenceBuilder(
        std::filesystem::path project_root,
        StartupConfigState startup);

    void add_project_toolchain_config(ModuleLoaderToolchainConfig toolchain_config);
    void add_project_compiled_sample_cache_chunk_size_multiplier(size_t multiplier);
    void add_project_audio_device_selection(
        std::optional<std::string> output_device_id,
        std::optional<std::string> input_device_id);
    void add_audio_device_lane_ids(
        std::string output_lane_id,
        std::string input_lane_id);
    void add_iv_module_instances(std::vector<IvModuleInstanceInfo> instances);
    void add_graph_input_authored_state(GraphInputLanes::AuthoredStateSnapshot const &state);
    void add_lane_views(std::vector<LaneViewRequest> views);
    void add_lane_sample_channel_types(
        std::vector<ProjectSetTimelineLaneSampleChannelTypeRequest> requests);
    void add_lane_connections(std::vector<ProjectConnectTimelineLanesRequest> connections);

    [[nodiscard]] std::vector<ProjectCommand> build() const;
};
} // namespace iv
