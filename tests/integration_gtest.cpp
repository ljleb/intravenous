#include "runtime_test_harness.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/graph_input_lanes_timeline_bridge.h"
#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_definitions_iv_module_instances_bridge.h"
#include "runtime/iv_module_definitions_iv_module_reload_bridge.h"
#include "runtime/iv_module_definitions_iv_module_source_introspection_bridge.h"
#include "runtime/iv_module_instances.h"
#include "runtime/iv_module_instances_iv_module_definitions_bridge.h"
#include "runtime/iv_module_instances_graph_input_lanes_bridge.h"
#include "runtime/iv_module_reload.h"
#include "runtime/iv_module_reload_events.h"
#include "runtime/iv_module_reload_iv_module_definitions_bridge.h"
#include "runtime/lane_filters.h"
#include "runtime/lane_filters_lane_views_bridge.h"
#include "runtime/lane_views.h"
#include "runtime/iv_module_source_introspection.h"
#include "runtime/startup_config.h"
#include "runtime/timeline.h"
#include "runtime/timeline_lane_filters_bridge.h"

#include <gtest/gtest.h>

namespace {
using iv::test_support::read_only_module_fixture_workspace;
using iv::test_support::shared_inline_module_workspace;

struct IntegrationReloadWitness {
    std::optional<iv::IvModuleReloadResults> results {};
};

IntegrationReloadWitness *g_integration_reload_witness = nullptr;

IV_SUBSCRIBE_LINKER_EVENT(
    iv::IvModuleReloadResultsEvent,
    iv_runtime_iv_module_reload_results_event,
    +[](iv::IvModuleReloadResults const &results) {
        if (g_integration_reload_witness != nullptr) {
            g_integration_reload_witness->results = results;
        }
    });
}

TEST(Integration, StartupConfigDefinitionsAndIvModuleSourceIntrospectionInitializeAndShutdown)
{
    auto const workspace =
        read_only_module_fixture_workspace("local_cmake");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvModuleDefinitions definitions;
    iv::IvModuleSourceIntrospection introspection;
    iv::bind_iv_module_definitions_iv_module_source_introspection_bridge(introspection);

    auto const loaded = iv::test_support::BoundIvModuleSourceIntrospection::load_definition(
        startup,
        std::filesystem::weakly_canonical(workspace));
    auto const module_id = loaded.module_id;
    definitions.seed_loaded_definition(std::move(loaded));
    auto const initialized = introspection.initialize();

    EXPECT_EQ(initialized.module_id, module_id);

    iv::unbind_iv_module_definitions_iv_module_source_introspection_bridge(introspection);
}

