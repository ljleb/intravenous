#pragma once

#include "module_test_utils.h"
#include "runtime/graph_input_lanes.h"
#include "runtime/graph_input_lanes_lane_views_bridge.h"
#include "runtime/graph_input_lanes_timeline_bridge.h"
#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_definitions_iv_module_instances_bridge.h"
#include "runtime/iv_module_definitions_project_introspection_bridge.h"
#include "runtime/iv_module_instances.h"
#include "runtime/iv_module_instances_iv_module_definitions_bridge.h"
#include "runtime/iv_module_instances_graph_input_lanes_bridge.h"
#include "runtime/iv_module_reload.h"
#include "runtime/lane_views.h"
#include "runtime/lane_views_timeline_bridge.h"
#include "runtime/project_introspection.h"
#include "runtime/project_introspection_graph_input_lanes_bridge.h"
#include "runtime/startup_config.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace iv::test_support {
struct BoundRuntimeProjectIntrospection {
    iv::Timeline timeline;
    iv::RuntimeIvModuleInstances iv_module_instances;
    iv::RuntimeIvModuleDefinitions iv_module_definitions;
    iv::RuntimeGraphInputLanes graph_input_lanes;
    iv::RuntimeLaneViews lane_views;
    iv::RuntimeProjectIntrospection introspection;
    iv::StartupConfig startup_config;

    static iv::RuntimeIvModuleReloadedDefinition load_definition(
        iv::StartupConfigState const &config,
        std::filesystem::path module_root)
    {
        return iv::test::load_runtime_iv_module_definition(
            config,
            std::move(module_root));
    }

    BoundRuntimeProjectIntrospection(
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots)
        : startup_config(
              std::move(workspace_root),
              std::move(discovery_start),
              std::vector<std::filesystem::path>(std::move(extra_search_roots)))
    {
        iv::bind_graph_input_lanes_timeline_bridge(timeline);
        iv::bind_graph_input_lanes_lane_views_bridge(graph_input_lanes);
        iv::bind_iv_module_instances_iv_module_definitions_bridge(iv_module_definitions);
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
        iv::unbind_iv_module_instances_iv_module_definitions_bridge(iv_module_definitions);
        iv::unbind_graph_input_lanes_lane_views_bridge(graph_input_lanes);
        iv::unbind_graph_input_lanes_timeline_bridge(timeline);
    }

    auto initialize()
    {
        auto const config = startup_config.initialize();
        auto const module_root = std::filesystem::weakly_canonical(config.workspace_root);
        (void)iv_module_instances.create_instance(module_root);
        iv_module_definitions.seed_loaded_definition(load_definition(config, module_root));
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
