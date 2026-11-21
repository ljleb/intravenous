#include "test_support.h"

namespace iv::test::graph_jit_tests {

namespace {

constexpr char graph_jit_compensated_trigger_module_id[] =
    "iv.test.graph_jit.state_context.compensated_trigger_module";

constexpr char graph_jit_routed_unequal_compensation_module_id[] =
    "iv.test.graph_jit.state_context.routed_unequal_compensation_module";

struct SlicedEventConsumerProbeStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 4> indices{};
    std::array<std::uint64_t, 4> block_sizes{};
    std::array<std::uint64_t, 4> event_counts{};
    std::array<std::uint64_t, 4> trigger_counts{};
    std::array<std::uint64_t, 4> first_times{};
    std::array<std::uint64_t, 4> last_times{};
};

struct PersistentEventRingConsumerProbeStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 5> indices{};
    std::array<std::uint64_t, 5> event_counts{};
    std::array<std::uint64_t, 5> first_times{};
    std::array<std::uint64_t, 5> last_times{};
};

struct CompensatedTriggerProbeStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 4> counts{};
    std::array<std::uint64_t, 4> first_times{};
    std::array<std::uint64_t, 4> last_times{};
};

std::shared_ptr<iv::ConfiguredGraph const>
configured_merged_feed_forward_event_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit merged-event builder session");
    }
    auto const package_root = revision.package_root.generic_string();
    std::array packages{iv::details::BuilderPackageView{
        .package_root = package_root,
        .definitions = revision.provider_definitions,
        .config_pointer_fields = revision.config_pointer_fields,
        .retained_globals = revision.retained_globals,
        .node_state_structures = revision.node_state_structures,
    }};
    iv::details::set_builder_packages(session.get(), packages);

    iv::GraphBuilder graph(session.get());
    auto first = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto second = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.limited_trigger_event_source",
        std::nullopt,
        {});
    auto sink = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        std::nullopt,
        {});
    auto sink_b = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        std::nullopt,
        {});

    auto const first_port = first.event_port();
    auto const second_port = second.event_port();
    if (first_port.type != second_port.type
        || first_port.sources().size() != 1
        || second_port.sources().size() != 1) {
        throw std::runtime_error(
            "GraphJit merged-event fixture lost its source shape");
    }
    // Put the limited producer first in the semantic source order. Its raw
    // EventOutputPortId sequence is written across several callback slices
    // before fan-in merges both source sequences.
    std::array merged_sources{
        second_port.sources().front(),
        first_port.sources().front(),
    };
    auto merged = iv::EventPortRef(graph, first_port.type, merged_sources);
    sink.connect_event_input(0, merged);
    sink_b.connect_event_input(0, merged);
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_bounded_merged_feed_forward_event_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit bounded merged-event builder session");
    }
    auto const package_root = revision.package_root.generic_string();
    std::array packages{iv::details::BuilderPackageView{
        .package_root = package_root,
        .definitions = revision.provider_definitions,
        .config_pointer_fields = revision.config_pointer_fields,
        .retained_globals = revision.retained_globals,
        .node_state_structures = revision.node_state_structures,
    }};
    iv::details::set_builder_packages(session.get(), packages);

    iv::GraphBuilder graph(session.get());
    auto burst = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.fan_in_burst_event_source",
        std::nullopt,
        {});
    auto sparse = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.fan_in_sparse_event_source",
        std::nullopt,
        {});
    auto sink = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        std::nullopt,
        {});

    auto const burst_port = burst.event_port();
    auto const sparse_port = sparse.event_port();
    if (burst_port.type != sparse_port.type
        || burst_port.sources().size() != 1
        || sparse_port.sources().size() != 1) {
        throw std::runtime_error(
            "GraphJit bounded merged-event fixture lost its source shape");
    }
    std::array merged_sources{
        burst_port.sources().front(),
        sparse_port.sources().front(),
    };
    auto merged = iv::EventPortRef(graph, burst_port.type, merged_sources);
    sink.connect_event_input(0, merged);
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_overlapping_event_fan_in_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit overlapping-event builder session");
    }
    auto const package_root = revision.package_root.generic_string();
    std::array packages{iv::details::BuilderPackageView{
        .package_root = package_root,
        .definitions = revision.provider_definitions,
        .config_pointer_fields = revision.config_pointer_fields,
        .retained_globals = revision.retained_globals,
        .node_state_structures = revision.node_state_structures,
    }};
    iv::details::set_builder_packages(session.get(), packages);

    iv::GraphBuilder graph(session.get());
    auto shared = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto first = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto second = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto first_sink = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        std::nullopt,
        {});
    auto second_sink = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        std::nullopt,
        {});

    std::array<iv::EventPortRef, 2> first_sources{
        shared.event_port(), first.event_port()};
    std::array<iv::EventPortRef, 2> second_sources{
        shared.event_port(), second.event_port()};
    first_sink.connect_event_input(
        0,
        iv::EventPortRef{
            graph, std::span<iv::EventPortRef const>{first_sources}});
    second_sink.connect_event_input(
        0,
        iv::EventPortRef{
            graph, std::span<iv::EventPortRef const>{second_sources}});
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_structural_event_source_conversion_fan_in_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit structural-event builder session");
    }
    auto const package_root = revision.package_root.generic_string();
    std::array packages{iv::details::BuilderPackageView{
        .package_root = package_root,
        .definitions = revision.provider_definitions,
        .config_pointer_fields = revision.config_pointer_fields,
        .retained_globals = revision.retained_globals,
        .node_state_structures = revision.node_state_structures,
    }};
    iv::details::set_builder_packages(session.get(), packages);

    iv::GraphBuilder graph(session.get());
    auto converted = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto source = iv::details::configure_package_definition_provider(
            graph,
            "iv.test.graph_jit.state_context.midi_event_source",
            std::nullopt,
            {});
        auto raw = source.event_port();
        boundary.event_outputs(iv::EventPortRef{
            graph, iv::EventTypeId::trigger, raw.sources()});
    }, "midi-to-trigger-boundary");
    auto direct = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto source = iv::details::configure_package_definition_provider(
            graph,
            "iv.test.graph_jit.state_context.trigger_event_source",
            std::nullopt,
            {});
        boundary.event_outputs(source.event_port());
    }, "trigger-boundary");
    auto sink = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        std::nullopt,
        {});
    std::array<iv::EventPortRef, 2> sources{
        converted.event_port(), direct.event_port()};
    sink.connect_event_input(
        0,
        iv::EventPortRef{graph, std::span<iv::EventPortRef const>{sources}});
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const> configured_merged_converted_event_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) throw std::runtime_error("could not create merged converted-event session");
    auto const package_root = revision.package_root.generic_string();
    std::array packages{iv::details::BuilderPackageView{
        .package_root = package_root,
        .definitions = revision.provider_definitions,
        .config_pointer_fields = revision.config_pointer_fields,
        .retained_globals = revision.retained_globals,
        .node_state_structures = revision.node_state_structures,
    }};
    iv::details::set_builder_packages(session.get(), packages);
    iv::GraphBuilder graph(session.get());
    auto first = iv::details::configure_package_definition_provider(
        graph, "iv.test.graph_jit.state_context.midi_event_source", std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, "iv.test.graph_jit.state_context.midi_event_source", std::nullopt, {});
    auto sink_a = iv::details::configure_package_definition_provider(
        graph, "iv.test.graph_jit.state_context.trigger_event_consumer", std::nullopt, {});
    auto sink_b = iv::details::configure_package_definition_provider(
        graph, "iv.test.graph_jit.state_context.trigger_event_consumer", std::nullopt, {});
    auto const first_port = first.event_port();
    auto const second_port = second.event_port();
    std::array sources{first_port.sources().front(), second_port.sources().front()};
    auto merged = iv::EventPortRef(graph, first_port.type, sources);
    sink_a.connect_event_input(0, merged);
    sink_b.connect_event_input(0, merged);
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_merged_target_history_event_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create merged target-history event session");
    }
    auto const package_root = revision.package_root.generic_string();
    std::array packages{iv::details::BuilderPackageView{
        .package_root = package_root,
        .definitions = revision.provider_definitions,
        .config_pointer_fields = revision.config_pointer_fields,
        .retained_globals = revision.retained_globals,
        .node_state_structures = revision.node_state_structures,
    }};
    iv::details::set_builder_packages(session.get(), packages);

    iv::GraphBuilder graph(session.get());
    auto first = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto second = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto sink = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.retained_trigger_event_consumer",
        std::nullopt,
        {});
    auto const first_port = first.event_port();
    auto const second_port = second.event_port();
    std::array sources{
        first_port.sources().front(), second_port.sources().front()};
    sink.connect_event_input(
        0, iv::EventPortRef(graph, first_port.type, sources));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_disconnected_retained_event_output_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create disconnected retained-event session");
    }
    auto const package_root = revision.package_root.generic_string();
    std::array packages{iv::details::BuilderPackageView{
        .package_root = package_root,
        .definitions = revision.provider_definitions,
        .config_pointer_fields = revision.config_pointer_fields,
        .retained_globals = revision.retained_globals,
        .node_state_structures = revision.node_state_structures,
    }};
    iv::details::set_builder_packages(session.get(), packages);
    iv::GraphBuilder graph(session.get());
    (void)iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.retained_trigger_event_source",
        std::nullopt,
        {});
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

} // namespace

