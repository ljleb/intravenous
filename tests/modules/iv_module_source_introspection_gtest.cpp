#include "../module_test_utils.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/graph_input_lanes_timeline_bridge.h"
#include "runtime/iv_module_definitions_iv_module_source_introspection_bridge.h"
#include "runtime/iv_module_instances_graph_input_lanes_bridge.h"
#include "runtime/lane_filters.h"
#include "runtime/timeline_lane_filters_bridge.h"
#include "runtime/iv_module_source_introspection.h"
#include "runtime/iv_module_source_introspection_graph_input_lanes_bridge.h"
#include "runtime/timeline.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <ranges>
#include <set>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;
using iv::test_support::mutable_module_fixture_workspace;
using iv::test_support::make_inline_module_workspace;
using iv::test_support::read_only_module_fixture_workspace;
using iv::test_support::shared_inline_module_workspace;

struct SeededIvModuleSourceIntrospectionApp {
    iv::Timeline timeline;
    iv::IvModuleDefinitions definitions;
    iv::GraphInputLanes graph_input_lanes;
    iv::LaneFilters lane_filters;
    iv::IvModuleSourceIntrospection introspection;
    iv::StartupConfig startup_config;

    SeededIvModuleSourceIntrospectionApp(
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots)
        : startup_config(
              std::move(workspace_root),
              std::move(discovery_start),
              std::move(extra_search_roots))
    {
        iv::bind_graph_input_lanes_timeline_bridge(graph_input_lanes, timeline);
        iv::bind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
        iv::bind_iv_module_definitions_iv_module_source_introspection_bridge(introspection);
        iv::bind_iv_module_source_introspection_graph_input_lanes_bridge(graph_input_lanes);
        iv::bind_timeline_lane_filters_bridge(lane_filters);
    }

    ~SeededIvModuleSourceIntrospectionApp()
    {
        iv::unbind_iv_module_source_introspection_graph_input_lanes_bridge(graph_input_lanes);
        iv::unbind_iv_module_definitions_iv_module_source_introspection_bridge(introspection);
        iv::unbind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
        iv::unbind_timeline_lane_filters_bridge(lane_filters);
        iv::unbind_graph_input_lanes_timeline_bridge(graph_input_lanes, timeline);
    }

    auto initialize()
    {
        auto const config = startup_config.initialize();
        auto const module_root = std::filesystem::weakly_canonical(config.workspace_root);
        auto loaded = iv::test::load_runtime_iv_module_definition(
            config,
            module_root);

        graph_input_lanes.handle_iv_module_instance_builders_changed(
            iv::IvModuleInstanceBuildersChanged{
                .created = {
                    iv::IvModuleInstanceBuilder{
                        .instance = iv::IvModuleInstance{
                            .instance_id = "instance:1",
                            .definition_id = loaded.definition_id,
                            .module_root = loaded.module_root,
                            .module_id = loaded.module_id,
                            .introspection = loaded.introspection,
                        },
                        .builder = loaded.canonical_builder,
                    },
                },
            });

        definitions.seed_loaded_definition(std::move(loaded));
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

    void set_sample_input_value(
        std::string const &node_id,
        size_t input_ordinal,
        iv::Sample value,
        std::optional<size_t> member_ordinal = std::nullopt)
    {
        graph_input_lanes.set_sample_input_value(
            iv::ProjectSetSampleInputValueRequest {
                .node_id = node_id,
                .member_ordinal = member_ordinal,
                .input_ordinal = input_ordinal,
                .value = value,
            });
    }

    void clear_sample_input_value_override(
        std::string const &node_id,
        size_t member_ordinal,
        size_t input_ordinal)
    {
        graph_input_lanes.clear_sample_input_value_override(
            iv::ProjectClearSampleInputValueOverrideRequest {
                .node_id = node_id,
                .member_ordinal = member_ordinal,
                .input_ordinal = input_ordinal,
            });
    }
};
} // namespace

TEST(IvModuleSourceIntrospection, QueryBySpansReturnsMatchingLiveNodesWithPorts)
{
    auto const workspace = read_only_module_fixture_workspace("local_cmake");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {
            iv::SourceRange{
                .start = {.line = 7, .column = 1},
                .end = {.line = 15, .column = 1},
            },
        });
    ASSERT_FALSE(result.nodes.empty());

    auto const &node = result.nodes.front();
    EXPECT_FALSE(node.id.empty());
    EXPECT_FALSE(node.kind.empty());
    EXPECT_FALSE(node.source_spans.empty());
    bool has_any_port =
        !node.sample_inputs.empty() || !node.sample_outputs.empty() || !node.event_inputs.empty() ||
        !node.event_outputs.empty();
    EXPECT_TRUE(has_any_port);
}

