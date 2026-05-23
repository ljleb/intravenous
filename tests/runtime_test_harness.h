#pragma once

#include "module_test_utils.h"
#include "runtime/graph_input_lanes.h"
#include "runtime/graph_input_lanes_lane_views_bridge.h"
#include "runtime/graph_input_lanes_timeline_bridge.h"
#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_definitions_iv_module_instances_bridge.h"
#include "runtime/iv_module_definitions_project_introspection_bridge.h"
#include "runtime/iv_module_instances.h"
#include "runtime/iv_module_instances_graph_input_lanes_bridge.h"
#include "runtime/lane_views.h"
#include "runtime/lane_views_timeline_bridge.h"
#include "runtime/project_introspection.h"
#include "runtime/project_introspection_graph_input_lanes_bridge.h"

#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iv::test_support {
inline std::filesystem::path make_workspace(
    std::string_view name,
    std::source_location location = std::source_location::current())
{
    return iv::test::fresh_test_workspace(name, location);
}

inline void write_text(std::filesystem::path const &path, std::string const &text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + path.string());
    }
    out << text;
}

inline std::filesystem::path copy_fixture_workspace(
    std::string_view test_name,
    std::string const &fixture_name,
    std::source_location location = std::source_location::current())
{
    auto const workspace = make_workspace(test_name, location);
    iv::test::copy_directory(iv::test::test_modules_root() / fixture_name, workspace);
    return workspace;
}

inline std::filesystem::path make_inline_module_workspace(
    std::string_view test_name,
    std::string const &module_text,
    std::source_location location = std::source_location::current())
{
    auto const workspace = make_workspace(test_name, location);
    write_text(workspace / ".intravenous", "");
    write_text(workspace / "module.cpp", module_text);
    return workspace;
}

inline std::string find_program(std::string const &name)
{
    std::string command = "command -v " + name;
    FILE *pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return {};
    }

    std::string output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }
    pclose(pipe);

    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

inline std::string configured_program_or_find(
    std::string const &name,
    char const *configured_path)
{
    if (configured_path != nullptr && *configured_path != '\0') {
        return configured_path;
    }
    return find_program(name);
}

struct ScopedEnvVar {
    std::string key;
    std::optional<std::string> original;

    ScopedEnvVar(std::string key_, std::string value)
        : key(std::move(key_))
    {
        if (char const *existing = std::getenv(key.c_str())) {
            original = existing;
        }
        setenv(key.c_str(), value.c_str(), 1);
    }

    ~ScopedEnvVar()
    {
        if (original.has_value()) {
            setenv(key.c_str(), original->c_str(), 1);
        } else {
            unsetenv(key.c_str());
        }
    }
};

struct BoundRuntimeProjectIntrospection {
    iv::Timeline timeline;
    iv::RuntimeIvModuleDefinitions iv_module_definitions;
    iv::RuntimeIvModuleInstances iv_module_instances;
    iv::RuntimeGraphInputLanes graph_input_lanes;
    iv::RuntimeLaneViews lane_views;
    iv::RuntimeProjectIntrospection introspection;

    BoundRuntimeProjectIntrospection(
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots,
        iv::AudioDeviceFactory audio_device_factory = {})
        : iv_module_definitions(
              std::move(workspace_root),
              std::move(discovery_start),
              std::vector<std::filesystem::path>(std::move(extra_search_roots)),
              std::move(audio_device_factory))
    {
        iv::bind_graph_input_lanes_timeline_bridge(timeline);
        iv::bind_graph_input_lanes_lane_views_bridge(graph_input_lanes);
        iv::bind_iv_module_definitions_iv_module_instances_bridge(iv_module_instances);
        iv::bind_iv_module_definitions_project_introspection_bridge(introspection);
        iv::bind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
        iv::bind_project_introspection_graph_input_lanes_bridge(graph_input_lanes);
        iv::bind_lane_views_timeline_bridge(lane_views);
    }

    ~BoundRuntimeProjectIntrospection()
    {
        iv::unbind_lane_views_timeline_bridge(lane_views);
        iv::unbind_project_introspection_graph_input_lanes_bridge(graph_input_lanes);
        iv::unbind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
        iv::unbind_iv_module_definitions_project_introspection_bridge(introspection);
        iv::unbind_iv_module_definitions_iv_module_instances_bridge(iv_module_instances);
        iv::unbind_graph_input_lanes_lane_views_bridge(graph_input_lanes);
        iv::unbind_graph_input_lanes_timeline_bridge(timeline);
        iv_module_definitions.request_shutdown();
    }

    auto initialize()
    {
        (void)iv_module_definitions.initialize();
        return introspection.initialize();
    }

    auto query_by_spans(
        std::filesystem::path const &file_path,
        std::vector<iv::SourceRange> const &ranges,
        iv::SourceRangeMatchMode match_mode = iv::SourceRangeMatchMode::intersection) const
    {
        return introspection.query_by_spans(file_path, ranges, match_mode);
    }

    auto query_active_regions(std::filesystem::path const &file_path) const
    {
        return introspection.query_active_regions(file_path);
    }

    auto get_logical_node(std::string const &node_id) const
    {
        return introspection.get_logical_node(node_id);
    }

    auto get_logical_nodes(std::vector<std::string> const &node_ids) const
    {
        return introspection.get_logical_nodes(node_ids);
    }

    auto open_lane_view(iv::LaneViewRequest request)
    {
        return lane_views.open_view(std::move(request));
    }

    auto update_lane_view(iv::LaneViewRequest request)
    {
        return lane_views.update_view(std::move(request));
    }

    void close_lane_view(std::string const &view_id)
    {
        lane_views.close_view(view_id);
    }

    void set_sample_input_value(
        std::string const &node_id,
        size_t input_ordinal,
        iv::Sample value,
        std::optional<size_t> member_ordinal = std::nullopt)
    {
        auto const port = introspection.sample_graph_input_port_for_node(
            node_id,
            member_ordinal,
            input_ordinal);
        graph_input_lanes.set_sample_input_value(
            iv::RuntimeProjectSetSampleInputValueRequest{
                .node_id = node_id,
                .member_ordinal = member_ordinal,
                .input_ordinal = input_ordinal,
                .value = value,
                .graph_input_port = port,
            });
    }

    void clear_sample_input_value_override(
        std::string const &node_id,
        size_t member_ordinal,
        size_t input_ordinal)
    {
        auto const port = introspection.sample_graph_input_port_for_node(
            node_id,
            member_ordinal,
            input_ordinal);
        graph_input_lanes.clear_sample_input_value_override(
            iv::RuntimeProjectClearSampleInputValueOverrideRequest{
                .node_id = node_id,
                .member_ordinal = member_ordinal,
                .input_ordinal = input_ordinal,
                .graph_input_port = port,
            });
    }
};
} // namespace iv::test_support
