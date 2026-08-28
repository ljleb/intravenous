#include "../module_test_utils.h"

#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes_timeline_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_instances_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_iv_module_definitions_bridge.h>
#include <intravenous/runtime/iv_module_instances_graph_input_lanes_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/lane_filters.h>
#include <intravenous/runtime/timeline_lane_filters_bridge.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/iv_module_source_introspection_graph_input_lanes_bridge.h>
#include <intravenous/runtime/timeline.h>

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
    iv::IvModuleInstances instances;
    iv::IvModuleDefinitions definitions;
    iv::GraphInputLanes graph_input_lanes;
    iv::LaneFilters lane_filters;
    iv::IvModuleSourceIntrospection introspection;
    iv::StartupConfig startup_config;
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

    SeededIvModuleSourceIntrospectionApp(
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots)
        : startup_config(
              std::move(workspace_root),
              std::move(discovery_start),
              std::move(extra_search_roots)),
          timeline_lane_filters_scope(timeline, lane_filters),
          iv_module_definitions_iv_module_instances_scope(definitions, instances),
          iv_module_instances_iv_module_definitions_scope(instances, definitions),
          iv_module_definitions_iv_module_source_introspection_scope(
              definitions,
              introspection),
          iv_module_instances_iv_module_source_introspection_scope(
              instances,
              introspection),
          iv_module_instances_graph_input_lanes_scope(
              instances,
              graph_input_lanes),
          iv_module_source_introspection_graph_input_lanes_scope(
              introspection,
              graph_input_lanes)
    {
        iv::bind_graph_input_lanes_timeline_bridge(graph_input_lanes, timeline);
    }

    ~SeededIvModuleSourceIntrospectionApp()
    {
        iv::unbind_graph_input_lanes_timeline_bridge(graph_input_lanes, timeline);
    }

    auto initialize()
    {
        auto const config = startup_config.initialize();
        auto const module_root = std::filesystem::weakly_canonical(config.workspace_root);
        auto definition = iv::test::load_runtime_iv_module_definition(config, module_root);
        (void)instances.create_instance(
            definition.module_id,
            module_root,
            "instance:1");
        definitions.seed_loaded_definition(std::move(definition));
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

    auto get_virtual_node(std::string const &node_id) const
    {
        return introspection.get_virtual_node(node_id);
    }

    auto get_virtual_nodes(std::vector<std::string> const &node_ids) const
    {
        return introspection.get_virtual_nodes(node_ids);
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

    void set_sample_input_state(
        std::string const &node_id,
        size_t input_ordinal,
        iv::ProjectSampleInputState state,
        std::optional<size_t> member_ordinal = std::nullopt)
    {
        graph_input_lanes.set_sample_input_state(
            iv::ProjectSetSampleInputStateRequest {
                .node_id = node_id,
                .member_ordinal = member_ordinal,
                .input_ordinal = input_ordinal,
                .state = state,
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
                .end = {.line = 18, .column = 1},
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
        "iv_module_source_introspection_merged_virtual",
        R"(#include <intravenous/dsl.h>

namespace {
    template<int I>
    consteval iv::NodeRef make_value(iv::GraphBuilder& g)
    {
        return g.node<iv::Constant>(static_cast<float>(I)).node_ref();
    }

    consteval void merged_virtual_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const a = make_value<0>(g);
        auto const b = make_value<1>(g);
        auto const sink = a + b;
        g.outputs("main"_P = sink);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 22, .column = 1}}});

    size_t constant_count = 0;
    for (auto const &node : result.nodes) {
        if (!node.kind.contains("Constant")) continue;
        ++constant_count;
        EXPECT_EQ(node.member_count, 1u);
        EXPECT_FALSE(node.source_spans.empty());
    }
    EXPECT_EQ(constant_count, 2u);
}

TEST(IvModuleSourceIntrospection, GenericChannelOutputArgumentsArePublicOutputSourceSpans)
{
    auto const workspace = make_inline_module_workspace(
        "iv_module_source_introspection_generic_channel_outputs",
        R"(#include <intravenous/dsl.h>

namespace {
    consteval void generic_channel_outputs(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const source = g.node<Constant>(0.25f);
        g.outputs("main"_P[stereo::left] = source, "main"_P[stereo::right] = source);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();
    app.introspection.set_public_sample_outputs(app.graph_input_lanes.public_sample_outputs());
    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 16, .column = 1}}});
    std::vector<iv::VirtualNodeInfo const*> outputs;
    for (auto const& node : result.nodes) if (node.kind == "Public output") outputs.push_back(&node);
    ASSERT_EQ(outputs.size(), 2u);
    for (auto const* output : outputs) {
        ASSERT_FALSE(output->source_spans.empty());
        ASSERT_EQ(output->sample_outputs.size(), 1u);
        EXPECT_EQ(output->sample_outputs.front().connectivity, iv::VirtualPortConnectivity::connected);
        EXPECT_EQ(output->sample_outputs.front().state_value, "timelineLane");
        ASSERT_EQ(output->members.size(), 2u);
        for (auto const& member : output->members) {
            ASSERT_EQ(member.sample_outputs.size(), 1u);
            EXPECT_EQ(member.sample_outputs.front().state_value, "virtualFollow");
            EXPECT_EQ(member.sample_outputs.front().connectivity, iv::VirtualPortConnectivity::connected);
        }
    }
}