TEST(IvModuleSourceIntrospection, QueryBySpansKeepsDistinctDeclarationsSeparate)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_merged_logical",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"

namespace {
    template<int I>
    iv::NodeRef make_value(iv::GraphBuilder& g, iv::ModuleContext const& context)
    {
        (void)I;
        return g.node<iv::ValueSource>(&context.sample_period()).node_ref();
    }

    void merged_logical_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const a = make_value<0>(g, context);
        auto const b = make_value<1>(g, context);
        auto const sink = context.target_factory().sink(g, 0);
        sink(a + b);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.merged_logical_module", merged_logical_module);
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 22, .column = 1}}});

    size_t value_source_count = 0;
    for (auto const &node : result.nodes) {
        if (!node.kind.contains("ValueSource")) {
            continue;
        }
        ++value_source_count;
        EXPECT_EQ(node.member_count, 1u);
        EXPECT_FALSE(node.source_spans.empty());
    }

    EXPECT_EQ(value_source_count, 2u);
}

TEST(IvModuleSourceIntrospection, QueryBySpansKeepsAnnotatedLogicalNodeIdStableAcrossReload)
{
    auto const workspace = make_inline_module_workspace(
        "iv_module_source_introspection_stable_annotated_id",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"

namespace {
    void annotated_symbol_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const a = _annotate_node_source_info(
            g.node<ValueSource>(&context.sample_period()).node_ref(),
            "decl:annotated_symbol_module::a"
        );
        auto const sink = context.target_factory().sink(g, 0);
        sink(a);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.annotated_symbol_module", annotated_symbol_module);
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    auto const initial = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 1, .column = 1}, .end = {.line = 24, .column = 1}}});

    auto const initial_it = std::find_if(initial.nodes.begin(), initial.nodes.end(), [](auto const &node) {
        return node.kind.contains("ValueSource");
    });

    ASSERT_NE(initial_it, initial.nodes.end());
    auto const initial_id = initial_it->id;
    ASSERT_FALSE(initial_id.empty());

    auto const original_text = iv::test::read_text(module_cpp);
    iv::test::write_text(module_cpp, original_text + "\n");

    std::this_thread::sleep_for(1s);
    auto const reloaded = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 1, .column = 1}, .end = {.line = 25, .column = 1}}});

    iv::test::write_text(module_cpp, original_text);

    auto const reloaded_it =
        std::find_if(reloaded.nodes.begin(), reloaded.nodes.end(), [](auto const &node) {
            return node.kind.contains("ValueSource");
        });

    ASSERT_NE(reloaded_it, reloaded.nodes.end());
    EXPECT_EQ(reloaded_it->id, initial_id);
}

TEST(IvModuleSourceIntrospection, QueryBySpansReturnsAnnotatedLogicalNode)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_annotated_symbol",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"

namespace {
    void annotated_symbol_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const a = _annotate_node_source_info(
            g.node<ValueSource>(&context.sample_period()).node_ref(),
            "decl:annotated_symbol_module::a"
        );
        auto const sink = context.target_factory().sink(g, 0);
        sink(a);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.annotated_symbol_module", annotated_symbol_module);
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 24, .column = 1}}});

    auto const it = std::find_if(result.nodes.begin(), result.nodes.end(), [](auto const &node) {
        return node.kind.contains("ValueSource");
    });

    ASSERT_NE(it, result.nodes.end());
    EXPECT_FALSE(it->id.empty());
    EXPECT_FALSE(it->source_spans.empty());
}

TEST(IvModuleSourceIntrospection, QueryBySpansReturnsSingleAssignedDeclarationBackedRef)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_single_assigned_ref",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"

namespace {
    void assigned_ref_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        NodeRef x;
        x = g.node<ValueSource>(&context.sample_period()).node_ref();
        auto const sink = context.target_factory().sink(g, 0);
        sink(x);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.assigned_ref_module", assigned_ref_module);
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 24, .column = 1}}});

    auto const it = std::find_if(result.nodes.begin(), result.nodes.end(), [](auto const &node) {
        return node.kind.contains("ValueSource");
    });

    ASSERT_NE(it, result.nodes.end());
    EXPECT_FALSE(it->source_spans.empty());
}

TEST(IvModuleSourceIntrospection, InitializationFailsWhenDeclarationBackedRefIsAssignedTwice)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_double_assignment_fails",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"

