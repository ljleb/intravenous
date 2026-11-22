// Scoped BuilderSession semantics (docs/scoped_graph_builder_and_subgraph_closure_direction.md):
// session-owned ref identity, nested live scopes, non-destructive snapshots,
// construction failure that fails its session, both SubgraphClosure
// alternatives, and in-place finalization of a nested view.

#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/configured_graph.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Pass = iv::SumN<iv::mono, iv::SampleStreamLayout::planar, 1>;

struct PrivatePass {
    iv::Sample default_value;

    void build_graph(iv::GraphBuilder& graph) const
    {
        using namespace iv;
        auto input = iv::programmatic::declare_input(graph, "in", default_value);
        details::iv_builder_prepare_node_source_identity(&graph, "inner-pass", "pass");
        auto pass = graph.node<Pass>();
        pass(input);
        graph.outputs("value"_P = pass);
    }
};

struct RecursivePrivateModule {
    void build_graph(iv::GraphBuilder& graph) const
    {
        (void)graph.node<RecursivePrivateModule>();
    }
};

template<auto Production, iv::OutputRealization Realization, iv::ChannelTypeId Channels = iv::ChannelTypeId::mono>
struct PublicOutputSource {
    static constexpr bool sequential =
        std::is_same_v<std::remove_cv_t<decltype(Production)>, std::remove_cv_t<decltype(iv::OutputProduction::sequential)>>;

    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>("value", Production, Realization,
            {.channel_layout = {.channel_type = Channels, .sample_layout = iv::SampleStreamLayout::interleaved}})};
    }
    void tick_block(iv::TickBlockContext<PublicOutputSource> const&) const requires (sequential) {}
    void tock_coverage(iv::TockCoverageContext<PublicOutputSource>&) const requires (!sequential) {}
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<PublicOutputSource>&) const requires (!sequential) {}
};

struct BufferedEventRegion {
    static constexpr auto outputs() {
        return std::array{iv::event_output("events", iv::EventTypeId::trigger,
            iv::OutputProduction::region, iv::OutputRealization::buffered)};
    }
    void tock_coverage(iv::TockCoverageContext<BufferedEventRegion>&) const {}
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<BufferedEventRegion>&) const {}
};

// Every component a snapshot must preserve exactly.
struct GraphShape {
    std::size_t bundles = 0;
    std::size_t sample_connections = 0;
    std::size_t event_connections = 0;
    std::vector<std::string> virtual_ids{};
    std::vector<std::size_t> virtual_member_counts{};
    std::vector<std::size_t> virtual_source_info_counts{};
    std::vector<std::size_t> bundle_source_info_counts{};

    bool operator==(GraphShape const&) const = default;
};

GraphShape shape_of(iv::ConfiguredGraph const& graph)
{
    GraphShape result{
        .bundles = graph.node_bundles.size(),
        .sample_connections = graph.connections.configured_sample_connections().size(),
        .event_connections = graph.connections.configured_event_connections().size(),
    };
    for (auto const& record : graph.virtual_nodes.records()) {
        result.virtual_ids.push_back(record.id);
        result.virtual_member_counts.push_back(record.node_bundle_handles.size());
        result.virtual_source_info_counts.push_back(record.source_infos.size());
    }
    for (iv::NodeBundleHandle handle = 0; handle < graph.node_bundles.size(); ++handle) {
        result.bundle_source_info_counts.push_back(
            graph.node_bundles.bundle(handle).source_annotations().infos.size());
    }
    return result;
}

iv::ConfiguredGraph make_frozen_pass()
{
    using namespace iv;
    GraphBuilder graph;
    auto const input = iv::programmatic::declare_input(graph, "in");
    auto pass = details::configure_concrete_node<Pass>(graph);
    pass(input);
    pass._annotate_source_info("pass", "/tmp/frozen.cpp", 1, 2);
    graph.outputs("out"_P = pass);
    return std::move(graph).finish();
}
} // namespace