TEST(IvModuleSourceIntrospection, QueryBySpansKeepsAnnotatedVirtualNodeIdStableAcrossReload)
{
    auto const workspace = make_inline_module_workspace(
        "iv_module_source_introspection_stable_annotated_id",
        R"(#include <intravenous/dsl.h>

namespace {
    consteval void annotated_symbol_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const a = _annotate_node_source_info(
            g.node<Constant>(0.0f).node_ref(),
            "decl:annotated_symbol_module::a"
        );
        auto const& sink = a;
        g.outputs("main"_P = sink);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    auto const initial = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 1, .column = 1}, .end = {.line = 24, .column = 1}}});

    auto const initial_it = std::find_if(initial.nodes.begin(), initial.nodes.end(), [](auto const &node) {
        return node.kind.contains("Constant");
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

    auto const reloaded_it = std::find_if(reloaded.nodes.begin(), reloaded.nodes.end(), [](auto const &node) {
        return node.kind.contains("Constant");
    });
    ASSERT_NE(reloaded_it, reloaded.nodes.end());
    EXPECT_EQ(reloaded_it->id, initial_id);
}

TEST(IvModuleSourceIntrospection, QueryBySpansReturnsAnnotatedVirtualNode)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_annotated_symbol",
        R"(#include <intravenous/dsl.h>

namespace {
    consteval void annotated_symbol_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const a = _annotate_node_source_info(
            g.node<Constant>(0.0f).node_ref(),
            "decl:annotated_symbol_module::a"
        );
        auto const& sink = a;
        g.outputs("main"_P = sink);
    }
}

)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 24, .column = 1}}});

    auto const it = std::find_if(result.nodes.begin(), result.nodes.end(), [](auto const &node) {
        return node.kind.contains("Constant");
    });
    ASSERT_NE(it, result.nodes.end());
    EXPECT_FALSE(it->id.empty());
    EXPECT_FALSE(it->source_spans.empty());
}