namespace {
    void assigned_twice_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        NodeRef x;
        x = g.node<ValueSource>(&context.sample_period()).node_ref();
        x = g.node<ValueSource>(&context.sample_period()).node_ref();
        auto const sink = context.target_factory().sink(g, 0);
        sink(x);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.assigned_twice_module", assigned_twice_module);
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    EXPECT_THROW(
        {
            try {
                (void)app.initialize();
            } catch (std::exception const &e) {
                EXPECT_TRUE(std::string(e.what()).contains("already been initialized"));
                throw;
            }
        },
        std::exception);
}

TEST(IvModuleSourceIntrospection, QueryBySpansDoesNotMergeDifferentSchemas)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_schema_mismatch",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/arithmetic.h"

namespace {
    template<size_t Inputs>
    iv::NodeRef make_sum(iv::GraphBuilder& g)
    {
        return g.node<iv::Sum<Inputs>>().node_ref();
    }

    void schema_mismatch_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const a = make_sum<2>(g);
        auto const b = make_sum<3>(g);
        auto const sink = context.target_factory().sink(g, 0);
        sink(a + b);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.schema_mismatch_module", schema_mismatch_module);
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 21, .column = 1}}});

    size_t singleton_sum_count = 0;
    for (auto const &node : result.nodes) {
        if (node.member_count == 1 && node.kind.contains("BinaryOpNode") &&
            (node.sample_inputs.size() == 2 || node.sample_inputs.size() == 3)) {
            ++singleton_sum_count;
        }
    }

    EXPECT_GE(singleton_sum_count, 2u);
}

TEST(IvModuleSourceIntrospection, QueryBySpansAggregatesMixedConnectivity)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_mixed_connectivity",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/arithmetic.h"

namespace {
    template<int I>
    iv::NodeRef make_sum(iv::GraphBuilder& g)
    {
        (void)I;
        return g.node<iv::Sum<1>>().node_ref();
    }

    void mixed_connectivity_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const value = g.node<iv::ValueSource>(&context.sample_period()).node_ref();
        auto const a = make_sum<0>(g);
        auto const b = make_sum<1>(g);
        a(value);
        auto const sink = context.target_factory().sink(g, 0);
        sink(a + b);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.mixed_connectivity_module", mixed_connectivity_module);
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 22, .column = 1}}});

    size_t connected_sum_count = 0;
    size_t disconnected_sum_count = 0;
    for (auto const &node : result.nodes) {
        if (!node.kind.contains("BinaryOpNode") || node.sample_inputs.size() != 1) {
            continue;
        }
        EXPECT_EQ(node.member_count, 1u);
        if (node.sample_inputs.front().connectivity == iv::LogicalPortConnectivity::connected) {
            ++connected_sum_count;
        } else if (
            node.sample_inputs.front().connectivity == iv::LogicalPortConnectivity::disconnected) {
            ++disconnected_sum_count;
        }
    }

    EXPECT_EQ(connected_sum_count, 1u);
    EXPECT_EQ(disconnected_sum_count, 1u);
}

TEST(IvModuleSourceIntrospection, QueryBySpansIntersectsMultipleSelections)
{
    auto const workspace = read_only_module_fixture_workspace("local_cmake");
    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const dt_range = iv::SourceRange{.start = {.line = 8, .column = 20}, .end = {.line = 8, .column = 20}};
    auto const sink_range =
        iv::SourceRange{.start = {.line = 11, .column = 24}, .end = {.line = 11, .column = 24}};

    auto const dt_only = app.query_by_spans(module_cpp, {dt_range}, iv::SourceRangeMatchMode::intersection);
    auto const sink_only = app.query_by_spans(module_cpp, {sink_range}, iv::SourceRangeMatchMode::intersection);
    auto const both =
        app.query_by_spans(module_cpp, {dt_range, sink_range}, iv::SourceRangeMatchMode::intersection);

    auto const ids = [](iv::ProjectQueryResult const &query) {
        std::set<std::string> node_ids;
        for (auto const &node : query.nodes) {
            node_ids.insert(node.id);
        }
        return node_ids;
    };

    auto const dt_ids = ids(dt_only);
    auto const sink_ids = ids(sink_only);
    auto const both_ids = ids(both);

    std::set<std::string> intersection;
    std::set_intersection(
        dt_ids.begin(),
        dt_ids.end(),
        sink_ids.begin(),
        sink_ids.end(),
        std::inserter(intersection, intersection.end()));
    EXPECT_EQ(both_ids, intersection);
}