TEST_F(GraphJitRuntimeFixture, DirectEventFlow)
{
    auto direct_event_graph = configured_module_graph(
        *revision, graph_jit_direct_event_module_id);
    ASSERT_TRUE(direct_event_graph);
    auto direct_event_analysis =
        build_connection_analysis_plan(
            *direct_event_graph, 64);
    ASSERT_TRUE(direct_event_analysis.has_value())
        << (direct_event_analysis
                ? std::string{}
                : direct_event_analysis.error());
    ASSERT_EQ(direct_event_analysis->event_connections.size(), 1u);
    ASSERT_EQ(direct_event_analysis->event_producer_groups.size(), 1u);
    auto const& direct_event_group =
        direct_event_analysis->event_producer_groups.front();
    ASSERT_TRUE(direct_event_group.storage_plan.has_value());
    EXPECT_EQ(
        direct_event_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
    EXPECT_EQ(direct_event_group.sources.size(), 1u);
    EXPECT_EQ(direct_event_group.connection_indices.size(), 1u);

    auto direct_event = compile_graph(direct_event_graph, 123);
    ASSERT_TRUE(direct_event.succeeded())
        << (direct_event.diagnostics.empty()
                ? ""
                : direct_event.diagnostics.front().message);
    ASSERT_EQ(direct_event.compiled_graph->node_layout.nodes.size(), 2u);
    ASSERT_EQ(count_raw_regions(direct_event.compiled_graph->node_layout), 1u);

    auto direct_event_storage =
        direct_event.compiled_graph->node_layout.create_storage(resources);
    direct_event_storage.initialize();
    EventConsumerProbeStateMirror* direct_event_probe = nullptr;
    for (std::size_t i = 0;
         i < direct_event.compiled_graph->node_layout.nodes.size(); ++i) {
        if (direct_event.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            ASSERT_EQ(direct_event_probe, nullptr);
            direct_event_probe = static_cast<EventConsumerProbeStateMirror*>(
                direct_event_storage.state_ptr(i));
        }
    }
    ASSERT_NE(direct_event_probe, nullptr);

    direct_event.compiled_graph->root_operations.tick_block(
        direct_event_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(direct_event_probe->calls, 1u);
    EXPECT_EQ(direct_event_probe->last_index, 0u);
    EXPECT_EQ(direct_event_probe->last_block_size, 64u);
    EXPECT_EQ(direct_event_probe->event_count, 2u);
    EXPECT_EQ(direct_event_probe->trigger_count, 2u);
    EXPECT_EQ(direct_event_probe->first_time, 3u);
    EXPECT_EQ(direct_event_probe->last_time, 63u);
    EXPECT_EQ(direct_event_probe->marker, 0xe71e17u);

    // Direct event storage is per-root-invocation scratch: the producer clears
    // the previous bounded sequence before publishing this block, and the
    // consumer observes only timestamps in the new block.
    direct_event.compiled_graph->root_operations.tick_block(
        direct_event_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(direct_event_probe->calls, 2u);
    EXPECT_EQ(direct_event_probe->last_index, 64u);
    EXPECT_EQ(direct_event_probe->event_count, 2u);
    EXPECT_EQ(direct_event_probe->trigger_count, 2u);
    EXPECT_EQ(direct_event_probe->first_time, 67u);
    EXPECT_EQ(direct_event_probe->last_time, 127u);

}

TEST_F(GraphJitRuntimeFixture, EventRawStorageIsInitializedByNodeStorageLifecycle)
{
    auto feedback_graph = configured_event_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto compiled = compile_graph(feedback_graph, 141);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    std::vector<iv::NodeLayout::RegionHandle> raw_regions;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.regions.size(); ++i) {
        auto const& region = compiled.compiled_graph->node_layout.regions[i];
        if (region.kind != iv::NodeLayout::Region::Kind::raw) continue;
        EXPECT_NE(region.raw_initialize_fn, nullptr);
        EXPECT_TRUE(region.raw_initialize_data.empty());
        raw_regions.push_back(iv::NodeLayout::RegionHandle{.index = i});
        std::ranges::fill(
            storage.region_bytes(raw_regions.back()), std::byte{0xa5});
    }
    ASSERT_FALSE(raw_regions.empty());

    storage.initialize();
    for (auto const region : raw_regions) {
        auto const bytes = storage.region_bytes(region);
        EXPECT_TRUE(std::ranges::all_of(
            bytes, [](std::byte value) { return value == std::byte{}; }));
    }
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInMergesSlicedSourcesAndFansOut)
{
    auto graph = configured_merged_feed_forward_event_graph(*revision);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_connections.size(), 2u);
    ASSERT_EQ(analysis->event_producer_groups.size(), 1u);
    EXPECT_EQ(analysis->event_connections.front().sources.size(), 2u);
    EXPECT_TRUE(analysis->event_connections.front().requires_conversion);
    ASSERT_TRUE(analysis->event_producer_groups.front().storage_plan);
    EXPECT_EQ(
        analysis->event_producer_groups.front().storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);

    auto compiled = compile_graph(graph, 143);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    // The raw EventOutputPortId sequences and fan-in sequences live in the
    // root stack arena. NodeStorage owns only one overflow counter for each
    // concrete producer output.
    EXPECT_EQ(count_raw_regions(compiled.compiled_graph->node_layout), 2u);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    std::vector<EventConsumerProbeStateMirror*> probes;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            probes.push_back(static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(probes.size(), 2u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    for (auto const* probe : probes) {
        ASSERT_NE(probe, nullptr);
        EXPECT_EQ(probe->calls, 1u);
        EXPECT_EQ(probe->event_count, 10u);
        EXPECT_EQ(probe->trigger_count, 10u);
        EXPECT_EQ(probe->first_time, 3u);
        EXPECT_EQ(probe->last_time, 63u);
    }
}

TEST_F(GraphJitRuntimeFixture,
       OneEventOutputCanParticipateInTwoDifferentFanInSets)
{
    auto graph = configured_overlapping_event_fan_in_graph(*revision);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(*graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_producer_groups.size(), 2u);

    auto compiled = compile_graph(graph, 252);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    std::vector<EventConsumerProbeStateMirror*> probes;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            probes.push_back(static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(probes.size(), 2u);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    for (auto const* probe : probes) {
        ASSERT_NE(probe, nullptr);
        EXPECT_EQ(probe->event_count, 4u);
        EXPECT_EQ(probe->trigger_count, 4u);
        EXPECT_EQ(probe->first_time, 3u);
        EXPECT_EQ(probe->last_time, 63u);
    }
}

TEST_F(GraphJitRuntimeFixture,
       StructuralEventSourcesConvertBeforeTheirCommonFanIn)
{
    auto graph =
        configured_structural_event_source_conversion_fan_in_graph(*revision);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(*graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_connections.size(), 1u);
    auto const& connection = analysis->event_connections.front();
    ASSERT_EQ(connection.source_plans.size(), 2u);
    EXPECT_EQ(connection.source_type, iv::EventTypeId::trigger);
    EXPECT_EQ(connection.conversion.step_count, 0u);
    EXPECT_EQ(connection.source_plans[0].source_type, iv::EventTypeId::midi);
    ASSERT_EQ(connection.source_plans[0].conversion.step_count, 1u);
    EXPECT_EQ(
        connection.source_plans[0].conversion.steps[0],
        iv::EventConversionStepId::midi_to_trigger);
    EXPECT_EQ(
        connection.source_plans[1].source_type,
        iv::EventTypeId::trigger);
    EXPECT_EQ(connection.source_plans[1].conversion.step_count, 0u);

    auto compiled = compile_graph(graph, 253);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    EventConsumerProbeStateMirror* probe = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            probe = static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(probe, nullptr);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    // midi_to_trigger emits only MIDI note-on messages. The MIDI source emits
    // two note-ons and one note-off; the direct source emits two triggers.
    EXPECT_EQ(probe->event_count, 4u);
    EXPECT_EQ(probe->trigger_count, 4u);
    EXPECT_EQ(probe->first_time, 3u);
    EXPECT_EQ(probe->last_time, 63u);
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInUsesPlannedAggregateCapacity)
{
    auto graph = configured_bounded_merged_feed_forward_event_graph(*revision);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_producer_groups.size(), 1u);
    ASSERT_TRUE(analysis->event_producer_groups.front().storage_plan);
    EXPECT_EQ(
        analysis->event_producer_groups.front().storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
    // Lowering selects the fan-in aggregate capacity after it creates the raw
    // EventOutputPortId sequences and merge operations.
    auto compiled = compile_graph(graph, 147);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    EXPECT_EQ(count_raw_regions(compiled.compiled_graph->node_layout), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    EventConsumerProbeStateMirror* probe = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            probe = static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(probe, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);

    // Each source declares 0.125 events/sample, so the 64-frame sizing input
    // reserves eight events per producer. Source 0 emits eight and source 1
    // emits four; runtime correctness depends only on the resulting storage
    // aggregate capacity, not on a separate per-producer write quota.
    EXPECT_EQ(probe->calls, 1u);
    EXPECT_EQ(probe->event_count, 12u);
    EXPECT_EQ(probe->trigger_count, 12u);
    EXPECT_EQ(probe->first_time, 3u);
    EXPECT_EQ(probe->last_time, 3u);
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInRejectsUnavoidableLocalStackOverBudget)
{
    auto graph = configured_bounded_merged_feed_forward_event_graph(*revision);
    ASSERT_TRUE(graph);

    // Connection analysis does not create raw EventOutputPortId sequences or
    // fan-in sequences. The hard stack failure belongs to lowering, where
    // those concrete allocations exist.
    auto analysis = build_connection_analysis_plan(
        *graph,
        64,
        iv::RealtimeStorageCostModel{
            .stack_budget_bytes = 7u * sizeof(iv::TimedEvent),
        });
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());

    iv::GraphJit budgeted_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .realtime_storage_cost_model = iv::RealtimeStorageCostModel{
            .stack_budget_bytes = 7u * sizeof(iv::TimedEvent),
        },
    });
    auto compiled = budgeted_jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 212,
        .graph = graph,
        .definitions = definitions,
    });
    ASSERT_FALSE(compiled.succeeded());
    ASSERT_FALSE(compiled.diagnostics.empty());
    EXPECT_NE(
        compiled.diagnostics.front().message.find(
            "packed realtime stack requires"),
        std::string::npos)
        << compiled.diagnostics.front().message;
    EXPECT_NE(
        compiled.diagnostics.front().message.find(
            "after all eligible buffers were moved to NodeStorage"),
        std::string::npos)
        << compiled.diagnostics.front().message;
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInConvertsAfterMerge)
{
    auto graph = configured_merged_converted_event_graph(*revision);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_producer_groups.size(), 1u);
    EXPECT_EQ(analysis->event_producer_groups.front().sources.size(), 2u);
    ASSERT_EQ(analysis->event_connections.size(), 2u);
    for (auto const& connection : analysis->event_connections) {
        EXPECT_EQ(connection.source_type, iv::EventTypeId::midi);
        EXPECT_EQ(connection.target_type, iv::EventTypeId::trigger);
        ASSERT_EQ(connection.conversion.step_count, 1u);
        EXPECT_EQ(
            connection.conversion.steps[0],
            iv::EventConversionStepId::midi_to_trigger);
    }

    auto compiled = compile_graph(graph, 144);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    // The raw EventOutputPortId sequences, fan-in sequence, and converted
    // target sequence are transient stack allocations. NodeStorage owns only
    // the two concrete producer overflow counters.
    EXPECT_EQ(count_raw_regions(compiled.compiled_graph->node_layout), 2u);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    std::vector<EventConsumerProbeStateMirror*> probes;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            probes.push_back(static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(probes.size(), 2u);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    for (auto const* probe : probes) {
        EXPECT_EQ(probe->event_count, 4u);
        EXPECT_EQ(probe->trigger_count, 4u);
        EXPECT_EQ(probe->first_time, 5u);
        EXPECT_EQ(probe->last_time, 17u);
    }
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInTargetHistoryUsesRetainedFanIn)
{
    auto graph = configured_merged_target_history_event_graph(*revision);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_producer_groups.size(), 1u);
    auto const& group = analysis->event_producer_groups.front();
    ASSERT_TRUE(group.storage_plan);
    EXPECT_EQ(
        group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    // Connection analysis carries only the root-call and retained event bounds.
    // Merge copies and raw callback stack buffers are costed from the concrete
    // fan-in operations during lowering.
    EXPECT_EQ(group.storage_requirements.operations.invariant_copied_values, 0u);
    EXPECT_EQ(group.storage_requirements.operations.carry_extra_copied_values, 0u);

    auto compiled = compile_graph(graph, 148);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    RetainedEventConsumerProbeStateMirror* probe = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(RetainedEventConsumerProbeStateMirror)) {
            probe = static_cast<RetainedEventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(probe, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(probe->event_counts[0], 4u);
    EXPECT_EQ(probe->first_times[0], 3u);
    EXPECT_EQ(probe->second_times[0], 3u);
    EXPECT_EQ(probe->last_times[0], 63u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(probe->event_counts[1], 6u);
    EXPECT_EQ(probe->first_times[1], 63u);
    EXPECT_EQ(probe->second_times[1], 63u);
    EXPECT_EQ(probe->last_times[1], 127u);
}

// A fan-in reads a trigger source of latency 8 and one without latency.
// Latency compensation aligns the two paths, as it aligns sample paths
// (sequential_port_storage_planning.md, "Sample and event streams share one
// lifetime and ownership model"): the consumer reads each source's events 8
// positions after they were sent, the latent source's through its latency and
// the other's through compensation 8.
TEST_F(GraphJitRuntimeFixture, EventFanInAlignsSourcesOfUnequalLatency)
{
    auto graph = configured_unequal_latency_event_fan_in_graph(*revision);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(*graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_connections.size(), 1u);
    auto const& connection = analysis->event_connections.front();
    ASSERT_EQ(connection.deliveries.size(), 2u);
    for (auto const& delivery : connection.deliveries) {
        EXPECT_EQ(delivery.read_latency, 8u);
    }

    auto compiled = compile_graph(graph, 446);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* probe = find_unique_state<RetainedEventConsumerProbeStateMirror>(
        compiled.compiled_graph->node_layout, storage);
    ASSERT_NE(probe, nullptr);

    // The latent source sends -5, 5 and 61, read at 3, 13 and 69; the other
    // sends 3 and 63, read at 11 and 71. The window [-8, 64) holds 3, 11, 13.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(probe->calls, 1u);
    EXPECT_EQ(probe->event_counts[0], 3u);
    EXPECT_EQ(probe->first_times[0], 3u);
    EXPECT_EQ(probe->second_times[0], 11u);
    EXPECT_EQ(probe->last_times[0], 13u);

    // Then 59, 69 and 125, read at 67, 77 and 133, and 67 and 127, read at
    // 75 and 135. The window [56, 128) holds 67, 69, 71, 75 and 77.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    ASSERT_EQ(probe->calls, 2u);
    EXPECT_EQ(probe->event_counts[1], 5u);
    EXPECT_EQ(probe->first_times[1], 67u);
    EXPECT_EQ(probe->second_times[1], 69u);
    EXPECT_EQ(probe->last_times[1], 77u);
}

// A node whose sample input reads a ramp of latency 7 also reads triggers.
// Latency compensation aligns its event input with its sample input
// (sequential_port_storage_planning.md, "Sample and event streams share one
// lifetime and ownership model"): it reads each trigger 7 positions after it
// was sent.
TEST_F(GraphJitRuntimeFixture, EventInputIsCompensatedForItsNodesLatentSampleInput)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_compensated_trigger_module_id);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(*graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_connections.size(), 1u);
    ASSERT_EQ(analysis->event_connections.front().deliveries.size(), 1u);
    EXPECT_EQ(
        analysis->event_connections.front().deliveries.front().read_latency,
        7u);

    auto compiled = compile_graph(graph, 448);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* probe = find_unique_state<CompensatedTriggerProbeStateMirror>(
        compiled.compiled_graph->node_layout, storage);
    ASSERT_NE(probe, nullptr);

    // The source sends 3 and 63, then 67 and 127.
    for (std::int64_t begin = 0; begin < 128; begin += 64) {
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(), &empty_tick_invocation, index(begin), 64);
    }
    ASSERT_EQ(probe->calls, 2u);
    EXPECT_EQ(probe->counts[0], 1u);
    EXPECT_EQ(probe->first_times[0], 10u);
    EXPECT_EQ(probe->counts[1], 2u);
    EXPECT_EQ(probe->first_times[1], 70u);
    EXPECT_EQ(probe->last_times[1], 74u);
}