TEST(IvModuleSourceIntrospection, AnnotatesTiledSampleValueAndAggregateNodeCall)
{
    auto const workspace = make_inline_module_workspace(
        "iv_module_source_introspection_tiled_value_and_node_call",
        R"(#include <intravenous/dsl.h>

namespace {
    struct TriggerSource
    {
        static constexpr auto event_outputs()
        {
            return std::array<iv::EventOutputConfig, 1>{{{
                .name = "trigger", .type = iv::EventTypeId::trigger}}};
        }
        void tick(iv::TickSampleContext<TriggerSource> const&) const {}
    };

    consteval void tiled_value_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const left = g.node<Constant>(0.25f);
        auto const right = g.node<Constant>(-0.25f);
        auto const p = g.tile<stereo>(left, right);
        auto const trigger = g.node<TriggerSource>().event_port();
        auto const sink = g.node<Sum<stereo, SampleStreamLayout::planar, 1>>();
        sink(p);
        g.outputs(sink);
        g.event_outputs(trigger);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(
        workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const module_cpp =
        std::filesystem::weakly_canonical(workspace / "module.cpp");
    auto const tiled_value = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 19, .column = 1},
          .end = {.line = 19, .column = 80}}});
    auto const port = std::find_if(
        tiled_value.nodes.begin(), tiled_value.nodes.end(),
        [](auto const& node) { return node.type_identity == "sample-port"; });
    ASSERT_NE(port, tiled_value.nodes.end());
    ASSERT_EQ(port->sample_outputs.size(), 1u);
    EXPECT_EQ(
        port->sample_outputs.front().sample_channel_type,
        iv::ChannelTypeId::stereo);
    ASSERT_EQ(port->members.size(), 1u);
    for (auto const& member : port->members)
        EXPECT_EQ(member.sample_outputs.size(), 1u);

    auto const event_value = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 20, .column = 1},
          .end = {.line = 20, .column = 80}}});
    auto const event_port = std::find_if(
        event_value.nodes.begin(), event_value.nodes.end(),
        [](auto const& node) { return node.type_identity == "event-port"; });
    ASSERT_NE(event_port, event_value.nodes.end());
    ASSERT_EQ(event_port->event_outputs.size(), 1u);
    ASSERT_EQ(event_port->members.size(), 1u);
    EXPECT_EQ(event_port->members.front().event_outputs.size(), 1u);

    auto const node_call = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 22, .column = 1},
          .end = {.line = 22, .column = 40}}});
    auto const sink = std::find_if(
        node_call.nodes.begin(), node_call.nodes.end(),
        [](auto const& node) {
            return node.kind.contains("Sum")
                && node.type_identity != "sample-port";
        });
    ASSERT_NE(sink, node_call.nodes.end());
    EXPECT_FALSE(sink->source_spans.empty());
}

TEST(IvModuleSourceIntrospection, QueryBySpansReturnsSingleAssignedDeclarationBackedRef)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_single_assigned_ref",
        R"(#include <intravenous/dsl.h>

namespace {
    consteval void assigned_ref_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        NodeRef x;
        x = g.node<Constant>(0.0f).node_ref();
        auto const& sink = x;
        g.outputs("main"_P = sink);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 24, .column = 1}}});

    auto const it = std::find_if(result.nodes.begin(), result.nodes.end(), [](auto const &node) {
        return node.kind.contains("Constant");
    });
    ASSERT_NE(it, result.nodes.end());
    EXPECT_FALSE(it->source_spans.empty());
}

TEST(IvModuleSourceIntrospection, InitializationFailsWhenDeclarationBackedRefIsAssignedTwice)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_double_assignment_fails",
        R"(#include <intravenous/dsl.h>

namespace {
    consteval void assigned_twice_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        NodeRef x;
        x = g.node<Constant>(0.0f).node_ref();
        x = g.node<Constant>(1.0f).node_ref();
        auto const& sink = x;
        g.outputs("main"_P = sink);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    EXPECT_THROW((void)app.initialize(), std::exception);
}

TEST(IvModuleSourceIntrospection, QueryBySpansDoesNotMergeDifferentSchemas)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_schema_mismatch",
        R"(#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/arithmetic.h>

namespace {
    template<size_t Inputs>
    consteval iv::NodeRef make_sum(iv::GraphBuilder& g)
    {
        return g.node<iv::Sum<iv::mono, iv::SampleStreamLayout::planar, Inputs>>().node_ref();
    }

    consteval void schema_mismatch_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const a = make_sum<2>(g);
        auto const b = make_sum<3>(g);
        auto const sink = a + b;
        g.outputs("main"_P = sink);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 21, .column = 1}}});

    size_t singleton_sum_count = 0;
    for (auto const &node : result.nodes) {
        if (node.member_count == 1 && node.kind.contains("Sum<") &&
            (node.sample_inputs.size() == 2 || node.sample_inputs.size() == 3)) {
            ++singleton_sum_count;
        }
    }
    EXPECT_GE(singleton_sum_count, 2u);
}

TEST(IvModuleSourceIntrospection, SameLvalueWithDifferentNodeTypesProducesIndependentVirtualNodes)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_split_lvalue_types",
        R"(#include <intravenous/dsl.h>