TEST(ScopedGraphBuilder, MonoTileReusesTheSampleExpression)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = graph.node<Pass>();
    auto const source_port = static_cast<SamplePortRef>(source);
    auto const output = graph.tile<mono>(source_port);
    EXPECT_EQ(output.erased().handle, source_port.handle);
    auto const node_output = graph.tile<mono>(source);
    EXPECT_TRUE(std::ranges::equal(node_output.erased().channels(), source_port.channels()));
}

TEST(ScopedGraphBuilder, StereoArithmeticStillCreatesOneMemberPerChannel)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = graph.node<Pass>();
    auto quotient = graph.tile<stereo>(source, source) / 2;
    auto const handle = quotient.node_bundle_handle();
    graph.outputs("out"_P = quotient);
    auto const configured = std::move(graph).finish();
    auto const& bundle = configured.node_bundles.bundle(handle);
    ASSERT_TRUE(bundle.is_tiled());
    EXPECT_EQ(bundle.tiled_channel_type(), ChannelTypeId::stereo);
    EXPECT_EQ(bundle.tiled_members().size(), 2u);
}

TEST(ScopedGraphBuilder, MonoLayoutIsRejectedForTiledBundles)
{
    using namespace iv;
    GraphBuilder graph;
    EXPECT_THROW((graph.node<Pass, mono>()), std::exception);

    auto const configured = make_frozen_pass();
    auto bundles = configured.node_bundles;
    std::array const members{NodeBundleHandle{0}};
    EXPECT_THROW(bundles.append_tiled(members, mono_planar_channel_layout), std::exception);

    std::array const records{ConfiguredNodeBundleRecord{
        .kind = ConfiguredNodeBundleKind::tiled, .tiled_channel_type = ChannelTypeId::mono}};
    EXPECT_THROW(GraphBuilderNodeBundles::from_configured_records(records), std::exception);
}

TEST(ScopedGraphBuilder, PrivateModuleConstructionPreservesItsBoundary)
{
    using namespace iv;
    GraphBuilder root;
    details::iv_builder_prepare_node_source_identity(&root, "private-module", "module");
    auto module = root.node<PrivatePass>(Sample{.25f});
    auto const handle = module.node_bundle_handle();
    auto const input = iv::programmatic::declare_input(root, "in");
    module("in"_P = input);
    root.outputs("out"_P = module["value"_P]);

    auto const graph = std::move(root).finish();
    auto const& bundles = graph.node_bundles;
    auto const& bundle = bundles.bundle(handle);
    ASSERT_TRUE(bundle.is_subgraph());
    ASSERT_EQ(bundle.sample_input_count(), 1u);
    ASSERT_EQ(bundle.sample_output_count(), 1u);
    EXPECT_EQ(bundles.resolve_sample_input({handle, PortKind::sample, 0}).config.name, "in");
    EXPECT_EQ(bundles.resolve_sample_output({handle, PortKind::sample, 0}).config.name, "value");
}

TEST(ScopedGraphBuilder, RecursivePrivateModuleFailsConstruction)
{
    iv::GraphBuilder root;
    EXPECT_THROW(root.node<RecursivePrivateModule>(), std::runtime_error);
}

