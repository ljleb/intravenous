#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>

#include <gtest/gtest.h>

namespace iv {
namespace {

void pass_module(GraphBuilder& g)
{
    auto input = g.input<"in">(0.0f);
    auto pass = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
    pass(input);
    g.outputs("out"_P = pass);
}

void nested_module(GraphBuilder& g)
{
    auto input = g.input<"in">(0.0f);
    auto child = g.module<&pass_module>();
    child("in"_P = input);
    g.outputs("out"_P = child);
}

void tiled_module(GraphBuilder& g)
{
    auto input = g.input<"in">(0.0f);
    auto tiled = g.node<Sum<mono, SampleStreamLayout::planar, 1>, stereo>();
    tiled(input);
    g.outputs("out"_P = tiled);
}

static_assert(std::invocable<decltype(&pass_module), GraphBuilder&>);
static_assert(std::same_as<std::invoke_result_t<decltype(&pass_module), GraphBuilder&>, void>);

} // namespace

TEST(GraphModules, ModuleFunctionUsesTheRootGraphBuilderSignature)
{
    GraphBuilder root;
    pass_module(root);
    auto const root_built = root.build_root_node();
    ASSERT_EQ(root_built.graph.outputs().size(), 1u);
    EXPECT_EQ(root_built.graph.outputs().front().name, "out");

    GraphBuilder parent;
    auto child = parent.module<&pass_module>();
    EXPECT_EQ(child.sample_input_count(), 1u);
    EXPECT_EQ(child.sample_output_count(), 1u);

    child("in"_P = 0.25f);
    parent.outputs("main"_P = child["out"]);

    auto const nested_built = parent.build_root_node();
    ASSERT_EQ(nested_built.graph.outputs().size(), 1u);
    EXPECT_EQ(nested_built.graph.outputs().front().name, "main");
}

TEST(GraphModules, ModulesComposeRecursivelyThroughAuthoredGraphSplicing)
{
    GraphBuilder g;
    auto child = g.module<&nested_module>();
    child("in"_P = 0.5f);
    g.outputs("main"_P = child["out"]);

    EXPECT_NO_THROW((void)g.build_root_node());
}

TEST(GraphModules, FirstClassTiledNodeBundlesSurviveModuleSplicing)
{
    GraphBuilder g;
    auto child = g.module<&tiled_module>();

    EXPECT_EQ(child.sample_input_count(), 1u);
    EXPECT_EQ(child.sample_output_count(), 1u);

    child("in"_P = 0.5f);
    auto output = child["out"];
    EXPECT_EQ(output.channel_type, ChannelTypeId::stereo);
    EXPECT_EQ(output.channels.size(), 2u);
    g.outputs("main"_P = output);

    auto const built = g.build_root_node();
    ASSERT_EQ(built.graph.outputs().size(), 1u);
    EXPECT_EQ(built.graph.outputs().front().channel_layout.channel_type,
              ChannelTypeId::stereo);
}

TEST(GraphModules, FunctionalSubgraphRemainsAnExplicitBoundaryFacade)
{
    GraphBuilder g;
    auto nested = g.subgraph([&](SubgraphBuilder& boundary) {
        auto input = boundary.input<"in">(0.0f);
        auto pass = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
        pass(input);
        boundary.outputs("out"_P = pass);
    });

    nested("in"_P = 0.25f);
    g.outputs("main"_P = nested["out"]);

    EXPECT_NO_THROW((void)g.build_root_node());
}

} // namespace iv