namespace {
    consteval void split_lvalue_types_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto make_branch = [&]<bool Add>(auto output) {
            auto const value = g.node<Constant>(1.0f);
            NodeRef p;
            if constexpr (Add) {
                p = value + 1.0f;
            } else {
                p = value - 1.0f;
            }
            g.outputs(output = p);
        };

        make_branch.template operator()<true>("first_sum"_P);
        make_branch.template operator()<false>("difference"_P);
        make_branch.template operator()<true>("second_sum"_P);
    }
}
)"
    );

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        { { .start = { .line = 1, .column = 1 }, .end = { .line = 28, .column = 1 } } }
    );

    std::vector<iv::VirtualNodeInfo const*> split_nodes;
    for (auto const& node : result.nodes) {
        if (node.source_identity.ends_with("@p"))
            split_nodes.push_back(&node);
    }
    ASSERT_EQ(split_nodes.size(), 2u);
    EXPECT_EQ(split_nodes[0]->source_identity, split_nodes[1]->source_identity);
    EXPECT_NE(split_nodes[0]->id, split_nodes[1]->id);
    EXPECT_NE(split_nodes[0]->type_identity, split_nodes[1]->type_identity);
    EXPECT_TRUE(split_nodes[0]->id.contains("#type:"));
    EXPECT_TRUE(split_nodes[1]->id.contains("#type:"));

    auto const sum = std::ranges::find_if(split_nodes, [](auto const* node) {
        return node->kind.contains("Sum<");
    });
    auto const difference = std::ranges::find_if(split_nodes, [](auto const* node) {
        return node->kind.contains("BinaryOpNode");
    });
    ASSERT_NE(sum, split_nodes.end());
    ASSERT_NE(difference, split_nodes.end());
    ASSERT_EQ((*sum)->members.size(), 2u);
    EXPECT_EQ((*sum)->members[0].ordinal, 0u);
    EXPECT_EQ((*sum)->members[1].ordinal, 1u);
    ASSERT_EQ((*difference)->members.size(), 1u);
    EXPECT_EQ((*difference)->members[0].ordinal, 0u);

    app.set_sample_input_value((*sum)->id, 0, 0.25f);
    auto const updated_sum = app.get_virtual_node((*sum)->id);
    auto const untouched_difference = app.get_virtual_node((*difference)->id);
    EXPECT_FLOAT_EQ(static_cast<float>(updated_sum.sample_inputs[0].current_value), 0.25f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(untouched_difference.sample_inputs[0].current_value),
        0.0f
    );
}

TEST(IvModuleSourceIntrospection, QueryBySpansAggregatesMixedConnectivity)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_mixed_connectivity",
        R"(#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/arithmetic.h>

namespace {
    template<int I>
    consteval iv::NodeRef make_sum(iv::GraphBuilder& g)
    {
        (void)I;
        return g.node<iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>>().node_ref();
    }