TEST(IvModuleSourceIntrospection, QueryBySpansUnionsMultipleSelections)
{
    auto const workspace = read_only_module_fixture_workspace("local_cmake");
    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const dt_range = iv::SourceRange{.start = {.line = 8, .column = 20}, .end = {.line = 8, .column = 20}};
    auto const sink_range =
        iv::SourceRange{.start = {.line = 11, .column = 24}, .end = {.line = 11, .column = 24}};

    auto const dt_only = app.query_by_spans(module_cpp, {dt_range}, iv::SourceRangeMatchMode::intersection);
    auto const sink_only = app.query_by_spans(module_cpp, {sink_range}, iv::SourceRangeMatchMode::intersection);
    auto const both = app.query_by_spans(module_cpp, {dt_range, sink_range}, iv::SourceRangeMatchMode::union_);

    auto const ids = [](iv::ProjectQueryResult const &query) {
        std::set<std::string> node_ids;
        for (auto const &node : query.nodes) {
            node_ids.insert(node.id);
        }
        return node_ids;
    };

    auto const dt_ids = ids(dt_only);
    auto const sink_ids = ids(sink_only);
    auto const both_ids = ids(both);

    std::set<std::string> expected_union = dt_ids;
    expected_union.insert(sink_ids.begin(), sink_ids.end());
    EXPECT_EQ(both_ids, expected_union);
}

TEST(IvModuleSourceIntrospection, QueryActiveRegionsReturnsOnlySourceSpans)
{
    auto const workspace = read_only_module_fixture_workspace("local_cmake");
    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const nodes = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 1, .column = 1}, .end = {.line = 1000, .column = 1}}},
        iv::SourceRangeMatchMode::intersection);
    auto const active_regions = app.query_active_regions(module_cpp);

    auto const span_key = [](iv::LiveSourceSpan const &span) {
        return span.file_path + ":" + std::to_string(span.range.start.line) + ":" +
            std::to_string(span.range.start.column) + ":" + std::to_string(span.range.end.line) +
            ":" + std::to_string(span.range.end.column);
    };

    std::set<std::string> expected_spans;
    for (auto const &node : nodes.nodes) {
        for (auto const &span : node.source_spans) {
            expected_spans.insert(span_key(span));
        }
    }

    std::set<std::string> actual_spans;
    for (auto const &span : active_regions.source_spans) {
        actual_spans.insert(span_key(span));
    }
    EXPECT_EQ(actual_spans, expected_spans);
}