// One authored connection reaches two consumers through a subgraph's event
// input. One of them needs compensation 7, the other none. Each reads the
// source through its own compensation.
TEST_F(GraphJitRuntimeFixture, RoutedEventConnectionCompensatesEachTargetOnItsOwn)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_routed_unequal_compensation_module_id);
    ASSERT_TRUE(graph);
    auto compiled = compile_graph(graph, 450);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* probe = find_unique_state<CompensatedTriggerProbeStateMirror>(
        compiled.compiled_graph->node_layout, storage);
    auto* plain = find_unique_state<RetainedEventConsumerProbeStateMirror>(
        compiled.compiled_graph->node_layout, storage);
    ASSERT_NE(probe, nullptr);
    ASSERT_NE(plain, nullptr);

    // The source sends 3 and 63, then 67 and 127.
    for (std::int64_t begin = 0; begin < 128; begin += 64) {
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(), &empty_tick_invocation, index(begin), 64);
    }
    ASSERT_EQ(probe->calls, 2u);
    EXPECT_EQ(probe->counts[0], 1u);
    EXPECT_EQ(probe->first_times[0], 10u);
    EXPECT_EQ(probe->counts[1], 2u);
    EXPECT_EQ(probe->first_times[1], 70u);
    EXPECT_EQ(probe->last_times[1], 74u);
    ASSERT_EQ(plain->calls, 2u);
    EXPECT_EQ(plain->event_counts[0], 2u);
    EXPECT_EQ(plain->first_times[0], 3u);
    EXPECT_EQ(plain->last_times[0], 63u);
    EXPECT_EQ(plain->event_counts[1], 3u);
    EXPECT_EQ(plain->first_times[1], 63u);
    EXPECT_EQ(plain->last_times[1], 127u);
}