TEST(ScopedGraphBuilder, PublicSampleOutputsInferProductionAndRealizationAcrossDeclarations)
{
    using namespace iv;
    GraphBuilder root;
    auto region = details::configure_concrete_node<PublicOutputSource<OutputProduction::region, OutputRealization::buffered>>(root);
    auto other_region = details::configure_concrete_node<PublicOutputSource<OutputProduction::region, OutputRealization::buffered>>(root);
    auto stereo_region = details::configure_concrete_node<
        PublicOutputSource<OutputProduction::region, OutputRealization::buffered, ChannelTypeId::stereo>>(root);
    auto coverage = details::configure_concrete_node<PublicOutputSource<OutputProduction::coverage, OutputRealization::computed>>(root);
    auto sequential = details::configure_concrete_node<PublicOutputSource<OutputProduction::sequential, OutputRealization::buffered>>(root);

    root.outputs("coverage"_P = coverage, "region"_P = region);
    root.outputs({NamedRef{"named-coverage", coverage[0]}, NamedRef{"named-region", region[0]}});
    root.outputs("single-stereo"_P[stereo::left] = stereo_region[0].select_channel(0));
    root.outputs("single-stereo"_P[stereo::right] = stereo_region[0].select_channel(1));
    root.outputs("both-buffered"_P[stereo::left] = region, "both-buffered"_P[stereo::right] = other_region);
    root.outputs("with-computed"_P[stereo::left] = region, "with-computed"_P[stereo::right] = coverage);
    root.outputs("with-sequential"_P[stereo::left] = region, "with-sequential"_P[stereo::right] = sequential);
    auto const graph = std::move(root).finish();
    auto const outputs = graph.public_ports.sample_outputs(graph.node_bundles);

    auto expect = [&](std::string_view name, OutputProductionKind production, OutputRealization realization) {
        auto const found = std::ranges::find(outputs, name, &ContinuousOutputConfig::name);
        ASSERT_NE(found, outputs.end()) << name;
        EXPECT_EQ(output_production(*found), production) << name;
        EXPECT_EQ(found->realization, realization) << name;
    };
    expect("coverage", OutputProductionKind::coverage, OutputRealization::computed);
    expect("region", OutputProductionKind::region, OutputRealization::buffered);
    expect("named-coverage", OutputProductionKind::coverage, OutputRealization::computed);
    expect("named-region", OutputProductionKind::region, OutputRealization::buffered);
    expect("single-stereo", OutputProductionKind::region, OutputRealization::buffered);
    expect("both-buffered", OutputProductionKind::coverage, OutputRealization::buffered);
    expect("with-computed", OutputProductionKind::coverage, OutputRealization::computed);
    expect("with-sequential", OutputProductionKind::sequential, OutputRealization::buffered);
}

TEST(ScopedGraphBuilder, PublicEventOutputKeepsItsSourceProductionAndRealization)
{
    using namespace iv;
    GraphBuilder root;
    auto source = details::configure_concrete_node<BufferedEventRegion>(root);
    auto module = root.subgraph([&](SubgraphBuilder& boundary) {
        boundary.event_outputs("forwarded"_F = source.event_port());
    }, "event export");
    root.outputs("exported"_F = module.event_port());
    auto const graph = std::move(root).finish();
    auto const outputs = graph.public_ports.event_outputs(graph.node_bundles);
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs.front().name, "exported");
    EXPECT_EQ(outputs.front().production, OutputProductionKind::region);
    EXPECT_EQ(outputs.front().realization, OutputRealization::buffered);
}

TEST(ScopedGraphBuilder, RefsFromDifferentViewsOfOneSessionAreCompatible)
{
    using namespace iv;
    GraphBuilder root;
    auto source = details::configure_concrete_node<Pass>(root);
    auto nested = root.nested();
    auto inner = details::configure_concrete_node<Pass>(nested);
    // A ref created through the root view wires a node created through the
    // nested view: both belong to one session.
    inner(source);
    nested.outputs("out"_P = inner);
    auto placed = root.subgraph(std::move(nested), "nested");
    auto sink = details::configure_concrete_node<Pass>(root);
    sink(placed);
    root.outputs("out"_P = sink);

    auto const graph = std::move(root).finish();
    EXPECT_EQ(graph.connections.configured_sample_connections().size(), 4u);
}

TEST(ScopedGraphBuilder, RefsFromAnotherSessionAreRejected)
{
    using namespace iv;
    GraphBuilder first;
    GraphBuilder second;
    auto const source = details::configure_concrete_node<Pass>(first);
    auto const sink = details::configure_concrete_node<Pass>(second);
    EXPECT_THROW(sink(source), std::logic_error);
}