    consteval void mixed_connectivity_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const value = g.node<iv::Constant>(0.0f).node_ref();
        auto const a = make_sum<0>(g);
        auto const b = make_sum<1>(g);
        a(value);
        auto const sink = a + b;
        g.outputs("main"_P = sink);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 24, .column = 1}}});

    size_t connected_sum_count = 0;
    size_t disconnected_sum_count = 0;
    for (auto const &node : result.nodes) {
        if (!node.kind.contains("Sum<") || node.sample_inputs.size() != 1) continue;
        EXPECT_EQ(node.member_count, 1u);
        if (node.sample_inputs.front().connectivity == iv::VirtualPortConnectivity::connected) {
            ++connected_sum_count;
        } else if (node.sample_inputs.front().connectivity == iv::VirtualPortConnectivity::disconnected) {
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

    auto const tone_range = iv::SourceRange{.start = {.line = 8, .column = 20}, .end = {.line = 8, .column = 20}};
    auto const frequency_range = iv::SourceRange{.start = {.line = 11, .column = 24}, .end = {.line = 11, .column = 24}};

    auto const tone_only = app.query_by_spans(module_cpp, {tone_range}, iv::SourceRangeMatchMode::intersection);
    auto const frequency_only = app.query_by_spans(module_cpp, {frequency_range}, iv::SourceRangeMatchMode::intersection);
    auto const both = app.query_by_spans(
        module_cpp, {tone_range, frequency_range}, iv::SourceRangeMatchMode::intersection);

    auto const ids = [](iv::ProjectQueryResult const &query) {
        std::set<std::string> node_ids;
        for (auto const &node : query.nodes) node_ids.insert(node.id);
        return node_ids;
    };

    auto const tone_ids = ids(tone_only);
    auto const frequency_ids = ids(frequency_only);
    auto const both_ids = ids(both);

    std::set<std::string> intersection;
    std::set_intersection(
        tone_ids.begin(), tone_ids.end(),
        frequency_ids.begin(), frequency_ids.end(),
        std::inserter(intersection, intersection.end()));
    EXPECT_EQ(both_ids, intersection);
}

TEST(IvModuleSourceIntrospection, QueryBySpansUnionsMultipleSelections)
{
    auto const workspace = read_only_module_fixture_workspace("local_cmake");
    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const tone_range = iv::SourceRange{.start = {.line = 8, .column = 20}, .end = {.line = 8, .column = 20}};
    auto const frequency_range = iv::SourceRange{.start = {.line = 11, .column = 24}, .end = {.line = 11, .column = 24}};

    auto const tone_only = app.query_by_spans(module_cpp, {tone_range}, iv::SourceRangeMatchMode::intersection);
    auto const frequency_only = app.query_by_spans(module_cpp, {frequency_range}, iv::SourceRangeMatchMode::intersection);
    auto const both = app.query_by_spans(
        module_cpp, {tone_range, frequency_range}, iv::SourceRangeMatchMode::union_);

    auto const ids = [](iv::ProjectQueryResult const &query) {
        std::set<std::string> node_ids;
        for (auto const &node : query.nodes) node_ids.insert(node.id);
        return node_ids;
    };

    auto const tone_ids = ids(tone_only);
    auto const frequency_ids = ids(frequency_only);
    auto const both_ids = ids(both);

    std::set<std::string> expected_union = tone_ids;
    expected_union.insert(frequency_ids.begin(), frequency_ids.end());
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
        for (auto const &span : node.source_spans) expected_spans.insert(span_key(span));
    }

    std::set<std::string> actual_spans;
    for (auto const &span : active_regions.source_spans) actual_spans.insert(span_key(span));
    EXPECT_EQ(actual_spans, expected_spans);
}

TEST(IvModuleSourceIntrospection, QueryBySpansMergesPolyphonicCallbackNodesByExactSourceSpan)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_polyphonic_exact_spans",
        R"(#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/shaping.h>

