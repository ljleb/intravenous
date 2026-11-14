#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/debug_probe.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/introspection.h>
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
        return std::array<TickOutputConfig<Sample>, 1>{};
    }

    void tick(TickSampleContext<CStringConfigNode> const& ctx) const
    {
        ctx.template output<0>().push(0.0f);
    }
};

struct ImmutablePointsNode {
    ImmutableArray<Sample> points;
    explicit ImmutablePointsNode(ImmutableArray<Sample> points_) : points(points_) {}
    static constexpr bool intrinsically_replayable = true;
    static constexpr auto outputs() {
        return std::array{output<Sample>("out", OutputProduction::tick)};
    }
    void tick(TickSampleContext<ImmutablePointsNode> const& ctx) const {
        ctx.output<"out">().push(points.empty() ? Sample{} : points[0]);
    }
};
static_assert(std::is_trivially_copyable_v<ImmutablePointsNode>);
static_assert(std::is_standard_layout_v<ImmutableArray<Sample>>);

struct NodeCallEventSource {
    static constexpr auto outputs()
    {
        return std::array{
            event_output("trigger", EventTypeId::trigger, iv::OutputProduction::tick),
        };
    }

    void tick_block(TickBlockContext<NodeCallEventSource> const&) const {}
};

struct NodeCallEventFirstOutputs {
    static constexpr auto outputs() {
        return std::tuple{
            event_output("event-first", EventTypeId::trigger, OutputProduction::tick),
            output<Sample>("sample-first"),
            event_output("event-second", EventTypeId::trigger, OutputProduction::tick),
            output<Sample>("sample-second"),
        };
    }
    void tick_block(TickBlockContext<NodeCallEventFirstOutputs> const&) const {}
};
struct NodeCallEventSink {
    static constexpr auto inputs() {
        return std::array{event_input("in", EventTypeId::trigger, InputAccess::sequential)};
    }
    void tick_block(TickBlockContext<NodeCallEventSink> const&) const {}
};
struct NodeCallEventForwarder {
    static constexpr auto inputs() { return NodeCallEventSink::inputs(); }
    static constexpr auto outputs() { return NodeCallEventFirstOutputs::outputs(); }
    void tick_block(TickBlockContext<NodeCallEventForwarder> const&) const {}
};
struct NodeCallSampleSink {
    static constexpr auto inputs() { return std::array{input<Sample>("in")}; }
    void tick_block(TickBlockContext<NodeCallSampleSink> const&) const {}
};