TEST(ScopedGraphBuilder, NestedScopeSnapshotLeavesTheSessionUntouched)
{
    using namespace iv;
    GraphBuilder root;
    auto nested = root.nested();
    auto const input = iv::programmatic::declare_input(nested, "in");
    auto pass = details::configure_concrete_node<Pass>(nested);
    pass(input);
    pass._annotate_source_info("pass", "/tmp/frozen.cpp", 1, 2);
    nested.outputs("out"_P = pass);

    auto const before = shape_of(root.finish());
    auto const snapshot = nested.configured_graph();
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().message;
    EXPECT_EQ(shape_of(root.finish()), before);
    EXPECT_EQ(shape_of(*snapshot), shape_of(make_frozen_pass()));

    // The live scope keeps being used for the current construction, and a
    // frozen placement of its snapshot is equivalent to it.
    auto const live = root.subgraph(std::move(nested), "nested");
    root.outputs("out"_P = live);
    GraphBuilder frozen_root;
    auto const frozen = frozen_root.subgraph(*snapshot, "nested");
    frozen_root.outputs("out"_P = frozen);
    EXPECT_EQ(
        shape_of(std::move(root).finish()),
        shape_of(std::move(frozen_root).finish()));
}

TEST(ScopedGraphBuilder, FreeAncestorCaptureIsConstructibleButNotSnapshotable)
{
    using namespace iv;
    GraphBuilder root;
    auto outer = details::configure_concrete_node<Pass>(root);
    auto nested = root.nested();
    auto inner = details::configure_concrete_node<Pass>(nested);
    inner(outer);
    nested.outputs("out"_P = inner);

    auto const snapshot = nested.configured_graph();
    ASSERT_FALSE(snapshot.has_value());
    EXPECT_EQ(snapshot.error().kind,
              ConfiguredGraphSnapshotError::Kind::free_graph_capture);

    auto const placed = root.subgraph(std::move(nested), "nested");
    root.outputs("out"_P = placed);
    // The enclosing scope contains both ends of the capture and is closed.
    EXPECT_TRUE(root.configured_graph().has_value());
    EXPECT_NO_THROW((void)std::move(root).finish());
}

TEST(ScopedGraphBuilder, FailedSubgraphFailsItsSession)
{
    using namespace iv;
    GraphBuilder root;
    auto outer = details::configure_concrete_node<Pass>(root);

    EXPECT_THROW(root.subgraph([&](SubgraphBuilder& boundary) {
        (void)iv::programmatic::declare_input(boundary, "");
        auto inner = details::configure_concrete_node<Pass>(root);
        outer(inner);
        throw std::runtime_error("construction failed");
    }), std::runtime_error);

    // Nothing is undone in place; the session refuses further construction
    // and cannot finish, so the partial graph is never published.
    EXPECT_THROW((void)details::configure_concrete_node<Pass>(root), std::logic_error);
    EXPECT_THROW(root.outputs(), std::logic_error);
    EXPECT_THROW((void)std::move(root).finish(), std::logic_error);
}

TEST(ScopedGraphBuilder, NestedViewDestroyedWithoutFinalizationFailsItsSession)
{
    using namespace iv;
    GraphBuilder root;
    {
        auto nested = root.nested();
        (void)details::configure_concrete_node<Pass>(nested);
        nested.outputs();
    }
    EXPECT_THROW((void)details::configure_concrete_node<Pass>(root), std::logic_error);
    EXPECT_THROW((void)std::move(root).finish(), std::logic_error);
}

