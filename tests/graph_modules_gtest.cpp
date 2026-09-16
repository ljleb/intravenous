#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/debug_probe.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/dsl.h>
#include <configured_graph_test_view.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/compiler.h>
#include <intravenous/module/configured_graph_wire.h>
#include <intravenous/module/builder_session.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace iv {
namespace {

struct CStringConfigNode {
    char const* title = "title";
    char const* detail = "detail";
    char const* optional = nullptr;

    static constexpr auto outputs()
    {
        return std::array<OutputConfig, 1>{};
    }

    void tick(TickSampleContext<CStringConfigNode> const& ctx) const
    {
        ctx.outputs[0].push(0.0f);
    }
};

struct NodeCallEventSource {
    static constexpr auto outputs()
    {
        return std::array<OutputConfig, 1>{
            realtime_event_output("trigger", EventTypeId::trigger),
        };
    }

    void tick_block(TickBlockContext<NodeCallEventSource> const&) const {}
};

struct NodeCallMixedSink {
    static constexpr auto inputs()
    {
        return std::array<InputConfig, 3>{
            realtime_sample_input("left"),
            realtime_sample_input("right"),
            realtime_event_input("trigger", EventTypeId::trigger),
        };
    }

    void tick_block(TickBlockContext<NodeCallMixedSink> const&) const {}
};

static_assert(details::fixed_input_count_v<NodeCallMixedSink> == 2);
static_assert(details::fixed_event_input_count_v<NodeCallMixedSink> == 1);
static_assert(details::fixed_input_count_v<Constant> == 0);
static_assert(details::fixed_event_input_count_v<Constant> == 0);

struct CStringConfigDetails {
    char const* first = "first";
    char const* second = "second";
};

struct StructuredCStringConfigNode {
    char const* labels[2] = {"left", "right"};
    CStringConfigDetails details{};

    static constexpr auto outputs()
    {
        return std::array<OutputConfig, 1>{};
    }

