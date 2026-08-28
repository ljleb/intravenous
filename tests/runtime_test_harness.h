#pragma once

#include "module_test_utils.h"
#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes_timeline_bridge.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_instances_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_iv_module_definitions_bridge.h>
#include <intravenous/runtime/iv_module_instances_graph_input_lanes_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/lane_filters.h>
#include <intravenous/runtime/lane_filters_lane_views_bridge.h>
#include <intravenous/runtime/lane_views.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/iv_module_source_introspection_graph_input_lanes_bridge.h>
#include <intravenous/runtime/startup_config.h>
#include <intravenous/runtime/timeline_lane_filters_bridge.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace iv::test_support {
struct BoundIvModuleSourceIntrospection {
    iv::Timeline timeline;
    iv::IvModuleInstances iv_module_instances;
    iv::IvModuleDefinitions iv_module_definitions;
    iv::GraphInputLanes graph_input_lanes;
    iv::LaneFilters lane_filters;
    iv::LaneViews lane_views;
    iv::StartupConfig startup_config;
    iv::IvModuleSourceIntrospection introspection;
    iv::lane_filters_lane_views_bridge::scope lane_filters_lane_views_scope;
    iv::timeline_lane_filters_bridge::scope timeline_lane_filters_scope;
    iv::iv_module_definitions_iv_module_instances_bridge::scope
        iv_module_definitions_iv_module_instances_scope;
    iv::iv_module_instances_iv_module_definitions_bridge::scope
        iv_module_instances_iv_module_definitions_scope;
    iv::iv_module_definitions_iv_module_source_introspection_bridge::scope
        iv_module_definitions_iv_module_source_introspection_scope;
    iv::iv_module_instances_iv_module_source_introspection_bridge::scope
        iv_module_instances_iv_module_source_introspection_scope;
    iv::iv_module_instances_graph_input_lanes_bridge::scope
        iv_module_instances_graph_input_lanes_scope;
    iv::iv_module_source_introspection_graph_input_lanes_bridge::scope
        iv_module_source_introspection_graph_input_lanes_scope;

    static iv::IvModuleReloadedDefinition load_definition(
        iv::StartupConfigState const &config,
        std::filesystem::path module_root)
    {
        return iv::test::load_runtime_iv_module_definition(
            config,
            std::move(module_root));
    }

    BoundIvModuleSourceIntrospection(
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots)
        : startup_config(
              std::move(workspace_root),
              std::move(discovery_start),
              std::vector<std::filesystem::path>(std::move(extra_search_roots))),
          lane_filters_lane_views_scope(
              &lane_filters,
              &lane_views),
          timeline_lane_filters_scope(timeline, lane_filters),
          iv_module_definitions_iv_module_instances_scope(
                iv_module_definitions,
                iv_module_instances),
          iv_module_instances_iv_module_definitions_scope(
              iv_module_instances,
              iv_module_definitions),
          iv_module_definitions_iv_module_source_introspection_scope(
              iv_module_definitions,
              introspection),
          iv_module_instances_iv_module_source_introspection_scope(
              iv_module_instances,
              introspection),
          iv_module_instances_graph_input_lanes_scope(
              iv_module_instances,
              graph_input_lanes),
          iv_module_source_introspection_graph_input_lanes_scope(
              introspection,
              graph_input_lanes)
    {
        iv::bind_graph_input_lanes_timeline_bridge(graph_input_lanes, timeline);
    }

    ~BoundIvModuleSourceIntrospection()
    {
        iv::unbind_graph_input_lanes_timeline_bridge(graph_input_lanes, timeline);
    }

    auto initialize()
    {
        auto const config = startup_config.initialize();
        auto const module_root = std::filesystem::weakly_canonical(config.workspace_root);
        auto definition = load_definition(config, module_root);
        (void)iv_module_instances.create_instance(
            definition.module_id,
            module_root);
        iv_module_definitions.seed_loaded_definition(std::move(definition));
    }

    auto query_by_spans(
        std::filesystem::path const &file_path,
        std::vector<iv::SourceRange> const &ranges,
        iv::SourceRangeMatchMode match_mode = iv::SourceRangeMatchMode::intersection,
        std::optional<std::string> instance_id = std::nullopt) const
    {
        return introspection.query_by_spans(file_path, ranges, match_mode, instance_id);
    }

    auto query_active_regions(std::filesystem::path const &file_path) const
    {
        return introspection.query_active_regions(file_path);
    }

    auto get_virtual_node(std::string const &node_id) const
    {
        return introspection.get_virtual_node(node_id);
    }

    auto get_virtual_nodes(std::vector<std::string> const &node_ids) const
    {
        return introspection.get_virtual_nodes(node_ids);
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
        graph_input_lanes.set_sample_input_value(
            iv::ProjectSetSampleInputValueRequest{
                .node_id = node_id,
                .member_ordinal = member_ordinal,
                .input_ordinal = input_ordinal,
                .value = value,
            });
    }

    void set_sample_input_state(
        std::string const &node_id,
        size_t input_ordinal,
        iv::ProjectSampleInputState state,
        std::optional<size_t> member_ordinal = std::nullopt)
    {
        graph_input_lanes.set_sample_input_state(
            iv::ProjectSetSampleInputStateRequest{
                .node_id = node_id,
                .member_ordinal = member_ordinal,
                .input_ordinal = input_ordinal,
                .state = state,
            });
    }
};
} // namespace iv::test_support