TEST(ScopedGraphBuilder, ReusableClosuresPlaceFreshScopesOnEveryInvocation)
{
    using namespace iv;
    GraphBuilder root;
    SubgraphClosure callable = [&](SubgraphBuilder& boundary) {
        auto const input = iv::programmatic::declare_input(boundary, "");
        auto pass = details::configure_concrete_node<Pass>(root);
        pass(input);
        boundary.outputs("out"_P = pass);
    };
    SubgraphClosure frozen{std::make_shared<ConfiguredGraph const>(make_frozen_pass())};

    auto const first = root.subgraph(callable);
    auto const second = root.subgraph(callable);
    auto const third = root.subgraph(frozen);
    auto const fourth = root.subgraph(frozen);
    EXPECT_NE(first.node_bundle_handle(), second.node_bundle_handle());
    EXPECT_NE(third.node_bundle_handle(), fourth.node_bundle_handle());
    root.outputs();
    EXPECT_NO_THROW((void)std::move(root).finish());
}

TEST(ScopedGraphBuilder, NestedViewIsFinalizedInPlaceOnlyOnce)
{
    using namespace iv;
    GraphBuilder root;
    auto nested = root.nested();
    auto pass = details::configure_concrete_node<Pass>(nested);
    nested.outputs("out"_P = pass);
    auto const before = root.finish().node_bundles.size();

    (void)root.subgraph(std::move(nested), "nested");
    // In place: finalization appends only the subgraph bundle.
    EXPECT_EQ(root.finish().node_bundles.size(), before + 1);
    // The moved-from view no longer denotes a scope.
    EXPECT_THROW((void)root.subgraph(std::move(nested)), std::logic_error);
    // A view of another session cannot be finalized here.
    GraphBuilder other;
    EXPECT_THROW((void)root.subgraph(std::move(other)), std::logic_error);
}

TEST(ScopedGraphBuilder, HigherOrderClosureIsInvokedRepeatedlyInADeeperScope)
{
    using namespace iv;
    GraphBuilder root;
    auto source = details::configure_concrete_node<Pass>(root);
    // The closure captures a node of the outer scope and is invoked inside a
    // deeper scope; every invocation connects to it with an ordinary edge.
    SubgraphClosure voice = [&](SubgraphBuilder& boundary) {
        auto pass = details::configure_concrete_node<Pass>(root);
        pass(source);
        boundary.outputs("out"_P = pass);
    };
    auto deeper = root.nested();
    std::vector<NodeBundleHandle> voices;
    for (int index = 0; index < 3; ++index) {
        voices.push_back(deeper.subgraph(voice).node_bundle_handle());
    }
    deeper.outputs();
    EXPECT_FALSE(deeper.configured_graph().has_value());
    auto const placed = root.subgraph(std::move(deeper), "deeper");
    root.outputs();

    auto const graph = std::move(root).finish();
    auto const info = graph.node_bundles.subgraph_info(placed.node_bundle_handle());
    for (auto const handle : voices) {
        EXPECT_GE(handle, info.child_begin);
        EXPECT_LT(handle, info.child_begin + info.child_count);
    }
    EXPECT_EQ(graph.connections.configured_sample_connections().size(), 6u);
}

TEST(ScopedGraphBuilder, CaughtInnerFailureCannotResumeConstruction)
{
    using namespace iv;
    GraphBuilder root;
    bool resumed = false;
    EXPECT_THROW(root.subgraph([&](SubgraphBuilder& boundary) {
        try {
            (void)root.subgraph([&](SubgraphBuilder&) {
                throw std::runtime_error("inner attempt failed");
            });
        } catch (std::runtime_error const&) {
        }
        // The first failure is recorded on the session, so the enclosing
        // closure's next construction operation is refused.
        auto next = details::configure_concrete_node<Pass>(root);
        resumed = true;
        boundary.outputs("out"_P = next);
    }), std::logic_error);
    EXPECT_FALSE(resumed);
    EXPECT_THROW((void)std::move(root).finish(), std::logic_error);
}