    void tick(TickSampleContext<StructuredCStringConfigNode> const& ctx) const
    {
        ctx.outputs[0].push(0.0f);
    }
};

constexpr char cstring_title[] = "test probe";
constexpr char cstring_detail[] = "second string";
constexpr char cstring_left[] = "left label";
constexpr char cstring_right[] = "right label";
constexpr char cstring_first[] = "first detail";
constexpr char cstring_second[] = "second detail";

void configure_pointer_metadata_package(
    details::BuilderSession* session,
    std::span<NodeConfigPointerFieldData const> pointer_fields,
    std::span<RetainedGlobalData const> retained_globals)
{
    std::array packages{details::BuilderPackageView{
        .package_root = "test.pointer-metadata-package",
        .config_pointer_fields = pointer_fields,
        .retained_globals = retained_globals,
    }};
    details::set_builder_packages(session, packages);
    details::select_builder_package(session, 0);
}

iv::RuntimeGraphPlan compile_graph(
    iv::ConfiguredGraphTestView view,
    bool execution_root = false)
{
    auto configured = iv::thaw_configured_graph_for_test(view);
    auto executable = iv::GraphLowerer::lower(
        std::move(configured), {.execution_root = execution_root});
    return iv::GraphCompiler::compile(std::move(executable));
}

ConfiguredGraphTestView configure_functional_subgraph()
{
    GraphBuilder g;
    auto nested = g.subgraph([&](SubgraphBuilder& boundary) {
        auto input = boundary.input<"in">(0.0f);
        auto pass = details::configure_concrete_node<
            Sum<mono, SampleStreamLayout::planar, 1>>(g);
        pass(input);
        boundary.outputs("out"_P = pass);
    });

    nested("in"_P = 0.25f);
    g.outputs("main"_P = nested["out"]);
    return freeze_configured_graph_for_test(std::move(g).finish());
}

bool functional_subgraph_compiles()
{
    (void)compile_graph(configure_functional_subgraph());
    return true;
}

ConfiguredGraphTestView configure_direct_public_sample_passthrough()
{
    GraphBuilder g;
    auto input = g.input<"in">(0.0f);
    g.outputs("out"_P = input);
    return freeze_configured_graph_for_test(std::move(g).finish());
}

bool direct_public_sample_passthrough_compiles()
{
    auto const built = compile_graph(configure_direct_public_sample_passthrough());
    return built.graph.inputs().size() == 1
        && built.graph.outputs().size() == 1;
}

struct IntrospectionRegressionConfiguration {
    ConfiguredGraphTestView view;
};

IntrospectionRegressionConfiguration configure_introspection_regression()
{
    GraphBuilder g;
    auto input = g.input<"in">(0.25f);
    auto event = g.event_input<"event">(EventTypeId::empty);
    auto sum = details::configure_concrete_node<Sum<mono, SampleStreamLayout::planar, 1>>(g);
    auto annotated = _annotate_node_source_info(sum.node_ref(), "sum");
    sum(input);
    g.outputs("out"_P = sum);
    (void)annotated;
    (void)event;
    return {.view = freeze_configured_graph_for_test(std::move(g).finish())};
}

struct IntrospectionRegressionSnapshot {
    bool virtual_node_is_preserved = false;
    bool sample_ports_are_preserved = false;
    bool event_ports_are_preserved = false;
    bool shared_lowering_matches_canonical_metadata = false;
};

IntrospectionRegressionSnapshot introspection_regression_snapshot()
{
    auto const configured = configure_introspection_regression();
    auto const compiled = compile_graph(configured.view, true);
    auto const& metadata = compiled.introspection;
    auto const& execution = compiled.introspection;
    IntrospectionRegressionSnapshot result;

    result.virtual_node_is_preserved = metadata.virtual_nodes.size() == 1
        && metadata.virtual_nodes.front().source_identity == "sum"
        && metadata.virtual_nodes.front().sample_inputs.size() == 1
        && metadata.virtual_nodes.front().sample_outputs.size() == 1;

    result.sample_ports_are_preserved = metadata.public_sample_inputs.size() == 1
        && metadata.public_sample_outputs.size() == 1
        && metadata.public_sample_inputs.front().family_name == "in"
        && metadata.public_sample_inputs.front().configured_connected
        && metadata.public_sample_outputs.front().family_name == "out";

    result.event_ports_are_preserved = metadata.public_event_inputs.size() == 1
        && metadata.public_event_inputs.front().port_ordinal == 0
        && !metadata.public_event_inputs.front().graph_connected
        && metadata.public_event_outputs.empty();
    result.shared_lowering_matches_canonical_metadata = metadata.virtual_nodes.size()
            == execution.virtual_nodes.size()
        && metadata.public_sample_inputs.size()
            == execution.public_sample_inputs.size()
        && metadata.public_event_inputs.size()
            == execution.public_event_inputs.size()
        && metadata.public_sample_outputs.size()
            == execution.public_sample_outputs.size()
        && metadata.public_event_outputs.size()
            == execution.public_event_outputs.size()
        && metadata.virtual_nodes.front().id == execution.virtual_nodes.front().id
        && metadata.virtual_nodes.front().source_identity
            == execution.virtual_nodes.front().source_identity
        && metadata.public_sample_inputs.front().configured_connected
            == execution.public_sample_inputs.front().configured_connected
        && metadata.public_event_inputs.front().graph_connected
            == execution.public_event_inputs.front().graph_connected;
    return result;
}

} // namespace

TEST(GraphModules, BuilderSessionOwnsStateRatherThanAGraphBuilderObject)
{
    auto session = std::unique_ptr<
        details::BuilderSession,
        decltype(&details::iv_builder_session_destroy)>(
            details::iv_builder_session_create(),
            details::iv_builder_session_destroy);
    ASSERT_NE(session, nullptr);

    GraphBuilder builder(session.get());
    auto input = builder.input<"in">(0.0f);
    auto pass = details::configure_concrete_node<
        Sum<mono, SampleStreamLayout::planar, 1>>(builder);
    pass(input);
    builder.outputs("out"_P = pass);

    auto view = freeze_configured_graph_for_test(
        details::take_built_graph(session.get()));
    EXPECT_THROW(
        (void)details::take_built_graph(session.get()),
        std::logic_error);
    session.reset();

    auto const plan = compile_graph(view);
    ASSERT_EQ(plan.graph.outputs().size(), 1u);
    EXPECT_EQ(plan.graph.outputs().front().name, "out");
}