TEST_F(GraphJitRuntimeFixture, DisconnectedTickEventOutputRetainsItsLatency)
{
    auto graph = configured_disconnected_retained_event_output_graph(*revision);
    ASSERT_TRUE(graph);
    auto compiled = compile_graph(graph, 249);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto const& requirements = compiled.compiled_graph
        ->realtime_port_state_requirements.states;
    auto const& realizations = compiled.compiled_graph
        ->realtime_port_state_realizations.event_states;
    ASSERT_EQ(requirements.size(), 1u);
    ASSERT_EQ(realizations.size(), 1u);
    auto const& realization = realizations.front();
    ASSERT_LT(realization.requirement_index, requirements.size());
    auto const& requirement = requirements[realization.requirement_index];
    EXPECT_EQ(requirement.direction, iv::graph_jit::PortDirection::output);
    // A Tick event output owns one window, its last history + latency
    // positions before its next tick.
    EXPECT_EQ(
        requirement.role,
        iv::graph_jit::RealtimePortStateRole::tick_output_history);
    EXPECT_EQ(requirement.extent_samples, 8u);
    EXPECT_EQ(
        realization.authored_storage.storage,
        iv::graph_jit::EventRealtimePortStateStorage::callback_transient);
    ASSERT_TRUE(realization.retained_storage);
    EXPECT_EQ(
        realization.retained_storage->storage,
        iv::graph_jit::EventRealtimePortStateStorage::compact_carry);
    EXPECT_EQ(
        realization.retained_access,
        iv::graph_jit::EventRealtimePortStateAccess::direct);
    EXPECT_FALSE(realization.retained_source_index);
    EXPECT_FALSE(realization.retained_storage->has_source_indices);
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInPersistentRingRetainsBursts)
{
    auto graph = configured_merged_retained_event_graph(*revision, true);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_producer_groups.size(), 1u);
    ASSERT_TRUE(analysis->event_producer_groups.front().storage_plan);
    EXPECT_EQ(
        analysis->event_producer_groups.front().storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    auto compiled = compile_graph(graph, 147);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    PersistentEventRingConsumerProbeStateMirror* probe = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(PersistentEventRingConsumerProbeStateMirror)) {
            probe = static_cast<PersistentEventRingConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(probe, nullptr);
    for (std::uint64_t index : {0u, 64u, 128u, 192u}) {
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(),
            &empty_tick_invocation,
            iv::GlobalIndex::from_bits(index),
            64);
    }
    EXPECT_EQ(probe->event_counts[0], 64u);
    EXPECT_EQ(probe->event_counts[1], 128u);
    EXPECT_EQ(probe->event_counts[2], 192u);
    EXPECT_EQ(probe->event_counts[3], 192u);
    EXPECT_EQ(probe->first_times[3], 69u);
    EXPECT_EQ(probe->last_times[3], 197u);
}