TEST(ScopedGraphBuilder, ClosureThatSwallowsAFailureStillFailsItsInvocation)
{
    using namespace iv;
    GraphBuilder root;
    EXPECT_THROW(root.subgraph([&](SubgraphBuilder& boundary) {
        try {
            (void)root.subgraph([&](SubgraphBuilder&) {
                throw std::runtime_error("inner attempt failed");
            });
        } catch (std::runtime_error const&) {
        }
        // Returns normally without touching the builder again.
        (void)boundary;
    }), std::logic_error);
    EXPECT_THROW((void)std::move(root).finish(), std::logic_error);
}

TEST(ScopedGraphBuilder, NestedViewLeakedFromAClosureFailsTheSession)
{
    using namespace iv;
    GraphBuilder root;
    std::optional<GraphBuilder> leaked;
    // The closure returns while a scope it opened is still open, so its own
    // scope cannot be finalized.
    EXPECT_THROW((void)root.subgraph([&](SubgraphBuilder& boundary) {
        auto pass = details::configure_concrete_node<Pass>(root);
        boundary.outputs("out"_P = pass);
        leaked.emplace(root.nested());
    }), std::logic_error);
    ASSERT_TRUE(leaked.has_value());
    EXPECT_THROW(leaked->outputs(), std::logic_error);
    EXPECT_THROW((void)std::move(root).finish(), std::logic_error);
}

TEST(ScopedGraphBuilder, NestedViewsFinalizeInnermostFirst)
{
    using namespace iv;
    {
        GraphBuilder root;
        auto first = root.nested();
        auto first_node = details::configure_concrete_node<Pass>(first);
        first.outputs("out"_P = first_node);
        // Opened while `first` is current, so it is nested inside `first`.
        auto second = root.nested();
        auto second_node = details::configure_concrete_node<Pass>(second);
        second.outputs("out"_P = second_node);

        // Finalizing the outer scope first is refused, and the refusal fails
        // the session like any other error a session operation raises.
        EXPECT_THROW((void)root.subgraph(std::move(first), "first"), std::logic_error);
        EXPECT_THROW((void)root.subgraph(std::move(second), "second"), std::logic_error);
        EXPECT_THROW((void)std::move(root).finish(), std::logic_error);
    }

    GraphBuilder root;
    auto first = root.nested();
    auto first_node = details::configure_concrete_node<Pass>(first);
    first.outputs("out"_P = first_node);
    auto second = root.nested();
    auto second_node = details::configure_concrete_node<Pass>(second);
    second.outputs("out"_P = second_node);
    auto const inner = root.subgraph(std::move(second), "second");
    auto const outer = root.subgraph(std::move(first), "first");
    root.outputs("out"_P = outer);

    auto const graph = std::move(root).finish();
    auto const info = graph.node_bundles.subgraph_info(outer.node_bundle_handle());
    EXPECT_GE(inner.node_bundle_handle(), info.child_begin);
    EXPECT_LT(inner.node_bundle_handle(), info.child_begin + info.child_count);
}

TEST(ScopedGraphBuilder, DestroyingAnOuterViewBeforeItsInnerViewFailsTheSession)
{
    using namespace iv;
    GraphBuilder root;
    std::optional<GraphBuilder> outer{root.nested()};
    auto inner = outer->nested();
    auto pass = details::configure_concrete_node<Pass>(inner);
    inner.outputs("out"_P = pass);

    outer.reset();

    EXPECT_THROW((void)root.subgraph(std::move(inner), "inner"), std::logic_error);
    EXPECT_THROW((void)std::move(root).finish(), std::logic_error);
}

