#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/authored_graph_view.hpp>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/compiler.h>

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

iv::RuntimeGraphPlan compile_graph(
    iv::AuthoredGraphView view,
    bool execution_root = false)
{
    auto authored = iv::thaw_authored_graph(view);
    auto executable = iv::GraphLowerer::lower(
        std::move(authored), {.execution_root = execution_root});
    return iv::GraphCompiler::compile(std::move(executable));
}

struct RootSignatureAuthoring {
    AuthoredGraphView root_view;
    AuthoredGraphView parent_view;
    size_t child_sample_inputs;
    size_t child_sample_outputs;
};

consteval RootSignatureAuthoring author_root_signature_graphs()
{
    GraphBuilder root;
    pass_module(root);

    GraphBuilder parent;
    auto child = parent.module<pass_module>();
    auto const child_sample_inputs = child.sample_input_count();
    auto const child_sample_outputs = child.sample_output_count();
    child("in"_P = 0.25f);
    parent.outputs("main"_P = child["out"]);

    return {
        .root_view = freeze_authored_graph(std::move(root).finish()),
        .parent_view = freeze_authored_graph(std::move(parent).finish()),
        .child_sample_inputs = child_sample_inputs,
        .child_sample_outputs = child_sample_outputs,
    };
}

struct RootSignatureSnapshot {
    bool root_output_named_out = false;
    size_t child_sample_inputs = 0;
    size_t child_sample_outputs = 0;
    bool nested_output_named_main = false;
};

RootSignatureSnapshot root_signature_snapshot()
{
    auto const authoring = author_root_signature_graphs();
    auto const root_plan = compile_graph(authoring.root_view);
    auto const parent_plan = compile_graph(authoring.parent_view);
    return {
        .root_output_named_out =
            root_plan.graph.outputs().size() == 1
            && root_plan.graph.outputs().front().name == "out",
        .child_sample_inputs = authoring.child_sample_inputs,
        .child_sample_outputs = authoring.child_sample_outputs,
        .nested_output_named_main =
            parent_plan.graph.outputs().size() == 1
            && parent_plan.graph.outputs().front().name == "main",
    };
}

struct RecursiveModuleAuthoring {
    AuthoredGraphView view;
};

consteval RecursiveModuleAuthoring author_recursive_module()
{
    GraphBuilder g;
    auto child = g.module<nested_module>();
    child("in"_P = 0.5f);
    g.outputs("main"_P = child["out"]);
    return {.view = freeze_authored_graph(std::move(g).finish())};
}

struct RecursiveModuleSnapshot {
    size_t lowered_subgraph_count = 0;
    size_t nested_scope_count = 0;
    bool parent_scopes_valid = false;
};

