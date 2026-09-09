#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/debug_probe.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/dsl.h>
#include <authored_graph_test_view.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/compiler.h>
#include <intravenous/module/authored_graph_wire.h>
#include <intravenous/module/builder_session.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>

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
    auto child = g.module<pass_module>();
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

void event_module(GraphBuilder& g)
{
    auto input = g.event_input<"event">(EventTypeId::empty);
    auto relay = g.node<EventConcatenation>(1, EventTypeId::empty);
    relay.connect_event_input(0, input);
    g.event_outputs("event"_P = relay.event_port());
    g.outputs();
}

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
    static constexpr auto event_outputs()
    {
        return std::array<EventOutputConfig, 1>{EventOutputConfig {
            .name = "trigger", .type = EventTypeId::trigger,
        }};
    }

    void tick_block(TickBlockContext<NodeCallEventSource> const&) const {}
};

struct NodeCallMixedSink {
    static constexpr auto inputs()
    {
        return std::array<InputConfig, 2>{
            InputConfig {.name = "left"},
            InputConfig {.name = "right"},
        };
    }

    static constexpr auto event_inputs()
    {
        return std::array<EventInputConfig, 1>{EventInputConfig {
            .name = "trigger", .type = EventTypeId::trigger,
        }};
    }

    void tick_block(TickBlockContext<NodeCallMixedSink> const&) const {}
};

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

static_assert(std::invocable<decltype(&pass_module), GraphBuilder&>);
static_assert(std::same_as<std::invoke_result_t<decltype(&pass_module), GraphBuilder&>, void>);

iv::RuntimeGraphPlan compile_graph(
    iv::AuthoredGraphTestView view,
    bool execution_root = false)
{
    auto authored = iv::thaw_authored_graph_for_test(view);
    auto executable = iv::GraphLowerer::lower(
        std::move(authored), {.execution_root = execution_root});
    return iv::GraphCompiler::compile(std::move(executable));
}

struct RootSignatureAuthoring {
    AuthoredGraphTestView root_view;
    AuthoredGraphTestView parent_view;
    size_t child_sample_inputs;
    size_t child_sample_outputs;
};

RootSignatureAuthoring author_root_signature_graphs()
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
        .root_view = freeze_authored_graph_for_test(std::move(root).finish()),
        .parent_view = freeze_authored_graph_for_test(std::move(parent).finish()),
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
    AuthoredGraphTestView view;
};

RecursiveModuleAuthoring author_recursive_module()
{
    GraphBuilder g;
    auto child = g.module<nested_module>();
    child("in"_P = 0.5f);
    g.outputs("main"_P = child["out"]);
    return {.view = freeze_authored_graph_for_test(std::move(g).finish())};
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
    AuthoredGraphTestView view;
};

AnnotatedModuleAuthoring author_annotated_module()
{
    GraphBuilder g;
    auto child = _annotate_node_source_info(
        g.module<pass_module>(),
        "module-call");
    child("in"_P = 0.5f);
    g.outputs("main"_P = child["out"]);
    return {.view = freeze_authored_graph_for_test(std::move(g).finish())};
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
    AuthoredGraphTestView view;
    size_t child_sample_inputs;
    size_t child_sample_outputs;
    ChannelTypeId output_channel_type;
    size_t output_channel_count;
};