TEST(ScopedGraphBuilder, SnapshotAvailabilityFollowsTheScopeLifecycle)
{
    using namespace iv;
    GraphBuilder root;
    auto outer = root.nested();
    auto inner = outer.nested();
    auto pass = details::configure_concrete_node<Pass>(inner);
    inner.outputs("out"_P = pass);

    auto const incomplete = outer.configured_graph();
    ASSERT_FALSE(incomplete.has_value());
    EXPECT_EQ(incomplete.error().kind,
              ConfiguredGraphSnapshotError::Kind::open_nested_scope);
    EXPECT_TRUE(inner.configured_graph().has_value());

    auto const placed = outer.subgraph(std::move(inner), "inner");
    outer.outputs("out"_P = placed);
    // A finalized nested scope is local content of its parent.
    EXPECT_TRUE(outer.configured_graph().has_value());
    auto const closed = inner.configured_graph();
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error().kind, ConfiguredGraphSnapshotError::Kind::closed_scope);

    (void)root.subgraph(std::move(outer), "outer");
    root.outputs();
    // An interface operation on a closed view is an error of the session, so
    // it fails the session.
    EXPECT_THROW((void)iv::programmatic::declare_input(inner, "late"), std::logic_error);
    EXPECT_THROW((void)std::move(root).finish(), std::logic_error);
}

TEST(ScopedGraphBuilder, RootInterfaceDeclaredWhileANestedScopeIsCurrentBelongsToTheRoot)
{
    using namespace iv;
    GraphBuilder root;
    auto nested = root.nested();
    // Interface operations act on the view's scope; node creation acts on
    // the session's current scope, which is `nested`.
    auto const input = iv::programmatic::declare_input(root, "x");
    auto pass = details::configure_concrete_node<Pass>(root);
    pass(input);
    nested.outputs("out"_P = pass);
    // The nested node reads the root boundary, a capture outside its scope.
    EXPECT_FALSE(nested.configured_graph().has_value());

    auto const placed = root.subgraph(std::move(nested), "nested");
    root.outputs("out"_P = placed);
    auto const graph = std::move(root).finish();
    auto const inputs = graph.public_ports.sample_inputs(graph.node_bundles);
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs.front().name, "x");
    auto const info = graph.node_bundles.subgraph_info(placed.node_bundle_handle());
    auto const handle = pass.node_ref().node_bundle_handle();
    EXPECT_GE(handle, info.child_begin);
    EXPECT_LT(handle, info.child_begin + info.child_count);
}

TEST(ScopedGraphBuilder, FinishingTheRootWhileANestedViewIsOpenFailsTheSession)
{
    using namespace iv;
    GraphBuilder root;
    auto nested = root.nested();
    auto pass = details::configure_concrete_node<Pass>(nested);
    nested.outputs("out"_P = pass);
    EXPECT_THROW((void)std::move(root).finish(), std::logic_error);
    EXPECT_THROW((void)root.subgraph(std::move(nested), "nested"), std::logic_error);
}

TEST(ScopedGraphBuilder, ErrorRaisedByAnOperationFailsTheSessionEvenWhenCaught)
{
    using namespace iv;
    GraphBuilder root;
    auto pass = details::configure_concrete_node<Pass>(root);
    try {
        (void)pass.node_ref()[std::string_view("no_such_port")];
    } catch (std::logic_error const&) {
        // Catching the error does not make the session usable again.
    }
    EXPECT_THROW((void)details::configure_concrete_node<Pass>(root), std::logic_error);
    EXPECT_THROW((void)std::move(root).finish(), std::logic_error);
}

TEST(ScopedGraphBuilder, EnclosingInterfaceDeclaredWhileANestedScopeIsOpenBelongsToTheEnclosingScope)
{
    using namespace iv;
    GraphBuilder root;
    auto voice = root.nested();
    auto pass = details::configure_concrete_node<Pass>(voice);
    voice.outputs("out"_P = pass);

    // An interface operation on the root view acts on the root scope, even
    // while `voice` is the current construction scope. The voice's own
    // contents are unchanged, so its snapshot stays closed.
    (void)iv::programmatic::declare_input(root, "gain");
    EXPECT_TRUE(voice.configured_graph().has_value());

    auto const placed = root.subgraph(std::move(voice), "voice");
    root.outputs("out"_P = placed);
    auto const graph = std::move(root).finish();
    auto const inputs = graph.public_ports.sample_inputs(graph.node_bundles);
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs.front().name, "gain");
}
