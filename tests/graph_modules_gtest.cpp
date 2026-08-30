#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <ranges>

namespace iv {
namespace {

consteval void pass_module(GraphBuilder& g)
{
    auto input = g.input<"in">(0.0f);
    auto pass = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
    pass(input);
    g.outputs("out"_P = pass);
}

consteval void nested_module(GraphBuilder& g)
{
    auto input = g.input<"in">(0.0f);
    auto child = g.module<pass_module>();
    child("in"_P = input);
    g.outputs("out"_P = child);
}

consteval void tiled_module(GraphBuilder& g)
{
    auto input = g.input<"in">(0.0f);
    auto tiled = g.node<Sum<mono, SampleStreamLayout::planar, 1>, stereo>();
    tiled(input);
    g.outputs("out"_P = tiled);
}

consteval void event_module(GraphBuilder& g)
{
    auto input = g.event_input<"event">(EventTypeId::empty);
    auto relay = g.node<EventConcatenation>(1, EventTypeId::empty);
    relay.connect_event_input(0, input);
    g.event_outputs("event"_P = relay.event_port());
    g.outputs();
}

static_assert(std::invocable<decltype(&pass_module), GraphBuilder&>);
static_assert(std::same_as<std::invoke_result_t<decltype(&pass_module), GraphBuilder&>, void>);

struct RootSignatureSnapshot {
    bool root_output_named_out = false;
    size_t child_sample_inputs = 0;
    size_t child_sample_outputs = 0;
    bool nested_output_named_main = false;
};

consteval RootSignatureSnapshot root_signature_snapshot()
{
    RootSignatureSnapshot result;
    {
        GraphBuilder root;
        pass_module(root);
        auto const built = std::move(root).build();
        result.root_output_named_out =
            built.graph.outputs().size() == 1
            && built.graph.outputs().front().name == "out";
    }
    {
        GraphBuilder parent;
        auto child = parent.module<pass_module>();
        result.child_sample_inputs = child.sample_input_count();
        result.child_sample_outputs = child.sample_output_count();
        child("in"_P = 0.25f);
        parent.outputs("main"_P = child["out"]);
        auto const built = std::move(parent).build();
        result.nested_output_named_main =
            built.graph.outputs().size() == 1
            && built.graph.outputs().front().name == "main";
    }
    return result;
}

struct RecursiveModuleSnapshot {
    size_t lowered_subgraph_count = 0;
    size_t nested_scope_count = 0;
    bool parent_scopes_valid = false;
};

consteval RecursiveModuleSnapshot recursive_module_snapshot()
{
    GraphBuilder g;
    auto child = g.module<nested_module>();
    child("in"_P = 0.5f);
    g.outputs("main"_P = child["out"]);

    auto const built = std::move(g).build();
    RecursiveModuleSnapshot result{
        .lowered_subgraph_count = built.metadata.lowered_subgraphs.size(),
        .parent_scopes_valid = true,
    };
    for (auto const& scope : built.metadata.lowered_subgraphs) {
        if (scope.parent_scope == GRAPH_ID)
            continue;
        ++result.nested_scope_count;
        result.parent_scopes_valid = result.parent_scopes_valid
            && scope.parent_scope < built.metadata.lowered_subgraphs.size();
    }
    return result;
}

struct AnnotatedModuleSnapshot {
    size_t matching_nodes = 0;
    size_t virtual_nodes = 0;
    bool id_has_expected_prefix = false;
};

consteval AnnotatedModuleSnapshot annotated_module_snapshot()
{
    GraphBuilder g;
    auto child = _annotate_node_source_info(
        g.module<pass_module>(),
        "module-call");
    child("in"_P = 0.5f);
    g.outputs("main"_P = child["out"]);

    auto const metadata = std::move(g).build().introspection;
    auto const matching_nodes = std::ranges::count_if(
        metadata.virtual_nodes,
        [](auto const& node) {
            return node.source_identity == "module-call";
        });
    return {
        .matching_nodes = static_cast<size_t>(matching_nodes),
        .virtual_nodes = metadata.virtual_nodes.size(),
        .id_has_expected_prefix = !metadata.virtual_nodes.empty()
            && metadata.virtual_nodes.front().id.starts_with("module-call#type:"),
    };
}

struct TiledModuleSnapshot {
    size_t child_sample_inputs = 0;
    size_t child_sample_outputs = 0;
    ChannelTypeId output_channel_type = ChannelTypeId::mono;
    size_t output_channel_count = 0;
    bool graph_output_is_stereo = false;
    size_t lowered_subgraph_count = 0;
    size_t scope_output_sources = 0;
    size_t scope_member_nodes = 0;
    bool output_source_is_member = false;
};

consteval TiledModuleSnapshot tiled_module_snapshot()
{
    GraphBuilder g;
    auto child = g.module<tiled_module>();
    TiledModuleSnapshot result;
    result.child_sample_inputs = child.sample_input_count();
    result.child_sample_outputs = child.sample_output_count();

    child("in"_P = 0.5f);
    auto output = child["out"];
    result.output_channel_type = output.channel_type;
    result.output_channel_count = output.channels.size();
    g.outputs("main"_P = output);

    auto const built = std::move(g).build();
    result.graph_output_is_stereo =
        built.graph.outputs().size() == 1
        && built.graph.outputs().front().channel_layout.channel_type
            == ChannelTypeId::stereo;
    result.lowered_subgraph_count = built.metadata.lowered_subgraphs.size();
    if (!built.metadata.lowered_subgraphs.empty()) {
        auto const& scope = built.metadata.lowered_subgraphs.front();
        result.scope_output_sources = scope.sample_output_sources.size();
        result.scope_member_nodes = scope.member_nodes.size();
        if (!scope.sample_output_sources.empty()) {
            result.output_source_is_member =
                std::ranges::find(
                    scope.member_nodes,
                    scope.sample_output_sources.front().node)
                != scope.member_nodes.end();
        }
    }
    return result;
}

consteval bool event_interfaces_compile()
{
    GraphBuilder g;
    auto child = g.module<event_module>();
    if (child.event_input_count() != 1 || child.event_output_count() != 1)
        return false;

    auto source = g.node<EventConcatenation>(0, EventTypeId::empty);
    child.connect_event_input("event", source.event_port());
    auto sink = g.node<DummyEventSink>();
    sink.connect_event_input(0, child.event_port("event"));
    g.outputs();
    (void)std::move(g).build();
    return true;
}

consteval bool functional_subgraph_compiles()
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
    (void)std::move(g).build();
    return true;
}

consteval bool direct_public_sample_passthrough_compiles()
{
    GraphBuilder g;
    auto input = g.input<"in">(0.0f);
    g.outputs("out"_P = input);
    auto const built = std::move(g).build();
    return built.graph.inputs().size() == 1
        && built.graph.outputs().size() == 1;
}

struct IntrospectionRegressionSnapshot {
    bool virtual_node_is_preserved = false;
    bool sample_ports_are_preserved = false;
    bool event_ports_are_preserved = false;
    bool shared_lowering_matches_canonical_metadata = false;
};

consteval IntrospectionRegressionSnapshot introspection_regression_snapshot()
{
    GraphBuilder g;
    auto input = g.input<"in">(0.25f);
    auto event = g.event_input<"event">(EventTypeId::empty);
    auto sum = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
    auto annotated = _annotate_node_source_info(sum.node_ref(), "sum");
    sum(input);
    g.outputs("out"_P = sum);

    auto const compiled = std::move(g).build({.execution_root = true});
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
        && metadata.public_sample_inputs.front().authored_connected
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
        && metadata.public_sample_inputs.front().authored_connected
            == execution.public_sample_inputs.front().authored_connected
        && metadata.public_event_inputs.front().graph_connected
            == execution.public_event_inputs.front().graph_connected;
    (void)annotated;
    (void)event;
    return result;
}

} // namespace

