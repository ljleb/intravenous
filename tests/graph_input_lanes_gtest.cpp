#include "runtime_test_harness.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <ranges>

namespace {
using iv::test_support::BoundRuntimeProjectIntrospection;
using iv::test_support::copy_fixture_workspace;
using iv::test_support::make_inline_module_workspace;
}

TEST(RuntimeGraphInputLanes, QueryLanesReturnsGraphInputSubset)
{
    auto const workspace = copy_fixture_workspace("graph_input_lanes_query", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    BoundRuntimeProjectIntrospection app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const lanes = app.graph_input_lanes.query_lanes(iv::LaneQueryFilter{.kind = "graphInputs"});
    EXPECT_GT(lanes.total_lane_count, 0u);
    EXPECT_FALSE(lanes.lanes.empty());
    bool all_have_ports = true;
    for (auto const &lane : lanes.lanes) {
        all_have_ports = all_have_ports && !lane.graph_input_port.logical_node_id.empty();
    }
    EXPECT_TRUE(all_have_ports);
}

TEST(RuntimeGraphInputLanes, QueryLanesRejectsUnsupportedFilter)
{
    auto const workspace = copy_fixture_workspace("graph_input_lanes_bad_filter", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    BoundRuntimeProjectIntrospection app(workspace, iv::test::repo_root(), {});
    app.initialize();

    EXPECT_THROW(
        (void)app.graph_input_lanes.query_lanes(iv::LaneQueryFilter{.kind = "notGraphInputs"}),
        std::runtime_error);
}

TEST(RuntimeGraphInputLanes, SampleInputMutationsFlowThroughLiveSnapshots)
{
    auto const workspace = make_inline_module_workspace(
        "graph_input_lanes_live_snapshots",
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
    BoundRuntimeProjectIntrospection app(workspace, iv::test::repo_root(), {});
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