TEST_F(GraphJitRuntimeFixture, TransientEventSlicing)
{
    auto transient_event_graph = configured_module_graph(
        *revision, graph_jit_transient_event_module_id);
    ASSERT_TRUE(transient_event_graph);
    auto transient_event_analysis =
        build_connection_analysis_plan(
            *transient_event_graph, 64);
    ASSERT_TRUE(transient_event_analysis.has_value())
        << (transient_event_analysis
                ? std::string{}
                : transient_event_analysis.error());
    ASSERT_EQ(transient_event_analysis->event_connections.size(), 1u);
    ASSERT_EQ(transient_event_analysis->event_producer_groups.size(), 1u);
    EXPECT_TRUE(
        transient_event_analysis->event_connections[0]
            .requires_block_materialization);
    auto const& transient_event_group =
        transient_event_analysis->event_producer_groups.front();
    ASSERT_TRUE(transient_event_group.storage_plan.has_value());
    EXPECT_EQ(
        transient_event_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
    EXPECT_TRUE(transient_event_group.requires_invocation_aggregate);

    auto transient_event = compile_graph(transient_event_graph, 124);
    ASSERT_TRUE(transient_event.succeeded())
        << (transient_event.diagnostics.empty()
                ? ""
                : transient_event.diagnostics.front().message);
    ASSERT_EQ(transient_event.compiled_graph->node_layout.nodes.size(), 2u);
    // Both sequences live in the root stack arena. NodeStorage owns only the
    // logical producer's overflow counter.
    ASSERT_EQ(count_raw_regions(transient_event.compiled_graph->node_layout), 1u);

    auto transient_event_storage =
        transient_event.compiled_graph->node_layout.create_storage(resources);
    transient_event_storage.initialize();
    SlicedEventConsumerProbeStateMirror* transient_event_probe = nullptr;
    for (std::size_t i = 0;
         i < transient_event.compiled_graph->node_layout.nodes.size(); ++i) {
        if (transient_event.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(SlicedEventConsumerProbeStateMirror)) {
            ASSERT_EQ(transient_event_probe, nullptr);
            transient_event_probe =
                static_cast<SlicedEventConsumerProbeStateMirror*>(
                    transient_event_storage.state_ptr(i));
        }
    }
    ASSERT_NE(transient_event_probe, nullptr);

    transient_event.compiled_graph->root_operations.tick_block(
        transient_event_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(transient_event_probe->calls, 2u);
    EXPECT_EQ(transient_event_probe->indices[0], 0u);
    EXPECT_EQ(transient_event_probe->indices[1], 32u);
    EXPECT_EQ(transient_event_probe->block_sizes[0], 32u);
    EXPECT_EQ(transient_event_probe->block_sizes[1], 32u);
    EXPECT_EQ(transient_event_probe->event_counts[0], 4u);
    EXPECT_EQ(transient_event_probe->event_counts[1], 4u);
    EXPECT_EQ(transient_event_probe->trigger_counts[0], 4u);
    EXPECT_EQ(transient_event_probe->trigger_counts[1], 4u);
    EXPECT_EQ(transient_event_probe->first_times[0], 3u);
    EXPECT_EQ(transient_event_probe->last_times[0], 31u);
    EXPECT_EQ(transient_event_probe->first_times[1], 35u);
    EXPECT_EQ(transient_event_probe->last_times[1], 63u);

    // A short root block produces three 16-frame source slices and a 32+16
    // consumer split. The transient producer sequence is cleared once at the
    // root boundary, then all source slices append before materialization.
    transient_event.compiled_graph->root_operations.tick_block(
        transient_event_storage.buffer().data(), &empty_tick_invocation, index(64), 48);
    ASSERT_EQ(transient_event_probe->calls, 4u);
    EXPECT_EQ(transient_event_probe->indices[2], 64u);
    EXPECT_EQ(transient_event_probe->indices[3], 96u);
    EXPECT_EQ(transient_event_probe->block_sizes[2], 32u);
    EXPECT_EQ(transient_event_probe->block_sizes[3], 16u);
    EXPECT_EQ(transient_event_probe->event_counts[2], 4u);
    EXPECT_EQ(transient_event_probe->event_counts[3], 2u);
    EXPECT_EQ(transient_event_probe->trigger_counts[2], 4u);
    EXPECT_EQ(transient_event_probe->trigger_counts[3], 2u);
    EXPECT_EQ(transient_event_probe->first_times[2], 67u);
    EXPECT_EQ(transient_event_probe->last_times[2], 95u);
    EXPECT_EQ(transient_event_probe->first_times[3], 99u);
    EXPECT_EQ(transient_event_probe->last_times[3], 111u);

    // Event conversion is a transient storage operation owned by the producer
    // group. Two consumers requesting the same MIDI->trigger branch must share
    // one converted sequence rather than materializing the same fanout twice.
}

TEST_F(GraphJitRuntimeFixture, ConvertedEventFanout)
{
    auto converted_event_graph = configured_module_graph(
        *revision, graph_jit_converted_event_fanout_module_id);
    ASSERT_TRUE(converted_event_graph);
    auto converted_event_analysis =
        build_connection_analysis_plan(
            *converted_event_graph, 64);
    ASSERT_TRUE(converted_event_analysis.has_value())
        << (converted_event_analysis
                ? std::string{}
                : converted_event_analysis.error());
    ASSERT_EQ(converted_event_analysis->event_connections.size(), 2u);
    ASSERT_EQ(converted_event_analysis->event_producer_groups.size(), 1u);
    auto const& converted_event_group =
        converted_event_analysis->event_producer_groups.front();
    ASSERT_TRUE(converted_event_group.storage_plan.has_value());
    EXPECT_EQ(
        converted_event_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
    EXPECT_TRUE(converted_event_group.requires_invocation_aggregate);
    ASSERT_EQ(converted_event_group.connection_indices.size(), 2u);
    for (auto const& connection : converted_event_analysis->event_connections) {
        EXPECT_EQ(connection.source_type, iv::EventTypeId::midi);
        EXPECT_EQ(connection.target_type, iv::EventTypeId::trigger);
        EXPECT_TRUE(connection.requires_conversion);
        EXPECT_EQ(connection.conversion.source_type, iv::EventTypeId::midi);
        EXPECT_EQ(connection.conversion.target_type, iv::EventTypeId::trigger);
        ASSERT_EQ(connection.conversion.step_count, 1u);
        EXPECT_EQ(
            connection.conversion.steps[0],
            iv::EventConversionStepId::midi_to_trigger);
    }

    auto converted_event = compile_graph(converted_event_graph, 125);
    ASSERT_TRUE(converted_event.succeeded())
        << (converted_event.diagnostics.empty()
                ? ""
                : converted_event.diagnostics.front().message);
    ASSERT_EQ(converted_event.compiled_graph->node_layout.nodes.size(), 3u);
    // Both sequences are root-stack allocations; the sole raw region is the
    // MIDI producer's overflow counter.
    ASSERT_EQ(count_raw_regions(converted_event.compiled_graph->node_layout), 1u);

    auto converted_event_storage =
        converted_event.compiled_graph->node_layout.create_storage(resources);
    converted_event_storage.initialize();
    std::vector<EventConsumerProbeStateMirror*> converted_event_probes;
    for (std::size_t i = 0;
         i < converted_event.compiled_graph->node_layout.nodes.size(); ++i) {
        if (converted_event.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            converted_event_probes.push_back(
                static_cast<EventConsumerProbeStateMirror*>(
                    converted_event_storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(converted_event_probes.size(), 2u);

    converted_event.compiled_graph->root_operations.tick_block(
        converted_event_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    for (auto const* probe : converted_event_probes) {
        EXPECT_EQ(probe->calls, 1u);
        EXPECT_EQ(probe->event_count, 2u);
        EXPECT_EQ(probe->trigger_count, 2u);
        EXPECT_EQ(probe->first_time, 5u);
        EXPECT_EQ(probe->last_time, 17u);
    }

    converted_event.compiled_graph->root_operations.tick_block(
        converted_event_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    for (auto const* probe : converted_event_probes) {
        EXPECT_EQ(probe->calls, 2u);
        EXPECT_EQ(probe->event_count, 2u);
        EXPECT_EQ(probe->trigger_count, 2u);
        EXPECT_EQ(probe->first_time, 69u);
        EXPECT_EQ(probe->last_time, 81u);
    }

    // Small retained event windows use a compact persistent carry. The
    // producer may add events to its latency window, the newest positions
    // before its block, while the consumer asks for eight samples of history
    // on the following root call.
}

TEST_F(GraphJitRuntimeFixture, CompactRetainedEvents)
{
    auto retained_event_graph = configured_module_graph(
        *revision, graph_jit_retained_event_module_id);
    ASSERT_TRUE(retained_event_graph);
    auto retained_event_analysis =
        build_connection_analysis_plan(
            *retained_event_graph, 64);
    ASSERT_TRUE(retained_event_analysis.has_value())
        << (retained_event_analysis
                ? std::string{}
                : retained_event_analysis.error());
    ASSERT_EQ(retained_event_analysis->event_connections.size(), 1u);
    ASSERT_EQ(retained_event_analysis->event_producer_groups.size(), 1u);
    auto const& retained_event_connection =
        retained_event_analysis->event_connections.front();
    EXPECT_EQ(retained_event_connection.source_latency, 8u);
    EXPECT_EQ(retained_event_connection.target_history, 8u);
    EXPECT_DOUBLE_EQ(retained_event_connection.max_events_per_index, 0.5);
    auto const& retained_event_group =
        retained_event_analysis->event_producer_groups.front();
    EXPECT_DOUBLE_EQ(retained_event_group.max_events_per_index, 0.5);
    ASSERT_TRUE(retained_event_group.storage_plan.has_value());
    EXPECT_EQ(
        retained_event_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(
        retained_event_group.storage_requirements.retained_window_samples,
        16u);
    EXPECT_EQ(
        retained_event_group.storage_requirements.retained_event_capacity,
        8u);
    EXPECT_LT(
        retained_event_group.storage_requirements.retained_event_capacity,
        retained_event_group.storage_requirements.current_event_capacity);

    auto retained_event = compile_graph(retained_event_graph, 126);
    ASSERT_TRUE(retained_event.succeeded())
        << (retained_event.diagnostics.empty()
                ? ""
                : retained_event.diagnostics.front().message);
    ASSERT_EQ(retained_event.compiled_graph->node_layout.nodes.size(), 2u);
    // The producer-local callback sequence is separate from the fan-in
    // working sequence and its persistent compact carry sequence.
    ASSERT_EQ(count_raw_regions(retained_event.compiled_graph->node_layout), 3u);
    auto const& retained_state_requirements = retained_event.compiled_graph
        ->realtime_port_state_requirements.states;
    auto const& retained_state_realizations = retained_event.compiled_graph
        ->realtime_port_state_realizations.event_states;
    ASSERT_EQ(retained_state_requirements.size(), 2u);
    ASSERT_EQ(retained_state_realizations.size(), 2u);
    for (auto const& realization : retained_state_realizations) {
        ASSERT_LT(
            realization.requirement_index,
            retained_state_requirements.size());
        auto const& requirement =
            retained_state_requirements[realization.requirement_index];
        EXPECT_EQ(requirement.configured_port.port_kind, iv::PortKind::event);
        EXPECT_EQ(
            realization.authored_storage.storage,
            iv::graph_jit::EventRealtimePortStateStorage::callback_transient);
        EXPECT_EQ(
            realization.retained_access,
            iv::graph_jit::EventRealtimePortStateAccess::direct);
        ASSERT_TRUE(realization.retained_storage);
        EXPECT_EQ(
            realization.retained_storage->storage,
            iv::graph_jit::EventRealtimePortStateStorage::compact_carry);
        ASSERT_TRUE(realization.retained_storage->node_storage_offset);
        auto const retained_region = std::ranges::find_if(
            retained_event.compiled_graph->node_layout.regions,
            [&](iv::NodeLayout::Region const& region) {
                return region.kind == iv::NodeLayout::Region::Kind::raw
                    && region.storage_offset
                        == *realization.retained_storage->node_storage_offset;
            });
        ASSERT_NE(
            retained_region,
            retained_event.compiled_graph->node_layout.regions.end());
        EXPECT_FALSE(retained_region->migration_identity.empty());
        EXPECT_FALSE(realization.retained_storage->callback_arena_offset);
        EXPECT_FALSE(realization.retained_storage->has_source_indices);
        EXPECT_FALSE(realization.retained_source_index);
        EXPECT_FALSE(realization.materialization_conversion);
    }

    auto retained_event_storage =
        retained_event.compiled_graph->node_layout.create_storage(resources);
    retained_event_storage.initialize();
    RetainedEventConsumerProbeStateMirror* retained_event_probe = nullptr;
    for (std::size_t i = 0;
         i < retained_event.compiled_graph->node_layout.nodes.size(); ++i) {
        if (retained_event.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(RetainedEventConsumerProbeStateMirror)) {
            ASSERT_EQ(retained_event_probe, nullptr);
            retained_event_probe =
                static_cast<RetainedEventConsumerProbeStateMirror*>(
                    retained_event_storage.state_ptr(i));
        }
    }
    ASSERT_NE(retained_event_probe, nullptr);

    retained_event.compiled_graph->root_operations.tick_block(
        retained_event_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    // The source authors -5, 5 and 61; with latency 8 the consumer reads them
    // at 3, 13 and 69. Its window [-8, 64) holds the first two.
    ASSERT_EQ(retained_event_probe->calls, 1u);
    EXPECT_EQ(retained_event_probe->indices[0], 0u);
    EXPECT_EQ(retained_event_probe->event_counts[0], 2u);
    EXPECT_EQ(retained_event_probe->first_times[0], 3u);
    EXPECT_EQ(retained_event_probe->second_times[0], 13u);
    EXPECT_EQ(retained_event_probe->last_times[0], 13u);

    // The compact carry committed [56, 72): 69, the event authored at 61,
    // which was not final yet. The second call authors 59 inside the source's
    // latency window, before 61, then 69 and 125. The consumer's window
    // [56, 128) reads them in position order: 67, 69, 77.
    retained_event.compiled_graph->root_operations.tick_block(
        retained_event_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    ASSERT_EQ(retained_event_probe->calls, 2u);
    EXPECT_EQ(retained_event_probe->indices[1], 64u);
    EXPECT_EQ(retained_event_probe->event_counts[1], 3u);
    EXPECT_EQ(retained_event_probe->first_times[1], 67u);
    EXPECT_EQ(retained_event_probe->second_times[1], 69u);
    EXPECT_EQ(retained_event_probe->last_times[1], 77u);

    // The next retained window is [120, 136): 133 (authored at 125); the
    // first call's events were trimmed rather than accumulating indefinitely.
    retained_event.compiled_graph->root_operations.tick_block(
        retained_event_storage.buffer().data(), &empty_tick_invocation, index(128), 64);
    ASSERT_EQ(retained_event_probe->calls, 3u);
    EXPECT_EQ(retained_event_probe->indices[2], 128u);
    EXPECT_EQ(retained_event_probe->event_counts[2], 3u);
    EXPECT_EQ(retained_event_probe->first_times[2], 131u);
    EXPECT_EQ(retained_event_probe->second_times[2], 133u);
    EXPECT_EQ(retained_event_probe->last_times[2], 141u);

    // Retention larger than the compact-carry budget binds producer and
    // consumer directly to one persistent ring. The ring keeps monotonic
    // read/write indices in NodeStorage and advances only the oldest retained
    // index as the 160-sample history window moves across root calls.
}

TEST_F(GraphJitRuntimeFixture,
       SeparateEventOutputAndInputStateNeedsNoTransitionRoot)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_retained_event_module_id);
    ASSERT_TRUE(graph);
    auto predecessor = compile_graph(graph, 254);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);

    iv::GraphJitCompileRequest successor_request{
        .project_generation = 255,
        .graph = graph,
        .definitions = definitions,
    };
    auto steady_successor = jit->compile(successor_request);
    ASSERT_TRUE(steady_successor.succeeded())
        << (steady_successor.diagnostics.empty()
                ? ""
                : steady_successor.diagnostics.front().message);
    auto specialized = jit->compile_transition(
        iv::GraphJitTransitionCompileRequest{
            .successor = successor_request,
            .steady_successor = steady_successor.compiled_graph,
            .active_predecessor = predecessor.compiled_graph,
            .active_predecessor_phase =
                iv::CompiledGraphRealtimePhase::steady,
        });
    ASSERT_TRUE(specialized.succeeded())
        << (specialized.diagnostics.empty()
                ? ""
                : specialized.diagnostics.front().message);
    ASSERT_TRUE(specialized.compiled_graph->cutover);
    EXPECT_EQ(
        specialized.compiled_graph->root_operations.transition_tick_block,
        nullptr);
    auto const& transition = specialized.compiled_graph->cutover
        ->state_transition;
    // The output's one window and the input's history transfer to their own
    // storage, and the output's latency positions, which the input's group
    // stream also holds, are seeded there.
    ASSERT_EQ(transition.event_transfers.size(), 3u);
    EXPECT_TRUE(std::ranges::all_of(
        transition.event_transfers,
        [](auto const& transfer) { return !transfer.target_transition_only; }));
    EXPECT_EQ(
        std::ranges::count_if(
            transition.event_transfers,
            [](auto const& transfer) {
                return transfer.supplemental_target;
            }),
        1u);

    auto predecessor_storage = predecessor.compiled_graph
        ->node_layout.create_storage(resources);
    predecessor_storage.initialize();
    predecessor.compiled_graph->root_operations.tick_block(
        predecessor_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        64);

    auto successor_storage = specialized.compiled_graph
        ->node_layout.create_storage(resources);
    successor_storage.initialize(&predecessor_storage);
    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        transition,
        predecessor_storage.buffer(),
        successor_storage.buffer());
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared,
        predecessor_storage.buffer(),
        successor_storage.buffer(),
        index(64));

    RetainedEventConsumerProbeStateMirror* probe = nullptr;
    for (std::size_t node = 0;
         node < specialized.compiled_graph->node_layout.nodes.size();
         ++node) {
        if (specialized.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(RetainedEventConsumerProbeStateMirror)) {
            probe = static_cast<RetainedEventConsumerProbeStateMirror*>(
                successor_storage.state_ptr(node));
        }
    }
    ASSERT_NE(probe, nullptr);
    specialized.compiled_graph->root_operations.tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(64),
        64);
    // The probe's State is moved implicitly, so the successor's block is its
    // second call, and it reads what the steady graph reads there: the seeded
    // 69 (authored at 61 before the cutover) between 67 and 77.
    ASSERT_EQ(probe->calls, 2u);
    EXPECT_EQ(probe->event_counts[1], 3u);
    EXPECT_EQ(probe->first_times[1], 67u);
    EXPECT_EQ(probe->second_times[1], 69u);
    EXPECT_EQ(probe->last_times[1], 77u);
}

TEST_F(GraphJitRuntimeFixture, PersistentEventRing)
{
    auto persistent_event_ring_graph = configured_module_graph(
        *revision, graph_jit_persistent_event_ring_module_id);
    ASSERT_TRUE(persistent_event_ring_graph);
    auto persistent_event_ring_analysis =
        build_connection_analysis_plan(
            *persistent_event_ring_graph, 64);
    ASSERT_TRUE(persistent_event_ring_analysis.has_value())
        << (persistent_event_ring_analysis
                ? std::string{}
                : persistent_event_ring_analysis.error());
    ASSERT_EQ(persistent_event_ring_analysis->event_producer_groups.size(), 1u);
    auto const& persistent_event_ring_group =
        persistent_event_ring_analysis->event_producer_groups.front();
    ASSERT_TRUE(persistent_event_ring_group.storage_plan.has_value());
    EXPECT_EQ(
        persistent_event_ring_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(
        persistent_event_ring_group.storage_requirements.retained_window_samples,
        160u);
    EXPECT_EQ(
        persistent_event_ring_group.storage_requirements.retained_event_capacity,
        80u);
    EXPECT_GE(
        persistent_event_ring_group.storage_requirements.retained_event_capacity,
        persistent_event_ring_group.storage_requirements.current_event_capacity);

    auto persistent_event_ring = compile_graph(
        persistent_event_ring_graph, 127);
    ASSERT_TRUE(persistent_event_ring.succeeded())
        << (persistent_event_ring.diagnostics.empty()
                ? ""
                : persistent_event_ring.diagnostics.front().message);
    ASSERT_EQ(
        persistent_event_ring.compiled_graph->node_layout.nodes.size(),
        2u);
    // The persistent ring is the canonical producer storage; the only
    // other raw region is its independent producer overflow counter.
    ASSERT_EQ(
        count_raw_regions(persistent_event_ring.compiled_graph->node_layout),
        2u);
    auto persistent_event_ring_region = std::ranges::find_if(
        persistent_event_ring.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw
                && !region.migration_identity.empty();
        });
    ASSERT_NE(
        persistent_event_ring_region,
        persistent_event_ring.compiled_graph->node_layout.regions.end());
    EXPECT_NE(
        persistent_event_ring_region->migration_identity.find(
            "kind=persistent_ring"),
        std::string::npos);

    auto const& ring_state_requirements = persistent_event_ring.compiled_graph
        ->realtime_port_state_requirements.states;
    auto const& ring_state_realizations = persistent_event_ring.compiled_graph
        ->realtime_port_state_realizations.event_states;
    ASSERT_EQ(ring_state_requirements.size(), 1u);
    ASSERT_EQ(ring_state_realizations.size(), 1u);
    auto const& ring_state = ring_state_realizations.front();
    ASSERT_LT(ring_state.requirement_index, ring_state_requirements.size());
    EXPECT_EQ(
        ring_state_requirements[ring_state.requirement_index].role,
        iv::graph_jit::RealtimePortStateRole::sequential_input_history);
    EXPECT_EQ(
        ring_state.authored_storage.storage,
        iv::graph_jit::EventRealtimePortStateStorage::ring);
    EXPECT_EQ(
        ring_state.retained_access,
        iv::graph_jit::EventRealtimePortStateAccess::direct);
    ASSERT_TRUE(ring_state.retained_storage);
    EXPECT_EQ(
        ring_state.retained_storage->storage,
        iv::graph_jit::EventRealtimePortStateStorage::ring);
    ASSERT_TRUE(ring_state.retained_storage->node_storage_offset);
    EXPECT_EQ(
        *ring_state.retained_storage->node_storage_offset,
        persistent_event_ring_region->storage_offset);
    EXPECT_FALSE(ring_state.retained_storage->callback_arena_offset);
    EXPECT_FALSE(ring_state.retained_storage->has_source_indices);
    EXPECT_FALSE(ring_state.retained_source_index);

    auto persistent_event_ring_storage =
        persistent_event_ring.compiled_graph->node_layout.create_storage(resources);
    persistent_event_ring_storage.initialize();
    PersistentEventRingConsumerProbeStateMirror* persistent_event_ring_probe =
        nullptr;
    for (std::size_t i = 0;
         i < persistent_event_ring.compiled_graph->node_layout.nodes.size(); ++i) {
        if (persistent_event_ring.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(PersistentEventRingConsumerProbeStateMirror)) {
            ASSERT_EQ(persistent_event_ring_probe, nullptr);
            persistent_event_ring_probe =
                static_cast<PersistentEventRingConsumerProbeStateMirror*>(
                    persistent_event_ring_storage.state_ptr(i));
        }
    }
    ASSERT_NE(persistent_event_ring_probe, nullptr);

    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 1u);
    EXPECT_EQ(persistent_event_ring_probe->indices[0], 0u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[0], 32u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[0], 5u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[0], 5u);

    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 2u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[1], 64u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[1], 5u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[1], 69u);

    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), &empty_tick_invocation, index(128), 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 3u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[2], 96u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[2], 5u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[2], 133u);

    // At index 192 the requested history begins at sample 32, so the first
    // block's 32 co-timestamped events expire in-place. The retained data
    // itself is not copied.
    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), &empty_tick_invocation, index(192), 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 4u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[3], 96u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[3], 69u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[3], 197u);

    // Advancing by another root block moves the history start to 96, expires
    // the second block, and appends through the storage end of the 128-index
    // ring. The consumer therefore exercises a wrapped BlockView.
    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), &empty_tick_invocation, index(256), 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 5u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[4], 96u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[4], 133u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[4], 261u);

    // A retained canonical MIDI ring may simultaneously serve an identity
    // history consumer and a deduplicated converted trigger fanout. Conversion
    // materializes only the current consumer window, while target capacity
    // remains source-capacity-sized because the sizing rate does not constrain
    // timestamp clustering.
}