TEST(Integration, InstancesDefinitionsReloadAndGraphInputLanesInitializeAndShutdown)
{
    auto const workspace = shared_inline_module_workspace(
        "runtime_integration_graph_input_lanes_initialize",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"

namespace {
    void graph_input_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const dt = g.node<ValueSource>(&context.sample_period());
        auto const sink = context.target_factory().sink(g, 0);
        auto const voices = iv::polyphonic<2>(g, [&](auto m) {
            auto const saw = g.node<SawOscillator>();
            saw(
                "phase_offset"_P = 0.0,
                "frequency"_P = 440.0,
                "dt"_P = dt
            );
            return saw * ("amplitude"_P << m);
        });
        sink(voices);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.graph_input_module", graph_input_module);
)");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::Timeline timeline;
    iv::IvModuleInstances instances;
    iv::IvModuleDefinitions definitions;
    iv::IvModuleReload reload(startup);
    iv::GraphInputLanes graph_input_lanes;
    iv::LaneFilters lane_filters;
    iv::LaneViews lane_views;

    iv::bind_graph_input_lanes_timeline_bridge(graph_input_lanes, timeline);
    iv::bind_timeline_lane_filters_bridge(lane_filters);
    iv::bind_lane_filters_lane_views_bridge(lane_filters, lane_views);
    iv::bind_iv_module_instances_iv_module_definitions_bridge(definitions);
    iv::bind_iv_module_definitions_iv_module_instances_bridge(instances);
    iv::bind_iv_module_definitions_iv_module_reload_bridge(reload);
    iv::bind_iv_module_reload_iv_module_definitions_bridge(definitions);
    iv::bind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);

    IntegrationReloadWitness reload_witness;
    g_integration_reload_witness = &reload_witness;
    (void)instances.create_instance(std::filesystem::weakly_canonical(workspace));
    g_integration_reload_witness = nullptr;

    ASSERT_TRUE(reload_witness.results.has_value());
    ASSERT_TRUE(reload_witness.results->failed.empty())
        << reload_witness.results->failed.front().message;

    auto const loaded_definitions = definitions.loaded_definitions();
    ASSERT_EQ(loaded_definitions.size(), 1u);
    EXPECT_FALSE(loaded_definitions.front().introspection.logical_nodes.empty());

    auto const view = lane_views.open_view(iv::LaneViewRequest{
        .view_id = "view",
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.source = "dsp_graph.graph_input"},
        },
    });
    EXPECT_GT(view.lanes.total_lane_count, 0u);

    iv::unbind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
    iv::unbind_iv_module_reload_iv_module_definitions_bridge(definitions);
    iv::unbind_iv_module_definitions_iv_module_reload_bridge(reload);
    iv::unbind_iv_module_definitions_iv_module_instances_bridge(instances);
    iv::unbind_iv_module_instances_iv_module_definitions_bridge(definitions);
    iv::unbind_lane_filters_lane_views_bridge(lane_filters, lane_views);
    iv::unbind_timeline_lane_filters_bridge(lane_filters);
    iv::unbind_graph_input_lanes_timeline_bridge(graph_input_lanes, timeline);
}

TEST(Integration, SampleInputMutationsFlowThroughLiveSnapshots)
{
    auto const workspace = shared_inline_module_workspace(
        "runtime_integration_live_input_snapshots",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"

void polyphonic_module(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const& io = context.target_factory();
    auto const dt = g.node<ValueSource>(&context.sample_period());
    auto const sink = io.sink(g, 0);
    auto const voices = iv::polyphonic<2>(g, [&](auto m) {
        auto const saw = g.node<SawOscillator>();
        saw(
            "phase_offset"_P = 0.0,
            "frequency"_P = 440.0,
            "dt"_P = dt
        );
        return saw * ("amplitude"_P << m);
    });
    sink(voices);
    g.outputs();
}

IV_EXPORT_MODULE("iv.test.polyphonic_module", polyphonic_module);
)");

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    iv::test_support::BoundIvModuleSourceIntrospection app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 13, .column = 20}, .end = {.line = 13, .column = 20}}});
    ASSERT_EQ(result.nodes.size(), 1u);
    auto const logical_id = result.nodes.front().id;

    app.set_sample_input_value(logical_id, 1, 0.25f);
    auto const logical_override = app.get_logical_node(logical_id);
    EXPECT_FLOAT_EQ(static_cast<float>(logical_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(logical_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(logical_override.members[1].sample_inputs[1].current_value), 0.25f);

    app.set_sample_input_value(logical_id, 1, 0.75f, 1u);
    auto const concrete_override = app.get_logical_node(logical_id);
    EXPECT_TRUE(concrete_override.members[1].sample_inputs[1].has_concrete_override);
    EXPECT_FLOAT_EQ(
        static_cast<float>(concrete_override.members[1].sample_inputs[1].current_value), 0.75f);

    app.clear_sample_input_value_override(logical_id, 1u, 1);
    auto const cleared_override = app.get_logical_node(logical_id);
    EXPECT_FALSE(cleared_override.members[1].sample_inputs[1].has_concrete_override);
    EXPECT_FLOAT_EQ(
        static_cast<float>(cleared_override.members[1].sample_inputs[1].current_value), 0.25f);
}