TiledModuleAuthoring author_tiled_module()
{
    GraphBuilder g;
    auto child = g.module<tiled_module>();
    auto const child_sample_inputs = child.sample_input_count();
    auto const child_sample_outputs = child.sample_output_count();
    child("in"_P = 0.5f);
    auto output = child["out"];
    auto const output_channel_type = output.channel_type;
    auto const output_channel_count = output.channels().size();
    g.outputs("main"_P = output);
    return {
        .view = freeze_authored_graph_for_test(std::move(g).finish()),
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

AuthoredGraphTestView author_event_interfaces()
{
    GraphBuilder g;
    auto child = g.module<event_module>();
    auto source = g.node<EventConcatenation>(0, EventTypeId::empty);
    child.connect_event_input("event", source.event_port());
    auto sink = g.node<DummyEventSink>();
    sink.connect_event_input(0, child.event_port("event"));
    g.outputs();
    return freeze_authored_graph_for_test(std::move(g).finish());
}

bool event_interfaces_compile()
{
    (void)compile_graph(author_event_interfaces());
    return true;
}

AuthoredGraphTestView author_functional_subgraph()
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
    return freeze_authored_graph_for_test(std::move(g).finish());
}

bool functional_subgraph_compiles()
{
    (void)compile_graph(author_functional_subgraph());
    return true;
}

AuthoredGraphTestView author_direct_public_sample_passthrough()
{
    GraphBuilder g;
    auto input = g.input<"in">(0.0f);
    g.outputs("out"_P = input);
    return freeze_authored_graph_for_test(std::move(g).finish());
}

bool direct_public_sample_passthrough_compiles()
{
    auto const built = compile_graph(author_direct_public_sample_passthrough());
    return built.graph.inputs().size() == 1
        && built.graph.outputs().size() == 1;
}

struct IntrospectionRegressionAuthoring {
    AuthoredGraphTestView view;
};

IntrospectionRegressionAuthoring author_introspection_regression()
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
    return {.view = freeze_authored_graph_for_test(std::move(g).finish())};
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

TEST(GraphModules, BuilderSessionOwnsStateRatherThanAGraphBuilderObject)
{
    auto session = std::unique_ptr<
        details::BuilderSession,
        decltype(&details::iv_builder_session_destroy)>(
            details::iv_builder_session_create(),
            details::iv_builder_session_destroy);
    ASSERT_NE(session, nullptr);

    GraphBuilder builder(session.get());
    pass_module(builder);

    auto view = freeze_authored_graph_for_test(
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
    auto source = graph.node<NodeCallEventSource>();
    auto sink = graph.node<NodeCallMixedSink>();
    auto tiled_sink = graph.node<NodeCallMixedSink, stereo>();

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

TEST(GraphModules, BuilderCapturesCStringConfigurationFromCompilerFieldMetadata)
{
    auto session = std::unique_ptr<
        details::BuilderSession,
        decltype(&details::iv_builder_session_destroy)>(
            details::iv_builder_session_create(),
            details::iv_builder_session_destroy);
    std::array offsets{
        offsetof(CStringConfigNode, title),
        offsetof(CStringConfigNode, detail),
        offsetof(CStringConfigNode, optional),
    };
    std::array layouts{details::NodeConfigLayout{
        .node_code_key = details::node_code_key_v<CStringConfigNode>,
        .c_string_offsets = offsets,
    }};
    details::set_builder_node_config_layouts(session.get(), layouts);

    GraphBuilder builder(session.get());
    std::string title = "test probe";
    std::string detail = "second string";
    auto probe = builder.node<CStringConfigNode>(CStringConfigNode{
        .title = title.c_str(),
        .detail = detail.c_str(),
        .optional = nullptr,
    });
    builder.outputs(probe);

    auto archive = serialize_authored_graph(
        details::take_built_graph(session.get()));
    ASSERT_EQ(archive.node_configs.size(), 1u);
    auto const& relocations = archive.node_configs.front().string_relocations;
    ASSERT_EQ(relocations.size(), 2u);
    EXPECT_EQ(relocations[0].byte_offset, offsetof(CStringConfigNode, title));
    EXPECT_EQ(relocations[0].value, title);
    EXPECT_EQ(relocations[1].byte_offset, offsetof(CStringConfigNode, detail));
    EXPECT_EQ(relocations[1].value, detail);
    char const* optional = reinterpret_cast<char const*>(1);
    std::memcpy(
        &optional,
        archive.node_configs.front().bytes.data()
            + offsetof(CStringConfigNode, optional),
        sizeof(optional));
    EXPECT_EQ(optional, nullptr);
}

TEST(GraphModules, BuilderCapturesNestedAndArrayCStringConfiguration)
{
    auto session = std::unique_ptr<
        details::BuilderSession,
        decltype(&details::iv_builder_session_destroy)>(
            details::iv_builder_session_create(),
            details::iv_builder_session_destroy);
    std::array offsets{
        offsetof(StructuredCStringConfigNode, labels),
        offsetof(StructuredCStringConfigNode, labels) + sizeof(char const*),
        offsetof(StructuredCStringConfigNode, details)
            + offsetof(CStringConfigDetails, first),
        offsetof(StructuredCStringConfigNode, details)
            + offsetof(CStringConfigDetails, second),
    };
    std::array layouts{details::NodeConfigLayout{
        .node_code_key = details::node_code_key_v<StructuredCStringConfigNode>,
        .c_string_offsets = offsets,
    }};
    details::set_builder_node_config_layouts(session.get(), layouts);

    std::string left = "left label";
    std::string right = "right label";
    std::string first = "first detail";
    std::string second = "second detail";
    GraphBuilder builder(session.get());
    auto node = builder.node<StructuredCStringConfigNode>(
        StructuredCStringConfigNode{
            .labels = {left.c_str(), right.c_str()},
            .details = {first.c_str(), second.c_str()},
        });
    builder.outputs(node);

    auto archive = serialize_authored_graph(
        details::take_built_graph(session.get()));
    ASSERT_EQ(archive.node_configs.size(), 1u);
    auto const& relocations = archive.node_configs.front().string_relocations;
    ASSERT_EQ(relocations.size(), offsets.size());
    EXPECT_EQ(relocations[0].byte_offset, offsets[0]);
    EXPECT_EQ(relocations[0].value, left);
    EXPECT_EQ(relocations[1].byte_offset, offsets[1]);
    EXPECT_EQ(relocations[1].value, right);
    EXPECT_EQ(relocations[2].byte_offset, offsets[2]);
    EXPECT_EQ(relocations[2].value, first);
    EXPECT_EQ(relocations[3].byte_offset, offsets[3]);
    EXPECT_EQ(relocations[3].value, second);
}

TEST(GraphModules, BuilderRejectsAmbiguousCStringConfigurationLayouts)
{
    auto session = std::unique_ptr<
        details::BuilderSession,
        decltype(&details::iv_builder_session_destroy)>(
            details::iv_builder_session_create(),
            details::iv_builder_session_destroy);
    std::array<std::size_t, 2> repeated_offsets{0, 0};
    std::array malformed{details::NodeConfigLayout{
        .node_code_key = details::node_code_key_v<CStringConfigNode>,
        .c_string_offsets = repeated_offsets,
    }};
    EXPECT_THROW(
        details::set_builder_node_config_layouts(session.get(), malformed),
        std::invalid_argument);

    std::array<std::size_t, 1> unique_offset{0};
    std::array duplicate_keys{
        details::NodeConfigLayout{
            .node_code_key = details::node_code_key_v<CStringConfigNode>,
            .c_string_offsets = unique_offset,
        },
        details::NodeConfigLayout{
            .node_code_key = details::node_code_key_v<CStringConfigNode>,
            .c_string_offsets = unique_offset,
        },
    };
    EXPECT_THROW(
        details::set_builder_node_config_layouts(session.get(), duplicate_keys),
        std::invalid_argument);
}

TEST(GraphModules, OutputRequestCopiesBorrowedStringsIntoTheSession)
{
    GraphBuilder builder;
    auto source = builder.node<Constant>(Sample{0.25f});
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
        freeze_authored_graph_for_test(std::move(builder).finish()));
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

TEST(GraphModules, ErasedModuleOutputsSupportRuntimeCheckedChannelOperations)
{
    GraphBuilder graph;
    auto left_module = graph.module<tiled_module>();
    auto right_module = graph.module<tiled_module>();
    left_module("in"_P = 0.25f);
    right_module("in"_P = 0.5f);

    auto const named_output = left_module["out"];
    auto const named_left = named_output[stereo::left];
    auto const default_right = right_module[stereo::right];
    auto const sum = left_module + right_module;
    auto const sum_port = static_cast<SamplePortRef>(sum);
    auto const sum_left = sum[stereo::left];
    graph.outputs("main"_P = sum);

    EXPECT_EQ(static_cast<SamplePortRef>(named_left).channel_type,
              ChannelTypeId::mono);
    EXPECT_EQ(static_cast<SamplePortRef>(default_right).channel_type,
              ChannelTypeId::mono);
    EXPECT_EQ(sum_port.channel_type, ChannelTypeId::stereo);
    EXPECT_EQ(sum_port.channels().size(), 2u);
    EXPECT_EQ(static_cast<SamplePortRef>(sum_left).channel_type,
              ChannelTypeId::mono);
    EXPECT_THROW((void)named_output[mono::center], std::logic_error);

    auto const plan = compile_graph(
        freeze_authored_graph_for_test(std::move(graph).finish()));
    ASSERT_EQ(plan.graph.outputs().size(), 1u);
    EXPECT_EQ(plan.graph.outputs().front().channel_layout.channel_type,
              ChannelTypeId::stereo);
}

TEST(GraphModules, EventInterfacesResolveThroughTheImportedBoundary)
{
    EXPECT_TRUE(event_interfaces_compile());
}

TEST(GraphModules, FunctionalSubgraphRemainsAnExplicitBoundaryFacade)
{
    EXPECT_TRUE(functional_subgraph_compiles());

    auto const built = compile_graph(author_functional_subgraph());
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
        auto const relay = g.node<EventConcatenation>(1, EventTypeId::empty);
        relay.connect_event_input(0, input);
        g.event_outputs("event"_P = relay.event_port());
    });
    scope.connect_event_input("event", source);
    g.outputs();

    auto const built = compile_graph(
        freeze_authored_graph_for_test(std::move(g).finish()));
    ASSERT_EQ(built.metadata.lowered_subgraphs.size(), 1u);
    auto const& lowered_scope = built.metadata.lowered_subgraphs.front();
    EXPECT_TRUE(lowered_scope.sample_outputs.empty());
    ASSERT_EQ(lowered_scope.event_inputs.size(), 1u);
    EXPECT_EQ(lowered_scope.event_inputs.front().name, "event");
    ASSERT_EQ(built.graph.outputs().size(), 0u);
    ASSERT_EQ(built.graph.event_outputs().size(), 1u);
    EXPECT_EQ(built.graph.event_outputs().front().name, "event");
}

} // namespace iv