TEST(GraphModules, TypedNodeCallsForwardNormalizedSampleAndEventRequests)
{
    GraphBuilder graph;
    auto left = graph.input<"left">(0.0f);
    auto right = graph.input<"right">(0.0f);
    auto source = details::configure_concrete_node<NodeCallEventSource>(graph);
    auto sink = details::configure_concrete_node<NodeCallMixedSink>(graph);
    auto tiled_sink = details::configure_concrete_tiled_node<NodeCallMixedSink, stereo>(graph);

    sink(
        "left"_P = left,
        "right"_P = right,
        "trigger"_F = source);
    tiled_sink(
        "right"_P = right,
        "left"_P = left,
        "trigger"_F = source.event_port());

    EXPECT_TRUE(sink.input_is_connected(0));
    EXPECT_TRUE(sink.input_is_connected(1));
    EXPECT_TRUE(sink.event_input_is_connected(0));
    EXPECT_TRUE(tiled_sink.input_is_connected(0));
    EXPECT_TRUE(tiled_sink.input_is_connected(1));
    EXPECT_TRUE(tiled_sink.event_input_is_connected(0));
}

TEST(GraphModules, BuilderCapturesPointerConfigurationAsSymbolicRelocations)
{
    auto session = std::unique_ptr<
        details::BuilderSession,
        decltype(&details::iv_builder_session_destroy)>(
            details::iv_builder_session_create(),
            details::iv_builder_session_destroy);
    std::array fields{
        NodeConfigPointerFieldData{
            .code_key = details::node_code_key_v<CStringConfigNode>,
            .byte_offset = offsetof(CStringConfigNode, title)},
        NodeConfigPointerFieldData{
            .code_key = details::node_code_key_v<CStringConfigNode>,
            .byte_offset = offsetof(CStringConfigNode, detail)},
        NodeConfigPointerFieldData{
            .code_key = details::node_code_key_v<CStringConfigNode>,
            .byte_offset = offsetof(CStringConfigNode, optional)},
    };
    std::array globals{
        RetainedGlobalData{
            .address = cstring_title,
            .size = sizeof(cstring_title),
            .ordinal = 0,
        },
        RetainedGlobalData{
            .address = cstring_detail,
            .size = sizeof(cstring_detail),
            .ordinal = 1,
        },
    };
    configure_pointer_metadata_package(session.get(), fields, globals);

    GraphBuilder builder(session.get());
    auto probe = details::configure_concrete_node<CStringConfigNode>(builder, CStringConfigNode{
        .title = cstring_title,
        .detail = cstring_detail,
        .optional = nullptr,
    });
    builder.outputs(probe);

    auto archive = serialize_configured_graph(
        details::take_built_graph(session.get()));
    ASSERT_EQ(archive.node_configs.size(), 1u);
    auto const& relocations = archive.node_configs.front().relocations;
    ASSERT_EQ(relocations.size(), 3u);
    EXPECT_EQ(relocations[0].byte_offset, offsetof(CStringConfigNode, title));
    EXPECT_EQ(relocations[0].package_root, "test.pointer-metadata-package");
    ASSERT_TRUE(relocations[0].retained_global_ordinal.has_value());
    EXPECT_EQ(*relocations[0].retained_global_ordinal, 0u);
    EXPECT_EQ(relocations[0].addend, 0u);
    EXPECT_EQ(relocations[1].byte_offset, offsetof(CStringConfigNode, detail));
    EXPECT_EQ(relocations[1].package_root, "test.pointer-metadata-package");
    ASSERT_TRUE(relocations[1].retained_global_ordinal.has_value());
    EXPECT_EQ(*relocations[1].retained_global_ordinal, 1u);
    EXPECT_EQ(relocations[1].addend, 0u);
    EXPECT_EQ(relocations[2].byte_offset, offsetof(CStringConfigNode, optional));
    EXPECT_TRUE(relocations[2].package_root.empty());
    EXPECT_FALSE(relocations[2].retained_global_ordinal.has_value());
}