TEST_F(GraphJitRuntimeFixture, RetainedConvertedEventFanout)
{
    auto retained_converted_event_graph = configured_module_graph(
        *revision, graph_jit_retained_converted_event_fanout_module_id);
    ASSERT_TRUE(retained_converted_event_graph);
    auto retained_converted_event_analysis =
        build_connection_analysis_plan(
            *retained_converted_event_graph, 64);
    ASSERT_TRUE(retained_converted_event_analysis.has_value())
        << (retained_converted_event_analysis
                ? std::string{}
                : retained_converted_event_analysis.error());
    ASSERT_EQ(
        retained_converted_event_analysis->event_producer_groups.size(),
        1u);
    auto const& retained_converted_event_group =
        retained_converted_event_analysis->event_producer_groups.front();
    ASSERT_TRUE(retained_converted_event_group.storage_plan.has_value());
    EXPECT_EQ(
        retained_converted_event_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(
        retained_converted_event_group.storage_requirements.retained_window_samples,
        160u);
    EXPECT_TRUE(retained_converted_event_group.requires_invocation_aggregate);
    ASSERT_EQ(
        retained_converted_event_analysis->event_connections.size(),
        3u);
    EXPECT_EQ(
        std::ranges::count_if(
            retained_converted_event_analysis->event_connections,
            [](auto const& connection) {
                return connection.requires_conversion
                    && connection.target_type == iv::EventTypeId::trigger;
            }),
        2);

    EXPECT_EQ(
        std::ranges::count_if(
            retained_converted_event_analysis->event_connections,
            [](auto const& connection) {
                return connection.requires_block_materialization;
            }),
        1);

    auto retained_converted_event = compile_graph(
        retained_converted_event_graph, 128);
    ASSERT_TRUE(retained_converted_event.succeeded())
        << (retained_converted_event.diagnostics.empty()
                ? ""
                : retained_converted_event.diagnostics.front().message);
    ASSERT_EQ(
        retained_converted_event.compiled_graph->node_layout.nodes.size(),
        4u);
    // One persistent canonical MIDI ring plus its producer overflow counter;
    // the deduplicated converted trigger storage is root-stack storage.
    ASSERT_EQ(
        count_raw_regions(retained_converted_event.compiled_graph->node_layout),
        2u);
    auto retained_converted_persistent_region = std::ranges::find_if(
        retained_converted_event.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw
                && !region.migration_identity.empty();
        });
    auto retained_converted_telemetry_region = std::ranges::find_if(
        retained_converted_event.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw
                && region.migration_identity.empty();
        });
    ASSERT_NE(
        retained_converted_persistent_region,
        retained_converted_event.compiled_graph->node_layout.regions.end());
    ASSERT_NE(
        retained_converted_telemetry_region,
        retained_converted_event.compiled_graph->node_layout.regions.end());
    EXPECT_NE(
        retained_converted_persistent_region->migration_identity.find(
            "kind=persistent_ring"),
        std::string::npos);
    auto retained_converted_event_storage =
        retained_converted_event.compiled_graph->node_layout.create_storage(resources);
    retained_converted_event_storage.initialize();
    RetainedMidiConsumerProbeStateMirror* retained_midi_probe = nullptr;
    EventConsumerProbeStateMirror* retained_trigger_probe = nullptr;
    SlicedEventConsumerProbeStateMirror* retained_sliced_trigger_probe = nullptr;
    for (std::size_t i = 0;
         i < retained_converted_event.compiled_graph->node_layout.nodes.size();
         ++i) {
        auto const state_size =
            retained_converted_event.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(RetainedMidiConsumerProbeStateMirror)) {
            ASSERT_EQ(retained_midi_probe, nullptr);
            retained_midi_probe =
                static_cast<RetainedMidiConsumerProbeStateMirror*>(
                    retained_converted_event_storage.state_ptr(i));
        } else if (state_size == sizeof(EventConsumerProbeStateMirror)) {
            ASSERT_EQ(retained_trigger_probe, nullptr);
            retained_trigger_probe =
                static_cast<EventConsumerProbeStateMirror*>(
                    retained_converted_event_storage.state_ptr(i));
        } else if (state_size == sizeof(SlicedEventConsumerProbeStateMirror)) {
            ASSERT_EQ(retained_sliced_trigger_probe, nullptr);
            retained_sliced_trigger_probe =
                static_cast<SlicedEventConsumerProbeStateMirror*>(
                    retained_converted_event_storage.state_ptr(i));
        }
    }
    ASSERT_NE(retained_midi_probe, nullptr);
    ASSERT_NE(retained_trigger_probe, nullptr);
    ASSERT_NE(retained_sliced_trigger_probe, nullptr);

    retained_converted_event.compiled_graph->root_operations.tick_block(
        retained_converted_event_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(retained_midi_probe->calls, 1u);
    EXPECT_EQ(retained_midi_probe->event_counts[0], 3u);
    EXPECT_EQ(retained_midi_probe->midi_counts[0], 3u);
    EXPECT_EQ(retained_midi_probe->first_times[0], 5u);
    EXPECT_EQ(retained_midi_probe->last_times[0], 17u);
    EXPECT_EQ(retained_trigger_probe->calls, 1u);
    EXPECT_EQ(retained_trigger_probe->event_count, 2u);
    EXPECT_EQ(retained_trigger_probe->trigger_count, 2u);
    EXPECT_EQ(retained_trigger_probe->first_time, 5u);
    EXPECT_EQ(retained_trigger_probe->last_time, 17u);
    ASSERT_EQ(retained_sliced_trigger_probe->calls, 2u);
    EXPECT_EQ(retained_sliced_trigger_probe->indices[0], 0u);
    EXPECT_EQ(retained_sliced_trigger_probe->indices[1], 32u);
    EXPECT_EQ(retained_sliced_trigger_probe->event_counts[0], 2u);
    EXPECT_EQ(retained_sliced_trigger_probe->event_counts[1], 0u);

    retained_converted_event.compiled_graph->root_operations.tick_block(
        retained_converted_event_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    ASSERT_EQ(retained_midi_probe->calls, 2u);
    EXPECT_EQ(retained_midi_probe->event_counts[1], 6u);
    EXPECT_EQ(retained_midi_probe->midi_counts[1], 6u);
    EXPECT_EQ(retained_midi_probe->first_times[1], 5u);
    EXPECT_EQ(retained_midi_probe->last_times[1], 81u);
    EXPECT_EQ(retained_trigger_probe->calls, 2u);
    EXPECT_EQ(retained_trigger_probe->event_count, 2u);
    EXPECT_EQ(retained_trigger_probe->trigger_count, 2u);
    EXPECT_EQ(retained_trigger_probe->first_time, 69u);
    EXPECT_EQ(retained_trigger_probe->last_time, 81u);
    ASSERT_EQ(retained_sliced_trigger_probe->calls, 4u);
    EXPECT_EQ(retained_sliced_trigger_probe->indices[2], 64u);
    EXPECT_EQ(retained_sliced_trigger_probe->indices[3], 96u);
    EXPECT_EQ(retained_sliced_trigger_probe->event_counts[2], 2u);
    EXPECT_EQ(retained_sliced_trigger_probe->event_counts[3], 0u);
    auto const* overflow_count = reinterpret_cast<std::uint64_t const*>(
        retained_converted_event_storage.buffer().data()
        + retained_converted_telemetry_region->storage_offset);
    EXPECT_EQ(*overflow_count, 0u);

}

} // namespace iv::test::graph_jit_tests