struct NodeCallMixedSink {
    static constexpr auto inputs()
    {
        return std::tuple{
            input<Sample>("left", iv::InputAccess::sequential),
            input<Sample>("right", iv::InputAccess::sequential),
            event_input("trigger", EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void tick_block(TickBlockContext<NodeCallMixedSink> const&) const {}
};

struct IntrospectionContinuousDomainNode {
    static constexpr auto inputs()
    {
        return std::array{input<Sample, FractionalGlobalIndex, FFTBlock>(
            "in", InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{output<Sample, FractionalGlobalIndex, FFTBlock>(
            "out", OutputProduction::tick)};
    }

    void tick_block(
        TickBlockContext<IntrospectionContinuousDomainNode> const&) const
    {}
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
        return std::array<TickOutputConfig<Sample>, 1>{};
    }

    void tick(TickSampleContext<StructuredCStringConfigNode> const& ctx) const
    {
        ctx.template output<0>().push(0.0f);
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

ConfiguredGraph configure_direct_public_sample_passthrough()
{
    GraphBuilder g;
    auto input = g.input<"in">(0.0f);
    g.outputs("out"_P = input);
    return std::move(g).finish();
}

ConfiguredGraph configure_introspection_regression()
{
    GraphBuilder g;
    auto input = g.input<"in">(0.25f);
    auto event = g.event_input<"event">(EventTypeId::empty);
    auto sum = details::configure_concrete_node<Sum<mono, SampleStreamLayout::planar, 1>>(g);
    auto const sum_handle = sum.node_bundle_handle();
    auto annotated = _annotate_node_source_info(sum.node_ref(), "sum");
    sum(input);
    g.outputs("out"_P = sum);
    (void)annotated;
    (void)event;
    auto configured = std::move(g).finish();
    configured.node_bundles.set_registered_node_type_identity(
        sum_handle,
        RegisteredNodeTypeIdentity{
            .node_type_id = "iv.test.introspection.sum",
            .provider_package_root = "/tmp/iv-test-provider",
        });
    return configured;
}

ConfiguredGraph configure_continuous_domain_introspection()
{
    GraphBuilder g;
    auto node = details::configure_concrete_node<
        IntrospectionContinuousDomainNode>(g);
    (void)_annotate_node_source_info(node.node_ref(), "continuous-domain");
    return std::move(g).finish();
}

struct IntrospectionRegressionSnapshot {
    bool virtual_node_is_preserved = false;
    bool continuous_ports_are_preserved = false;
    bool event_ports_are_preserved = false;
    bool registered_definition_id_is_preserved = false;
};

IntrospectionRegressionSnapshot introspection_regression_snapshot()
{
    auto const configured = configure_introspection_regression();
    auto const metadata = build_graph_introspection_metadata(configured);
    IntrospectionRegressionSnapshot result;

    result.virtual_node_is_preserved = metadata.virtual_nodes.size() == 1
        && metadata.virtual_nodes.front().source_identity == "sum"
        && metadata.virtual_nodes.front().continuous_inputs.size() == 1
        && metadata.virtual_nodes.front().continuous_outputs.size() == 1;
    result.registered_definition_id_is_preserved =
        metadata.virtual_nodes.size() == 1
        && metadata.virtual_nodes.front().definition_id
            == "iv.test.introspection.sum"
        && metadata.virtual_nodes.front().members.size() == 1
        && metadata.virtual_nodes.front().members.front().definition_id
            == "iv.test.introspection.sum";

    result.continuous_ports_are_preserved =
        metadata.public_continuous_inputs.size() == 1
        && metadata.public_continuous_outputs.size() == 1
        && metadata.public_continuous_inputs.front().family_name == "in"
        && metadata.public_continuous_inputs.front().configured_connected
        && metadata.public_continuous_outputs.front().family_name == "out";

    result.event_ports_are_preserved = metadata.public_event_inputs.size() == 1
        && metadata.public_event_inputs.front().port_index == 0
        && !metadata.public_event_inputs.front().graph_connected
        && metadata.public_event_outputs.empty();
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

    auto configured = details::take_built_graph(session.get());
    EXPECT_THROW(
        (void)details::take_built_graph(session.get()),
        std::logic_error);
    session.reset();

    auto const outputs = configured.public_ports.sample_outputs(
        configured.node_bundles);
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs.front().name, "out");
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
            .index = 0,
        },
        RetainedGlobalData{
            .address = cstring_detail,
            .size = sizeof(cstring_detail),
            .index = 1,
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
    ASSERT_TRUE(relocations[0].retained_global_index.has_value());
    EXPECT_EQ(*relocations[0].retained_global_index, 0u);
    EXPECT_EQ(relocations[0].addend, 0u);
    EXPECT_EQ(relocations[1].byte_offset, offsetof(CStringConfigNode, detail));
    EXPECT_EQ(relocations[1].package_root, "test.pointer-metadata-package");
    ASSERT_TRUE(relocations[1].retained_global_index.has_value());
    EXPECT_EQ(*relocations[1].retained_global_index, 1u);
    EXPECT_EQ(relocations[1].addend, 0u);
    EXPECT_EQ(relocations[2].byte_offset, offsetof(CStringConfigNode, optional));
    EXPECT_TRUE(relocations[2].package_root.empty());
    EXPECT_FALSE(relocations[2].retained_global_index.has_value());
}

TEST(GraphModules, NamedPortKindsSelectTheFirstMatchingOutput)
{
    GraphBuilder g;
    auto source = details::configure_concrete_node<NodeCallEventFirstOutputs>(g);
    auto sample_sink = details::configure_concrete_node<NodeCallSampleSink>(g);
    auto event_sink = details::configure_concrete_node<NodeCallEventSink>(g);
    auto default_sink = details::configure_concrete_node<NodeCallEventSink>(g);
    auto const source_handle = source.node_bundle_handle();
    sample_sink("in"_P = source);
    event_sink("in"_F = source);
    default_sink(source.node_ref()); // Runtime kind from the erased node.
    auto typed_default_sink = details::configure_concrete_node<NodeCallEventSink>(g);
    typed_default_sink(source); // Same default with constexpr source metadata.
    EXPECT_EQ(source.output_port_at(0).port_kind, PortKind::event);
    EXPECT_EQ(source.output_port_at(2), (NodeBundlePortId{source_handle, PortKind::event, 1}));
    EXPECT_EQ(source.output_port_at(3), (NodeBundlePortId{source_handle, PortKind::sample, 1}));
    EXPECT_EQ(source.event_port().sources().front(), (EventOutputPortId{source_handle, 0}));
    EXPECT_EQ(static_cast<SamplePortRef>(source).channels().front(),
        (SampleOutputChannelId{source_handle, 0, 0}));
    g.outputs();
    auto graph = std::move(g).finish();
    ASSERT_EQ(graph.connections.configured_sample_connections().size(), 1u);
    EXPECT_EQ(graph.connections.configured_sample_connections()[0].source_channels[0].channel,
        (SampleOutputChannelId{source_handle, 0, 0}));
    size_t event_target_count = 0;
    for (auto const& connection : graph.connections.configured_event_connections()) {
        ASSERT_EQ(connection.sources.size(), 1u);
        EXPECT_EQ(connection.sources[0].port, (EventOutputPortId{source_handle, 0}));
        event_target_count += connection.targets.size();
    }
    EXPECT_EQ(event_target_count, 3u);
}

TEST(GraphModules, NodeChainingUsesDeclaredKindAndReturnsTheTargetNode)
{
    GraphBuilder g;
    auto source = details::configure_concrete_node<NodeCallEventFirstOutputs>(g);
    auto forwarder = details::configure_concrete_node<NodeCallEventForwarder>(g);
    auto sink = details::configure_concrete_node<NodeCallEventSink>(g);
    auto const source_handle = source.node_bundle_handle();
    auto const forwarder_handle = forwarder.node_bundle_handle();
    auto const sink_handle = sink.node_bundle_handle();
    auto next = source > forwarder;
    EXPECT_EQ(next.node_bundle_handle(), forwarder_handle);
    auto result = next > sink;
    EXPECT_EQ(result.node_bundle_handle(), sink_handle);
    g.outputs(next);
    auto graph = std::move(g).finish();
    auto has_connection = [&](NodeBundleHandle from, NodeBundleHandle to) {
        return std::ranges::any_of(graph.connections.configured_event_connections(), [&](auto const& connection) {
            return std::ranges::any_of(connection.sources, [&](auto const& source) {
                return source.port == EventOutputPortId{from, 0};
            }) && std::ranges::find(connection.targets, EventInputPortId{to, 0})
                != connection.targets.end();
        });
    };
    EXPECT_TRUE(has_connection(source_handle, forwarder_handle));
    EXPECT_TRUE(has_connection(forwarder_handle, sink_handle));
}

TEST(GraphModules, MixedPublicOutputsPreserveKindOrderThroughSubgraphsAndArchives)
{
    GraphBuilder g;
    auto module = g.subgraph([&g](SubgraphBuilder& boundary) {
        auto source = details::configure_concrete_node<NodeCallEventFirstOutputs>(g);
        boundary.outputs("events"_F = source, "audio"_P = source,
            "events2"_F = source.event_port(1), "audio2"_P = source[1]);
    });
    auto const module_handle = module.node_bundle_handle();
    EXPECT_EQ(module.output_port_at(0).port_kind, PortKind::event);
    auto sink = details::configure_concrete_node<NodeCallEventSink>(g);
    sink(module);
    g.outputs("events"_F = module, "audio"_P = module);
    auto graph = std::move(g).finish();
    auto const& outputs = graph.public_ports.outputs(graph.node_bundles);
    ASSERT_EQ(outputs.size(), 2u);
    EXPECT_FALSE(is_continuous(outputs[0]));
    EXPECT_TRUE(is_continuous(outputs[1]));
    auto archive = serialize_configured_graph(graph);
    std::array records{details::node_compiler_record<NodeCallEventFirstOutputs>,
        details::node_compiler_record<NodeCallEventSink>};
    auto restored = deserialize_configured_graph(archive.bytes, records, archive.node_configs);
    auto const& restored_outputs = restored.public_ports.outputs(restored.node_bundles);
    ASSERT_EQ(restored_outputs.size(), 2u);
    EXPECT_FALSE(is_continuous(restored_outputs[0]));
    EXPECT_TRUE(is_continuous(restored_outputs[1]));
    EXPECT_EQ(restored.node_bundles.output_port_at(module_handle, 0).port_kind, PortKind::event);
    EXPECT_EQ(restored.node_bundles.output_port_at(module_handle, 2),
        (NodeBundlePortId{module_handle, PortKind::event, 1}));
    EXPECT_EQ(restored.node_bundles.output_port_at(module_handle, 3),
        (NodeBundlePortId{module_handle, PortKind::sample, 1}));
}

TEST(GraphModules, MixedPublicOutputOrderUsesOutputCountsInsteadOfInputCounts)
{
    GraphBuilder g;
    (void)g.input<"audio-in">();
    (void)g.event_input<"event-in-a">(EventTypeId::trigger);
    (void)g.event_input<"event-in-b">(EventTypeId::trigger);
    (void)g.event_input<"event-in-c">(EventTypeId::trigger);
    auto source = details::configure_concrete_node<NodeCallEventFirstOutputs>(g);
    g.outputs("event-a"_F = source.event_port(0), "audio-a"_P = source[0],
        "audio-b"_P = source[1], "event-b"_F = source.event_port(1));
    auto graph = std::move(g).finish();
    auto const& ports = graph.node_bundles.bundle(
        graph.public_ports.boundary_handle()).boundary_ports();
    ASSERT_EQ(ports.sample_input_count(), 1u);
    ASSERT_EQ(ports.event_input_count(), 3u);
    ASSERT_EQ(ports.sample_output_count(), 2u);
    ASSERT_EQ(ports.event_output_count(), 2u);
    auto const& outputs = graph.public_ports.outputs(graph.node_bundles);
    ASSERT_EQ(outputs.size(), 4u);
    EXPECT_FALSE(is_continuous(outputs[0]));
    EXPECT_TRUE(is_continuous(outputs[1]));
    EXPECT_TRUE(is_continuous(outputs[2]));
    EXPECT_FALSE(is_continuous(outputs[3]));
}

TEST(GraphModules, BareEventNodePublicOutputUsesItsFirstDeclaredPort)
{
    GraphBuilder g;
    auto source = details::configure_concrete_node<NodeCallEventFirstOutputs>(g);
    g.outputs(source.node_ref());
    auto graph = std::move(g).finish();
    EXPECT_EQ(graph.public_ports.sample_outputs(graph.node_bundles).size(), 0u);
    EXPECT_EQ(graph.public_ports.event_outputs(graph.node_bundles).size(), 1u);
}

TEST(GraphModules, MixedNamedRefListPreservesEventFirstOrder)
{
    GraphBuilder g;
    auto source = details::configure_concrete_node<NodeCallEventFirstOutputs>(g);
    g.outputs({{"events", source.event_port()}, {"audio", static_cast<SamplePortRef>(source)}});
    auto graph = std::move(g).finish();
    auto const& outputs = graph.public_ports.outputs(graph.node_bundles);
    ASSERT_EQ(outputs.size(), 2u);
    EXPECT_FALSE(is_continuous(outputs[0]));
    EXPECT_TRUE(is_continuous(outputs[1]));
}

TEST(GraphModules, ImmutableArraysOwnCopiesAcrossGraphAndArchiveLifetimes)
{
    using Session = std::unique_ptr<details::BuilderSession,
        decltype(&details::iv_builder_session_destroy)>;
    Session session(details::iv_builder_session_create(), details::iv_builder_session_destroy);
    std::array fields{NodeConfigPointerFieldData{
        .code_key = details::node_code_key_v<ImmutablePointsNode>,
        // ImmutableArray is standard-layout, with its data pointer first.
        .byte_offset = offsetof(ImmutablePointsNode, points),
    }};
    configure_pointer_metadata_package(session.get(), fields, {});
    GraphBuilder builder(session.get());
    std::array source{Sample{.25f}, Sample{.5f}, Sample{.75f}};
    auto points = builder.immutable_array<Sample>(source);
    EXPECT_NE(points.data(), source.data());
    source[0] = Sample{99};
    EXPECT_EQ(points[0], Sample{.25f});
    auto node = details::configure_concrete_node<ImmutablePointsNode>(builder, points);
    auto const node_handle = node.node_bundle_handle();
    builder.outputs(node);
    auto graph = details::take_built_graph(session.get());
    auto archive = serialize_configured_graph(graph);
    ASSERT_EQ(archive.node_configs.size(), 1u);
    auto& config = archive.node_configs[0];
    ASSERT_EQ(config.relocations.size(), 1u);
    auto const& relocation = config.relocations[0];
    ASSERT_TRUE(relocation.immutable_data);
    EXPECT_FALSE(relocation.retained_global_index);
    EXPECT_TRUE(std::ranges::all_of(
        std::span(config.bytes).subspan(relocation.byte_offset, sizeof(void*)),
        [](std::byte byte) { return byte == std::byte{}; }));
    std::weak_ptr<ImmutableConfigData const> lifetime = relocation.immutable_data;
    graph = {};
    session.reset();
    std::array records{details::node_compiler_record<ImmutablePointsNode>};
    auto restored = deserialize_configured_graph(archive.bytes, records, archive.node_configs);
    archive = {};
    ASSERT_FALSE(lifetime.expired());
    auto const restored_node = restored.node_bundles.materialize_concrete_description(node_handle);
    ASSERT_EQ(restored_node.config_relocations.size(), 1u);
    EXPECT_EQ(restored_node.config_relocations[0].immutable_data, lifetime.lock());
    auto const& value = *static_cast<ImmutablePointsNode const*>(restored_node.node_storage.get());
    ASSERT_EQ(value.points.size(), 3u);
    EXPECT_EQ(value.points[0], Sample{.25f});
    EXPECT_EQ(value.points[1], Sample{.5f});
    EXPECT_EQ(value.points[2], Sample{.75f});
}

TEST(GraphModules, EmptyImmutableArrayHasNoAllocation)
{
    GraphBuilder builder;
    auto points = builder.immutable_array<Sample>({});
    EXPECT_TRUE(points.empty());
    EXPECT_EQ(points.data(), nullptr);
    EXPECT_EQ(points.begin(), points.end());
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
            .index = 7,
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
    ASSERT_TRUE(relocations.front().retained_global_index.has_value());
    EXPECT_EQ(*relocations.front().retained_global_index, 7u);

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
            .index = 0,
        },
        RetainedGlobalData{
            .address = cstring_right,
            .size = sizeof(cstring_right),
            .index = 1,
        },
        RetainedGlobalData{
            .address = cstring_first,
            .size = sizeof(cstring_first),
            .index = 2,
        },
        RetainedGlobalData{
            .address = cstring_second,
            .size = sizeof(cstring_second),
            .index = 3,
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
        ASSERT_TRUE(relocations[index].retained_global_index.has_value());
        EXPECT_EQ(*relocations[index].retained_global_index, index);
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
        .index = 0,
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

    auto configured = std::move(builder).finish();
    auto const outputs = configured.public_ports.sample_outputs(
        configured.node_bundles);
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs.front().name, "main");
}

TEST(GraphModules, PublicInputCanFeedPublicOutputWithoutAnInternalNode)
{
    auto const configured = configure_direct_public_sample_passthrough();
    EXPECT_EQ(configured.public_ports.sample_inputs(
                  configured.node_bundles).size(), 1u);
    EXPECT_EQ(configured.public_ports.sample_outputs(
                  configured.node_bundles).size(), 1u);
}

TEST(GraphModules, RuntimeIntrospectionPreservesVirtualAndPublicPorts)
{
    auto snapshot = introspection_regression_snapshot();
    EXPECT_TRUE(snapshot.virtual_node_is_preserved);
    EXPECT_TRUE(snapshot.continuous_ports_are_preserved);
    EXPECT_TRUE(snapshot.event_ports_are_preserved);
    EXPECT_TRUE(snapshot.registered_definition_id_is_preserved);
}

TEST(GraphModules, RuntimeIntrospectionPreservesContinuousValueTypeDomains)
{
    auto const configured = configure_continuous_domain_introspection();
    auto const metadata = build_graph_introspection_metadata(configured);
    ASSERT_EQ(metadata.virtual_nodes.size(), 1u);
    auto const& node = metadata.virtual_nodes.front();
    ASSERT_EQ(node.continuous_inputs.size(), 1u);
    ASSERT_EQ(node.continuous_outputs.size(), 1u);
    auto const& input_types = node.continuous_inputs.front().value_types;
    auto const& output_types = node.continuous_outputs.front().value_types;
    ASSERT_EQ(input_types.size(), 3u);
    ASSERT_EQ(output_types.size(), 3u);
    EXPECT_EQ(
        continuous_introspection_value_type(input_types[0]),
        ContinuousValueTypeId::sample);
    EXPECT_EQ(
        continuous_introspection_value_type(input_types[1]),
        ContinuousValueTypeId::fractional_global_index);
    EXPECT_EQ(
        continuous_introspection_value_type(input_types[2]),
        ContinuousValueTypeId::fft_block);
    EXPECT_EQ(
        continuous_introspection_value_type(output_types[0]),
        ContinuousValueTypeId::sample);
    EXPECT_EQ(
        continuous_introspection_value_type(output_types[1]),
        ContinuousValueTypeId::fractional_global_index);
    EXPECT_EQ(
        continuous_introspection_value_type(output_types[2]),
        ContinuousValueTypeId::fft_block);
}

} // namespace iv