TEST(GraphModules, BuilderRelocatesProviderNodePointerToCallerPackageGlobal)
{
    auto session = std::unique_ptr<
        details::BuilderSession,
        decltype(&details::iv_builder_session_destroy)>(
            details::iv_builder_session_create(),
            details::iv_builder_session_destroy);
    std::array provider_fields{
        NodeConfigPointerFieldData{
            .code_key = details::node_code_key_v<CStringConfigNode>,
            .byte_offset = offsetof(CStringConfigNode, title)},
    };
    std::array caller_globals{
        RetainedGlobalData{
            .address = cstring_title,
            .size = sizeof(cstring_title),
            .ordinal = 7,
        },
    };
    std::array packages{
        details::BuilderPackageView{
            .package_root = "test.provider-package",
            .config_pointer_fields = provider_fields,
        },
        details::BuilderPackageView{
            .package_root = "test.caller-package",
            .retained_globals = caller_globals,
        },
    };
    details::set_builder_packages(session.get(), packages);
    details::select_builder_package(session.get(), 0);

    GraphBuilder builder(session.get());
    auto node = details::configure_concrete_node<CStringConfigNode>(builder,
        CStringConfigNode{.title = cstring_title, .detail = nullptr, .optional = nullptr});
    builder.outputs(node);

    auto archive = serialize_configured_graph(
        details::take_built_graph(session.get()));
    ASSERT_EQ(archive.node_configs.size(), 1u);
    auto const& relocations = archive.node_configs.front().relocations;
    ASSERT_EQ(relocations.size(), 1u);
    EXPECT_EQ(relocations.front().package_root, "test.caller-package");
    ASSERT_TRUE(relocations.front().retained_global_ordinal.has_value());
    EXPECT_EQ(*relocations.front().retained_global_ordinal, 7u);

    auto const used_packages = details::builder_used_packages(session.get());
    EXPECT_EQ(used_packages, (std::vector<std::size_t>{0, 1}));
}

TEST(GraphModules, BuilderCapturesNestedAndArrayPointerConfiguration)
{
    auto session = std::unique_ptr<
        details::BuilderSession,
        decltype(&details::iv_builder_session_destroy)>(
            details::iv_builder_session_create(),
            details::iv_builder_session_destroy);
    std::array fields{
        NodeConfigPointerFieldData{
            .code_key = details::node_code_key_v<StructuredCStringConfigNode>,
            .byte_offset = offsetof(StructuredCStringConfigNode, labels)},
        NodeConfigPointerFieldData{
            .code_key = details::node_code_key_v<StructuredCStringConfigNode>,
            .byte_offset = offsetof(StructuredCStringConfigNode, labels)
                + sizeof(char const*)},
        NodeConfigPointerFieldData{
            .code_key = details::node_code_key_v<StructuredCStringConfigNode>,
            .byte_offset = offsetof(StructuredCStringConfigNode, details)
                + offsetof(CStringConfigDetails, first)},
        NodeConfigPointerFieldData{
            .code_key = details::node_code_key_v<StructuredCStringConfigNode>,
            .byte_offset = offsetof(StructuredCStringConfigNode, details)
                + offsetof(CStringConfigDetails, second)},
    };
    std::array globals{
        RetainedGlobalData{
            .address = cstring_left,
            .size = sizeof(cstring_left),
            .ordinal = 0,
        },
        RetainedGlobalData{
            .address = cstring_right,
            .size = sizeof(cstring_right),
            .ordinal = 1,
        },
        RetainedGlobalData{
            .address = cstring_first,
            .size = sizeof(cstring_first),
            .ordinal = 2,
        },
        RetainedGlobalData{
            .address = cstring_second,
            .size = sizeof(cstring_second),
            .ordinal = 3,
        },
    };
    configure_pointer_metadata_package(session.get(), fields, globals);

    GraphBuilder builder(session.get());
    auto node = details::configure_concrete_node<StructuredCStringConfigNode>(builder,
        StructuredCStringConfigNode{
            .labels = {cstring_left, cstring_right},
            .details = {cstring_first, cstring_second},
        });
    builder.outputs(node);

    auto archive = serialize_configured_graph(
        details::take_built_graph(session.get()));
    ASSERT_EQ(archive.node_configs.size(), 1u);
    auto const& relocations = archive.node_configs.front().relocations;
    ASSERT_EQ(relocations.size(), fields.size());
    for (size_t index = 0; index < fields.size(); ++index) {
        EXPECT_EQ(relocations[index].byte_offset, fields[index].byte_offset);
        EXPECT_EQ(relocations[index].package_root, "test.pointer-metadata-package");
        ASSERT_TRUE(relocations[index].retained_global_ordinal.has_value());
        EXPECT_EQ(*relocations[index].retained_global_ordinal, index);
        EXPECT_EQ(relocations[index].addend, 0u);
    }
}