TEST(IvModuleSourceIntrospection, QueryBySpansMergesPolyphonicCallbackNodesByExactSourceSpan)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_polyphonic_exact_spans",
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
    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 13, .column = 20}, .end = {.line = 13, .column = 20}}});

    ASSERT_EQ(result.nodes.size(), 1u);
    auto const &logical = result.nodes.front();
    EXPECT_EQ(logical.kind, "iv::SawOscillator");
    EXPECT_EQ(logical.member_count, 2u);
    ASSERT_EQ(logical.members.size(), 2u);
    EXPECT_EQ(logical.members[0].ordinal, 0u);
    EXPECT_EQ(logical.members[1].ordinal, 1u);
    EXPECT_EQ(logical.members[0].kind, "iv::SawOscillator");
    EXPECT_EQ(logical.members[1].kind, "iv::SawOscillator");
    ASSERT_EQ(logical.sample_inputs.size(), 3u);
    EXPECT_EQ(logical.sample_inputs[0].name, "phase_offset");
    EXPECT_EQ(logical.sample_inputs[1].name, "frequency");
    EXPECT_EQ(logical.sample_inputs[2].name, "dt");
    ASSERT_EQ(logical.sample_outputs.size(), 1u);
    EXPECT_EQ(logical.sample_outputs[0].name, "out");

    auto const resolved = app.get_logical_node(logical.id);
    EXPECT_EQ(resolved.kind, "iv::SawOscillator");
    EXPECT_EQ(resolved.member_count, 2u);
    ASSERT_EQ(resolved.sample_inputs.size(), 3u);
    EXPECT_EQ(resolved.sample_inputs[0].name, "phase_offset");
    EXPECT_EQ(resolved.sample_inputs[1].name, "frequency");
    EXPECT_EQ(resolved.sample_inputs[2].name, "dt");
    ASSERT_EQ(resolved.sample_outputs.size(), 1u);
    EXPECT_EQ(resolved.sample_outputs[0].name, "out");

    app.set_sample_input_value(logical.id, 1, 0.25f);
    auto const logical_override = app.get_logical_node(logical.id);
    ASSERT_EQ(logical_override.members.size(), 2u);
    EXPECT_FLOAT_EQ(static_cast<float>(logical_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(logical_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(logical_override.members[1].sample_inputs[1].current_value), 0.25f);
    EXPECT_FALSE(logical_override.members[0].sample_inputs[1].has_concrete_override);
    EXPECT_FALSE(logical_override.members[1].sample_inputs[1].has_concrete_override);

    app.set_sample_input_value(logical.id, 1, 0.75f, 1u);
    auto const concrete_override = app.get_logical_node(logical.id);
    ASSERT_EQ(concrete_override.members.size(), 2u);
    EXPECT_FLOAT_EQ(static_cast<float>(concrete_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(concrete_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(concrete_override.members[1].sample_inputs[1].current_value), 0.75f);
    EXPECT_FALSE(concrete_override.members[0].sample_inputs[1].has_concrete_override);
    EXPECT_TRUE(concrete_override.members[1].sample_inputs[1].has_concrete_override);

    app.clear_sample_input_value_override(logical.id, 1u, 1);
    auto const cleared_override = app.get_logical_node(logical.id);
    ASSERT_EQ(cleared_override.members.size(), 2u);
    EXPECT_FLOAT_EQ(static_cast<float>(cleared_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(cleared_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(cleared_override.members[1].sample_inputs[1].current_value), 0.25f);
    EXPECT_FALSE(cleared_override.members[0].sample_inputs[1].has_concrete_override);
    EXPECT_FALSE(cleared_override.members[1].sample_inputs[1].has_concrete_override);

    EXPECT_TRUE(std::ranges::none_of(result.nodes, [](auto const &node) {
        return node.kind == "Polyphonic";
    }));
}

TEST(IvModuleSourceIntrospection, QueryBySpansDoesNotAttributeInteriorPolyphonicLambdaSpansToOuterSubgraph)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_polyphonic_interior_span",
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
    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 13, .column = 20}, .end = {.line = 13, .column = 20}}});

    ASSERT_EQ(result.nodes.size(), 1u);
    EXPECT_EQ(result.nodes.front().kind, "iv::SawOscillator");
    EXPECT_TRUE(std::ranges::none_of(result.nodes, [](auto const &node) {
        return node.kind == "Polyphonic";
    }));
    EXPECT_TRUE(std::ranges::none_of(result.nodes, [](auto const &node) {
        return node.kind == "PolyphonicVoice";
    }));
}

TEST(IvModuleSourceIntrospection, QueryBySpansReturnsPolyphonicOuterLogicalIdentityAtDeclarationSpan)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_polyphonic_outer_identity",
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
    auto voice = iv::polyphonic<2>(g, [&](auto m) {
        auto const saw = g.node<SawOscillator>();
        saw(
            "phase_offset"_P = 0.0,
            "frequency"_P = 440.0,
            "dt"_P = dt
        );
        return saw * ("amplitude"_P << m);
    });
    auto x = std::move(voice);
    sink(x);
    g.outputs();
}

IV_EXPORT_MODULE("iv.test.polyphonic_module", polyphonic_module);
)");

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 12, .column = 6}, .end = {.line = 12, .column = 10}}});

    ASSERT_FALSE(result.nodes.empty());
    EXPECT_EQ(result.nodes.front().kind, "Polyphonic");
    EXPECT_FALSE(result.nodes.front().source_identity.empty());
    EXPECT_TRUE(result.nodes.front().source_identity.contains("@voice"))
        << result.nodes.front().source_identity;
}

TEST(IvModuleSourceIntrospection, ReloadKeepsLogicalNodeIdsAddressable)
{
    auto const workspace = mutable_module_fixture_workspace("iv_module_source_introspection_reload_epoch", "local_cmake");
    auto const module_cpp = workspace / "module.cpp";
    iv::test_support::write_text(workspace / ".intravenous", "");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const initial = app.query_by_spans(
        std::filesystem::weakly_canonical(module_cpp),
        {{.start = {.line = 7, .column = 1}, .end = {.line = 15, .column = 1}}});
    ASSERT_FALSE(initial.nodes.empty());

    auto const original_text = iv::test::read_text(module_cpp);
    iv::test::write_text(module_cpp, original_text + "\n");

    std::this_thread::sleep_for(1s);
    auto const reloaded = app.query_by_spans(
        std::filesystem::weakly_canonical(module_cpp),
        {{.start = {.line = 7, .column = 1}, .end = {.line = 16, .column = 1}}});

    iv::test::write_text(module_cpp, original_text);

    EXPECT_FALSE(reloaded.nodes.empty());
    EXPECT_NO_THROW((void)app.get_logical_node(initial.nodes.front().id));
}