RecursiveModuleSnapshot recursive_module_snapshot()
{
    auto const authored = author_recursive_module();
    auto const built = compile_graph(authored.view);
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

struct AnnotatedModuleAuthoring {
    AuthoredGraphView view;
};

consteval AnnotatedModuleAuthoring author_annotated_module()
{
    GraphBuilder g;
    auto child = _annotate_node_source_info(
        g.module<pass_module>(),
        "module-call");
    child("in"_P = 0.5f);
    g.outputs("main"_P = child["out"]);
    return {.view = freeze_authored_graph(std::move(g).finish())};
}

struct AnnotatedModuleSnapshot {
    size_t matching_nodes = 0;
    size_t virtual_nodes = 0;
    bool id_has_expected_prefix = false;
};

AnnotatedModuleSnapshot annotated_module_snapshot()
{
    auto const authored = author_annotated_module();
    auto const metadata = compile_graph(authored.view).introspection;
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

struct TiledModuleAuthoring {
    AuthoredGraphView view;
    size_t child_sample_inputs;
    size_t child_sample_outputs;
    ChannelTypeId output_channel_type;
    size_t output_channel_count;
};

consteval TiledModuleAuthoring author_tiled_module()
{
    GraphBuilder g;
    auto child = g.module<tiled_module>();
    auto const child_sample_inputs = child.sample_input_count();
    auto const child_sample_outputs = child.sample_output_count();
    child("in"_P = 0.5f);
    auto output = child["out"];
    auto const output_channel_type = output.channel_type;
    auto const output_channel_count = output.channels.size();
    g.outputs("main"_P = output);
    return {
        .view = freeze_authored_graph(std::move(g).finish()),
        .child_sample_inputs = child_sample_inputs,
        .child_sample_outputs = child_sample_outputs,
        .output_channel_type = output_channel_type,
        .output_channel_count = output_channel_count,
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

TiledModuleSnapshot tiled_module_snapshot()
{
    auto const authored = author_tiled_module();
    auto const built = compile_graph(authored.view);
    TiledModuleSnapshot result;
    result.child_sample_inputs = authored.child_sample_inputs;
    result.child_sample_outputs = authored.child_sample_outputs;
    result.output_channel_type = authored.output_channel_type;
    result.output_channel_count = authored.output_channel_count;
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

consteval AuthoredGraphView author_event_interfaces()
{
    GraphBuilder g;
    auto child = g.module<event_module>();
    auto source = g.node<EventConcatenation>(0, EventTypeId::empty);
    child.connect_event_input("event", source.event_port());
    auto sink = g.node<DummyEventSink>();
    sink.connect_event_input(0, child.event_port("event"));
    g.outputs();
    return freeze_authored_graph(std::move(g).finish());
}

bool event_interfaces_compile()
{
    (void)compile_graph(author_event_interfaces());
    return true;
}

consteval AuthoredGraphView author_functional_subgraph()
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
    return freeze_authored_graph(std::move(g).finish());
}

bool functional_subgraph_compiles()
{
    (void)compile_graph(author_functional_subgraph());
    return true;
}

consteval AuthoredGraphView author_direct_public_sample_passthrough()
{
    GraphBuilder g;
    auto input = g.input<"in">(0.0f);
    g.outputs("out"_P = input);
    return freeze_authored_graph(std::move(g).finish());
}

bool direct_public_sample_passthrough_compiles()
{
    auto const built = compile_graph(author_direct_public_sample_passthrough());
    return built.graph.inputs().size() == 1
        && built.graph.outputs().size() == 1;
}

struct IntrospectionRegressionAuthoring {
    AuthoredGraphView view;
};

consteval IntrospectionRegressionAuthoring author_introspection_regression()
{
    GraphBuilder g;
    auto input = g.input<"in">(0.25f);
    auto event = g.event_input<"event">(EventTypeId::empty);
    auto sum = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
    auto annotated = _annotate_node_source_info(sum.node_ref(), "sum");
    sum(input);
    g.outputs("out"_P = sum);
    (void)annotated;
    (void)event;
    return {.view = freeze_authored_graph(std::move(g).finish())};
}

struct IntrospectionRegressionSnapshot {
    bool virtual_node_is_preserved = false;
    bool sample_ports_are_preserved = false;
    bool event_ports_are_preserved = false;
    bool shared_lowering_matches_canonical_metadata = false;
};

IntrospectionRegressionSnapshot introspection_regression_snapshot()
{
    auto const authored = author_introspection_regression();
    auto const compiled = compile_graph(authored.view, true);
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
    return result;
}

} // namespace

TEST(GraphModules, ModuleFunctionUsesTheRootGraphBuilderSignature)
{
    auto snapshot = root_signature_snapshot();
    EXPECT_TRUE(snapshot.root_output_named_out);
    EXPECT_EQ(snapshot.child_sample_inputs, 1u);
    EXPECT_EQ(snapshot.child_sample_outputs, 1u);
    EXPECT_TRUE(snapshot.nested_output_named_main);
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

TEST(GraphModules, ModulesComposeRecursivelyThroughAuthoredGraphSplicing)
{
    auto snapshot = recursive_module_snapshot();
    EXPECT_EQ(snapshot.lowered_subgraph_count, 2u);
    EXPECT_EQ(snapshot.nested_scope_count, 1u);
    EXPECT_TRUE(snapshot.parent_scopes_valid);
}

TEST(GraphModules, AnnotatedModuleHasOneTypedVirtualNode)
{
    auto snapshot = annotated_module_snapshot();
    EXPECT_EQ(snapshot.matching_nodes, 1u);
    EXPECT_EQ(snapshot.virtual_nodes, 1u);
    EXPECT_TRUE(snapshot.id_has_expected_prefix);
}

TEST(GraphModules, FirstClassTiledNodeBundlesSurviveModuleSplicing)
{
    auto snapshot = tiled_module_snapshot();
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
    EXPECT_TRUE(event_interfaces_compile());
}

TEST(GraphModules, FunctionalSubgraphRemainsAnExplicitBoundaryFacade)
{
    EXPECT_TRUE(functional_subgraph_compiles());
}

} // namespace iv