TEST(GraphModules, BuilderRejectsInvalidPointerMetadataPackages)
{
    auto session = std::unique_ptr<
        details::BuilderSession,
        decltype(&details::iv_builder_session_destroy)>(
            details::iv_builder_session_create(),
            details::iv_builder_session_destroy);
    std::array empty_root{details::BuilderPackageView{}};
    EXPECT_THROW(
        details::set_builder_packages(session.get(), empty_root),
        std::invalid_argument);

    std::array invalid_globals{RetainedGlobalData{
        .address = nullptr,
        .size = 1,
        .ordinal = 0,
    }};
    std::array invalid_global_package{details::BuilderPackageView{
        .package_root = "test.invalid-pointer-metadata",
        .retained_globals = invalid_globals,
    }};
    EXPECT_THROW(
        details::set_builder_packages(session.get(), invalid_global_package),
        std::invalid_argument);
}

TEST(GraphModules, OutputRequestCopiesBorrowedStringsIntoTheSession)
{
    GraphBuilder builder;
    auto source = details::configure_concrete_node<Constant>(builder, Sample{0.25f});
    std::string name = "main";
    auto const request = SampleOutputRequest{
        .ref = static_cast<SamplePortRef>(source),
        .name = name,
        .channel_layout = {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        },
        .family_name = name,
        .family_channel_type = ChannelTypeId::mono,
    };
    builder.outputs(std::span<SampleOutputRequest const>(&request, 1));
    name[0] = 'x';

    auto plan = compile_graph(
        freeze_configured_graph_for_test(std::move(builder).finish()));
    ASSERT_EQ(plan.graph.outputs().size(), 1u);
    EXPECT_EQ(plan.graph.outputs().front().name, "main");
}

TEST(GraphModules, PublicInputCanFeedPublicOutputWithoutAnInternalNode)
{
    EXPECT_TRUE(direct_public_sample_passthrough_compiles());
}

TEST(GraphModules, RuntimeIntrospectionPreservesVirtualAndPublicPorts)
{
    auto snapshot = introspection_regression_snapshot();
    EXPECT_TRUE(snapshot.virtual_node_is_preserved);
    EXPECT_TRUE(snapshot.sample_ports_are_preserved);
    EXPECT_TRUE(snapshot.event_ports_are_preserved);
    EXPECT_TRUE(snapshot.shared_lowering_matches_canonical_metadata);
}

TEST(GraphModules, FunctionalSubgraphRemainsAnExplicitBoundaryFacade)
{
    EXPECT_TRUE(functional_subgraph_compiles());

    auto const built = compile_graph(configure_functional_subgraph());
    ASSERT_EQ(built.metadata.lowered_subgraphs.size(), 1u);
    auto const& scope = built.metadata.lowered_subgraphs.front();
    EXPECT_EQ(scope.parent_scope, GRAPH_ID);
    ASSERT_EQ(scope.sample_inputs.size(), 1u);
    EXPECT_EQ(scope.sample_inputs.front().name, "in");
    ASSERT_EQ(scope.sample_outputs.size(), 1u);
    EXPECT_EQ(scope.sample_outputs.front().name, "out");
    ASSERT_EQ(scope.sample_output_sources.size(), 1u);
    EXPECT_TRUE(std::ranges::contains(
        scope.member_nodes, scope.sample_output_sources.front().node));
}

TEST(GraphModules, EventOnlyFunctionalSubgraphDoesNotRequireSampleOutputs)
{
    GraphBuilder g;
    auto const source = g.event_input<"event">(EventTypeId::empty);
    auto const scope = g.subgraph([&](SubgraphBuilder& boundary) {
        auto const input = boundary.event_input<"event">(EventTypeId::empty);
        auto const relay = details::configure_concrete_node<EventConcatenation>(
            g, 1, EventTypeId::empty);
        relay.connect_event_input(0, input);
        g.event_outputs("event"_P = relay.event_port());
    });
    scope.connect_event_input("event", source);
    g.outputs();

    auto const built = compile_graph(
        freeze_configured_graph_for_test(std::move(g).finish()));
    ASSERT_EQ(built.metadata.lowered_subgraphs.size(), 1u);
    auto const& lowered_scope = built.metadata.lowered_subgraphs.front();
    EXPECT_TRUE(lowered_scope.sample_outputs.empty());
    ASSERT_EQ(lowered_scope.event_inputs.size(), 1u);
    EXPECT_EQ(lowered_scope.event_inputs.front().name, "event");
    ASSERT_EQ(built.graph.outputs().size(), 1u);
    EXPECT_FALSE(is_sample(built.graph.outputs().front()));
    EXPECT_EQ(built.graph.outputs().front().name, "event");
}

} // namespace iv