consteval void polyphonic_module(iv::GraphBuilder& g)
{
    using namespace iv;



    iv::polyphonic<2>(g, [&]<size_t Voice>(auto m) {
        auto const saw = g.node<SawOscillator>();
        saw(
            "phase_offset"_P = 0.0,
            "frequency"_P = 440.0
        );
        (void)Voice;
        g.outputs("main"_P = saw * m["amplitude"_P]);
    });
}
)");

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 11, .column = 20}, .end = {.line = 11, .column = 20}}});

    ASSERT_EQ(result.nodes.size(), 1u);
    auto const& virtual_node = result.nodes.front();
    EXPECT_EQ(virtual_node.kind, "iv::SawOscillator");
    EXPECT_EQ(virtual_node.member_count, 2u);
    ASSERT_EQ(virtual_node.members.size(), 2u);
    EXPECT_EQ(virtual_node.members[0].ordinal, 0u);
    EXPECT_EQ(virtual_node.members[1].ordinal, 1u);
    EXPECT_EQ(virtual_node.members[0].kind, "iv::SawOscillator");
    EXPECT_EQ(virtual_node.members[1].kind, "iv::SawOscillator");
    ASSERT_EQ(virtual_node.sample_inputs.size(), 2u);
    EXPECT_EQ(virtual_node.sample_inputs[0].name, "phase_offset");
    EXPECT_EQ(virtual_node.sample_inputs[1].name, "frequency");
    ASSERT_EQ(virtual_node.sample_outputs.size(), 1u);
    EXPECT_EQ(virtual_node.sample_outputs[0].name, "out");

    auto const resolved = app.get_virtual_node(virtual_node.id);
    EXPECT_EQ(resolved.kind, "iv::SawOscillator");
    EXPECT_EQ(resolved.member_count, 2u);
    ASSERT_EQ(resolved.sample_inputs.size(), 2u);
    EXPECT_EQ(resolved.sample_inputs[0].name, "phase_offset");
    EXPECT_EQ(resolved.sample_inputs[1].name, "frequency");
    ASSERT_EQ(resolved.sample_outputs.size(), 1u);
    EXPECT_EQ(resolved.sample_outputs[0].name, "out");

    app.set_sample_input_value(virtual_node.id, 1, 0.25f);
    auto const virtual_override = app.get_virtual_node(virtual_node.id);
    ASSERT_EQ(virtual_override.members.size(), 2u);
    EXPECT_FLOAT_EQ(static_cast<float>(virtual_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(virtual_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(virtual_override.members[1].sample_inputs[1].current_value), 0.25f);
    EXPECT_FALSE(virtual_override.members[0].sample_inputs[1].has_concrete_override);
    EXPECT_FALSE(virtual_override.members[1].sample_inputs[1].has_concrete_override);

    app.set_sample_input_value(virtual_node.id, 1, 0.75f, 1u);
    auto const concrete_override = app.get_virtual_node(virtual_node.id);
    ASSERT_EQ(concrete_override.members.size(), 2u);
    EXPECT_FLOAT_EQ(static_cast<float>(concrete_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(concrete_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(concrete_override.members[1].sample_inputs[1].current_value), 0.75f);
    EXPECT_FALSE(concrete_override.members[0].sample_inputs[1].has_concrete_override);
    EXPECT_TRUE(concrete_override.members[1].sample_inputs[1].has_concrete_override);

    app.set_sample_input_state(
        virtual_node.id,
        1,
        iv::ProjectSampleInputState::virtual_follow,
        1u);
    auto const cleared_override = app.get_virtual_node(virtual_node.id);
    ASSERT_EQ(cleared_override.members.size(), 2u);
    EXPECT_FLOAT_EQ(static_cast<float>(cleared_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(cleared_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(cleared_override.members[1].sample_inputs[1].current_value), 0.25f);
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
        R"(#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/shaping.h>

consteval void polyphonic_module(iv::GraphBuilder& g)
{
    using namespace iv;



    iv::polyphonic<2>(g, [&]<size_t Voice>(auto m) {
        auto const saw = g.node<SawOscillator>();
        saw(
            "phase_offset"_P = 0.0,
            "frequency"_P = 440.0
        );
        (void)Voice;
        g.outputs("main"_P = saw * m["amplitude"_P]);
    });
}
)");

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 11, .column = 20}, .end = {.line = 11, .column = 20}}});

    ASSERT_EQ(result.nodes.size(), 1u);
    EXPECT_EQ(result.nodes.front().kind, "iv::SawOscillator");
    EXPECT_TRUE(std::ranges::none_of(result.nodes, [](auto const &node) {
        return node.kind == "Polyphonic";
    }));
    EXPECT_TRUE(std::ranges::none_of(result.nodes, [](auto const &node) {
        return node.kind == "PolyphonicVoice";
    }));
}

TEST(IvModuleSourceIntrospection, ReloadKeepsVirtualNodeIdsAddressable)
{
    auto const workspace = mutable_module_fixture_workspace("iv_module_source_introspection_reload_epoch", "local_cmake");
    auto const module_cpp = workspace / "module.cpp";
    iv::test_support::write_text(workspace / "iv_project.jsonl", "");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const initial = app.query_by_spans(
        std::filesystem::weakly_canonical(module_cpp),
        {{.start = {.line = 7, .column = 1}, .end = {.line = 18, .column = 1}}});
    ASSERT_FALSE(initial.nodes.empty());

    auto const original_text = iv::test::read_text(module_cpp);
    iv::test::write_text(module_cpp, original_text + "\n");
    std::this_thread::sleep_for(1s);
    auto const reloaded = app.query_by_spans(
        std::filesystem::weakly_canonical(module_cpp),
        {{.start = {.line = 7, .column = 1}, .end = {.line = 19, .column = 1}}});
    iv::test::write_text(module_cpp, original_text);

    EXPECT_FALSE(reloaded.nodes.empty());
    EXPECT_NO_THROW((void)app.get_virtual_node(initial.nodes.front().id));
}