TEST(GraphModules, ModuleFunctionUsesTheRootGraphBuilderSignature)
{
    constexpr auto snapshot = root_signature_snapshot();
    EXPECT_TRUE(snapshot.root_output_named_out);
    EXPECT_EQ(snapshot.child_sample_inputs, 1u);
    EXPECT_EQ(snapshot.child_sample_outputs, 1u);
    EXPECT_TRUE(snapshot.nested_output_named_main);
}

TEST(GraphModules, PublicInputCanFeedPublicOutputWithoutAnInternalNode)
{
    static_assert(direct_public_sample_passthrough_compiles());
    EXPECT_TRUE(direct_public_sample_passthrough_compiles());
}

TEST(GraphModules, ConstevalIntrospectionPreservesVirtualAndPublicPorts)
{
    constexpr auto snapshot = introspection_regression_snapshot();
    EXPECT_TRUE(snapshot.virtual_node_is_preserved);
    EXPECT_TRUE(snapshot.sample_ports_are_preserved);
    EXPECT_TRUE(snapshot.event_ports_are_preserved);
    EXPECT_TRUE(snapshot.shared_lowering_matches_canonical_metadata);
}

TEST(GraphModules, ModulesComposeRecursivelyThroughAuthoredGraphSplicing)
{
    constexpr auto snapshot = recursive_module_snapshot();
    EXPECT_EQ(snapshot.lowered_subgraph_count, 2u);
    EXPECT_EQ(snapshot.nested_scope_count, 1u);
    EXPECT_TRUE(snapshot.parent_scopes_valid);
}

TEST(GraphModules, AnnotatedModuleHasOneTypedVirtualNode)
{
    constexpr auto snapshot = annotated_module_snapshot();
    EXPECT_EQ(snapshot.matching_nodes, 1u);
    EXPECT_EQ(snapshot.virtual_nodes, 1u);
    EXPECT_TRUE(snapshot.id_has_expected_prefix);
}

TEST(GraphModules, FirstClassTiledNodeBundlesSurviveModuleSplicing)
{
    constexpr auto snapshot = tiled_module_snapshot();
    EXPECT_EQ(snapshot.child_sample_inputs, 1u);
    EXPECT_EQ(snapshot.child_sample_outputs, 1u);
    EXPECT_EQ(snapshot.output_channel_type, ChannelTypeId::stereo);
    EXPECT_EQ(snapshot.output_channel_count, 2u);
    EXPECT_TRUE(snapshot.graph_output_is_stereo);
    EXPECT_EQ(snapshot.lowered_subgraph_count, 1u);
    EXPECT_EQ(snapshot.scope_output_sources, 1u);
    EXPECT_EQ(snapshot.scope_member_nodes, 3u);
    EXPECT_TRUE(snapshot.output_source_is_member);
}

TEST(GraphModules, EventInterfacesResolveThroughTheImportedBoundary)
{
    static_assert(event_interfaces_compile());
    EXPECT_TRUE(event_interfaces_compile());
}

TEST(GraphModules, FunctionalSubgraphRemainsAnExplicitBoundaryFacade)
{
    static_assert(functional_subgraph_compiles());
    EXPECT_TRUE(functional_subgraph_compiles());
}

} // namespace iv
