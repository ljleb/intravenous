#include "test_support.h"

namespace iv::test::graph_jit_tests {

namespace {

constexpr char graph_jit_event_feedback_burst_a_id[] = "iv.test.graph_jit.state_context.event_feedback_burst_a";

constexpr char graph_jit_event_feedback_fanout_a_id[] = "iv.test.graph_jit.state_context.event_feedback_fanout_a";

struct EventFeedbackAStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t scc_feedback_latency = 0;
    std::array<std::uint64_t, 24> indices{};
    std::array<std::uint64_t, 24> block_sizes{};
    std::array<std::uint64_t, 24> input_counts{};
    std::array<std::uint64_t, 24> first_input_times{};
    std::uint32_t marker = 0;
};

struct RetainedDualEventFeedbackStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t scc_feedback_latency = 0;
    std::array<std::uint64_t, 24> indices{};
    std::array<std::uint64_t, 24> block_sizes{};
    std::array<std::uint64_t, 24> exact_counts{};
    std::array<std::uint64_t, 24> exact_first_times{};
    std::array<std::uint64_t, 24> exact_last_times{};
    std::array<std::uint64_t, 24> converted_counts{};
    std::array<std::uint64_t, 24> converted_first_times{};
    std::array<std::uint64_t, 24> converted_last_times{};
    std::uint64_t marker = 0;
};

std::shared_ptr<iv::ConfiguredGraph const>
configured_event_feedback_scc_external_fanout_graph(
    iv::PackageRevision const& revision,
    std::string_view observer_definition =
        "iv.test.graph_jit.state_context.trigger_event_consumer",
    std::string_view first_definition = graph_jit_event_feedback_a_id,
    std::string_view second_definition = graph_jit_event_feedback_b_id)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit event-feedback SCC-fanout builder session");
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
        graph, first_definition, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, second_definition, std::nullopt, {});
    auto observer = iv::details::configure_package_definition_provider(
        graph, observer_definition, std::nullopt, {});
    first.connect_event_input(0, second.event_port());
    second.connect_event_input(0, first.event_port().detach(10));
    observer.connect_event_input(0, first.event_port());
    graph.outputs();
    // Its regions are located by migration identity, which needs node identities.
    return with_node_identities(std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get())));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_mixed_event_feedback_graph(iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create mixed event-feedback builder session");
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
    auto ingress = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto first = iv::details::configure_package_definition_provider(
        graph, graph_jit_event_feedback_a_id, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, graph_jit_event_feedback_b_id, std::nullopt, {});
    first.connect_event_input(0, second.event_port());
    std::array sources{
        first.event_port().detach(10),
        ingress.event_port(),
    };
    second.connect_event_input(0, iv::EventPortRef(graph, sources));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_retained_converted_event_feedback_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create retained converted event-feedback builder session");
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
        graph, graph_jit_retained_dual_event_feedback_a_id, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, graph_jit_dual_event_feedback_b_id, std::nullopt, {});
    first.connect_event_input(0, second.event_port(0));
    first.connect_event_input(1, second.event_port(1));
    second.connect_event_input(0, first.event_port().detach(10));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const> configured_event_feedback_fanout_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit event-feedback fanout builder session");
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
    auto source = iv::details::configure_package_definition_provider(
        graph, graph_jit_event_feedback_fanout_a_id, std::nullopt, {});
    auto first = iv::details::configure_package_definition_provider(
        graph, graph_jit_event_feedback_b_id, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, graph_jit_event_feedback_b_id, std::nullopt, {});
    source.connect_event_input(0, first.event_port());
    source.connect_event_input(1, second.event_port());
    auto detached = source.event_port().detach(10);
    first.connect_event_input(0, detached);
    second.connect_event_input(0, detached);
    graph.outputs();
    // Its regions are located by migration identity, which needs node identities.
    return with_node_identities(std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get())));
}

} // namespace

TEST_F(GraphJitRuntimeFixture, ExactTypeEventDetachFeedback)
{
    // Its nodes have identities, so their State and storage migrate.
    auto feedback_graph = configured_event_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const detached = std::ranges::find_if(
        analysis->event_connections,
        [](iv::graph_jit::detail::EventConnectionPlan const& connection) { return connection.detach.has_value(); });
    ASSERT_NE(detached, analysis->event_connections.end());
    ASSERT_TRUE(detached->detach.has_value());
    EXPECT_EQ(detached->detach->loop_extra_latency, 10u);
    ASSERT_TRUE(detached->detach_region.has_value());
    ASSERT_LT(*detached->detach_region, analysis->schedule.regions.size());
    auto const& region = analysis->schedule.regions[*detached->detach_region];
    ASSERT_TRUE(region.cyclic);
    EXPECT_EQ(region.maximum_block_size, 8u);
    EXPECT_EQ(region.scc_feedback_latency, 8u);

    auto compiled = compile_graph(feedback_graph, 124);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    EventFeedbackAStateMirror* state_a = nullptr;
    EventFeedbackBStateMirror* state_b = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            compiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(EventFeedbackAStateMirror)) {
            ASSERT_EQ(state_a, nullptr);
            state_a = static_cast<EventFeedbackAStateMirror*>(storage.state_ptr(i));
        } else if (state_size == sizeof(EventFeedbackBStateMirror)) {
            ASSERT_EQ(state_b, nullptr);
            state_b = static_cast<EventFeedbackBStateMirror*>(storage.state_ptr(i));
        }
    }
    ASSERT_NE(state_a, nullptr);
    ASSERT_NE(state_b, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);

    ASSERT_EQ(state_a->calls, 8u);
    ASSERT_EQ(state_b->calls, 8u);
    EXPECT_EQ(state_a->scc_feedback_latency, 8u);
    EXPECT_EQ(state_b->scc_feedback_latency, 8u);
    EXPECT_EQ(state_a->marker, 0xa11ce001u);
    EXPECT_EQ(state_b->marker, 0xb22ce002ull);

    for (std::size_t slice = 0; slice < 8; ++slice) {
        auto const index = slice * 8u;
        EXPECT_EQ(state_a->indices[slice], index);
        EXPECT_EQ(state_b->indices[slice], index);
        EXPECT_EQ(state_a->block_sizes[slice], 8u);
        EXPECT_EQ(state_b->block_sizes[slice], 8u);

        // B runs before A in the explicit same-slice order. B publishes at +2,
        // so A observes that event directly in the current SCC slice.
        EXPECT_EQ(state_a->input_counts[slice], 1u);
        EXPECT_EQ(state_a->first_input_times[slice], index + 2u);

        // A's output is detached by exactly 10 samples. The first event lands at
        // time 11 and therefore first becomes visible to B in slice [8, 16).
        if (slice == 0) {
            EXPECT_EQ(state_b->input_counts[slice], 0u);
            EXPECT_EQ(state_b->first_input_times[slice], 0u);
        } else {
            EXPECT_EQ(state_b->input_counts[slice], 1u);
            EXPECT_EQ(state_b->first_input_times[slice], (slice - 1u) * 8u + 11u);
        }
    }

    // The last event emitted by A in the first root call is at 57. Detach adds
    // 10, so retained feedback storage must carry time 67 across both the
    // root boundary and a graph-generation migration. This also proves
    // transport latency (10) is independent of the SCC scheduling quantum (8).
    auto const calls_before = state_a->calls;
    ASSERT_EQ(state_b->calls, calls_before);
    ASSERT_LT(calls_before, 24u);
    auto recompiled = compile_graph(feedback_graph, 125);
    ASSERT_TRUE(recompiled.succeeded())
        << (recompiled.diagnostics.empty()
                ? ""
                : recompiled.diagnostics.front().message);
    auto migrated_storage =
        recompiled.compiled_graph->node_layout.create_storage(resources);
    migrated_storage.initialize(&storage);
    EventFeedbackAStateMirror* migrated_a = nullptr;
    EventFeedbackBStateMirror* migrated_b = nullptr;
    for (std::size_t i = 0;
         i < recompiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            recompiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(EventFeedbackAStateMirror)) {
            migrated_a = static_cast<EventFeedbackAStateMirror*>(
                migrated_storage.state_ptr(i));
        } else if (state_size == sizeof(EventFeedbackBStateMirror)) {
            migrated_b = static_cast<EventFeedbackBStateMirror*>(
                migrated_storage.state_ptr(i));
        }
    }
    ASSERT_NE(migrated_a, nullptr);
    ASSERT_NE(migrated_b, nullptr);
    recompiled.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(64), 8);
    // Both nodes keep their State: this call follows their earlier ones.
    ASSERT_EQ(migrated_a->calls, calls_before + 1u);
    ASSERT_EQ(migrated_b->calls, calls_before + 1u);
    EXPECT_EQ(migrated_a->indices[calls_before], 64u);
    EXPECT_EQ(migrated_b->indices[calls_before], 64u);
    EXPECT_EQ(migrated_a->block_sizes[calls_before], 8u);
    EXPECT_EQ(migrated_b->block_sizes[calls_before], 8u);
    EXPECT_EQ(migrated_a->input_counts[calls_before], 1u);
    EXPECT_EQ(migrated_a->first_input_times[calls_before], 66u);
    EXPECT_EQ(migrated_b->input_counts[calls_before], 1u);
    EXPECT_EQ(migrated_b->first_input_times[calls_before], 67u);

    // Legal root block sizes smaller than the SCC quantum must preserve exact
    // absolute event time across changing invocation boundaries.
    auto tail_storage =
        compiled.compiled_graph->node_layout.create_storage(resources);
    tail_storage.initialize();
    EventFeedbackAStateMirror* tail_a = nullptr;
    EventFeedbackBStateMirror* tail_b = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            compiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(EventFeedbackAStateMirror)) {
            tail_a = static_cast<EventFeedbackAStateMirror*>(
                tail_storage.state_ptr(i));
        } else if (state_size == sizeof(EventFeedbackBStateMirror)) {
            tail_b = static_cast<EventFeedbackBStateMirror*>(
                tail_storage.state_ptr(i));
        }
    }
    ASSERT_NE(tail_a, nullptr);
    ASSERT_NE(tail_b, nullptr);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), &empty_tick_invocation, index(4), 2);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), &empty_tick_invocation, index(6), 1);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), &empty_tick_invocation, index(7), 8);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), &empty_tick_invocation, index(15), 1);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), &empty_tick_invocation, index(16), 2);

    std::array<std::uint64_t, 6> const expected_indices{0, 4, 6, 7, 15, 16};
    std::array<std::uint64_t, 6> const expected_sizes{4, 2, 1, 8, 1, 2};
    std::array<std::uint64_t, 6> const expected_a_times{2, 5, 6, 9, 15, 17};
    std::array<std::uint64_t, 6> const expected_b_counts{0, 0, 0, 1, 1, 1};
    std::array<std::uint64_t, 6> const expected_b_times{0, 0, 0, 11, 15, 16};
    ASSERT_EQ(tail_a->calls, 6u);
    ASSERT_EQ(tail_b->calls, 6u);
    EXPECT_EQ(tail_a->scc_feedback_latency, 8u);
    EXPECT_EQ(tail_b->scc_feedback_latency, 8u);
    for (std::size_t i = 0; i < expected_indices.size(); ++i) {
        EXPECT_EQ(tail_a->indices[i], expected_indices[i]);
        EXPECT_EQ(tail_b->indices[i], expected_indices[i]);
        EXPECT_EQ(tail_a->block_sizes[i], expected_sizes[i]);
        EXPECT_EQ(tail_b->block_sizes[i], expected_sizes[i]);
        EXPECT_EQ(tail_a->input_counts[i], 1u);
        EXPECT_EQ(tail_a->first_input_times[i], expected_a_times[i]);
        EXPECT_EQ(tail_b->input_counts[i], expected_b_counts[i]);
        EXPECT_EQ(tail_b->first_input_times[i], expected_b_times[i]);
    }
}

TEST_F(GraphJitRuntimeFixture, MixedEventDetachComposesOrdinaryAndDelayedSources)
{
    auto graph = configured_mixed_event_feedback_graph(*revision);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(*graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const mixed = std::ranges::find_if(
        analysis->event_connections,
        [](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            return connection.requires_source_composition;
        });
    ASSERT_NE(mixed, analysis->event_connections.end());
    ASSERT_EQ(mixed->source_plans.size(), 2u);
    ASSERT_TRUE(mixed->source_plans[0].detach.has_value());
    EXPECT_FALSE(mixed->source_plans[1].detach.has_value());

    auto compiled = compile_graph(graph, 250);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventFeedbackBStateMirror* state = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventFeedbackBStateMirror)) {
            ASSERT_EQ(state, nullptr);
            state = static_cast<EventFeedbackBStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(state->calls, 8u);
    EXPECT_EQ(state->scc_feedback_latency, 8u);
    for (std::size_t slice = 0; slice < 8; ++slice) {
        auto const slice_index = slice * 8u;
        EXPECT_EQ(state->indices[slice], slice_index);
        EXPECT_EQ(state->block_sizes[slice], 8u);
        if (slice == 0) {
            EXPECT_EQ(state->input_counts[slice], 1u);
            EXPECT_EQ(state->first_input_times[slice], 3u);
        } else if (slice == 7) {
            EXPECT_EQ(state->input_counts[slice], 2u);
            EXPECT_EQ(state->first_input_times[slice], 59u);
        } else {
            EXPECT_EQ(state->input_counts[slice], 1u);
            EXPECT_EQ(
                state->first_input_times[slice],
                (slice - 1u) * 8u + 11u);
        }
    }

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 8);
    ASSERT_EQ(state->calls, 9u);
    EXPECT_EQ(state->indices[8], 64u);
    EXPECT_EQ(state->block_sizes[8], 8u);
    EXPECT_EQ(state->input_counts[8], 3u);
    EXPECT_EQ(state->first_input_times[8], 67u);
}

TEST_F(GraphJitRuntimeFixture, EventFanInStagesAcyclicIngressIntoFeedbackScc)
{
    auto graph = configured_staged_event_fan_in_graph(*revision);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const mixed = std::ranges::find_if(
        analysis->event_connections,
        [](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            return !connection.detach && connection.sources.size() == 2;
        });
    ASSERT_NE(mixed, analysis->event_connections.end());
    auto const first_region = analysis->schedule.bundle_to_region[
        mixed->sources[0].bundle];
    auto const second_region = analysis->schedule.bundle_to_region[
        mixed->sources[1].bundle];
    ASSERT_TRUE(first_region.has_value());
    ASSERT_TRUE(second_region.has_value());
    EXPECT_NE(*first_region, *second_region);
    EXPECT_TRUE(analysis->schedule.regions[*first_region].cyclic);
    EXPECT_FALSE(analysis->schedule.regions[*second_region].cyclic);

    auto compiled = compile_graph(graph, 214);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    EXPECT_TRUE(std::ranges::any_of(
        compiled.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.migration_identity.find("kind=ordered_compact_carry")
                != std::string::npos;
        }));
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventFeedbackAStateMirror* state = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventFeedbackAStateMirror)) {
            ASSERT_EQ(state, nullptr);
            state = static_cast<EventFeedbackAStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(state->calls, 8u);
    for (std::size_t slice = 0; slice < 8; ++slice) {
        auto const index = slice * 8u;
        EXPECT_EQ(state->indices[slice], index);
        EXPECT_EQ(state->block_sizes[slice], 8u);
        auto const expected_count = slice == 0
            ? 2u
            : (slice == 1 || slice == 7 ? 3u : 2u);
        EXPECT_EQ(state->input_counts[slice], expected_count);
        EXPECT_EQ(
            state->first_input_times[slice],
            slice == 0 ? 2u : index - 6u);
    }

    // The mixed fan-in's ordered compact carry restores B's event at 58 and
    // ingress event at 63. The next SCC slice inserts B's new event at 66 ahead
    // of the ingress source's 67/71 events without losing semantic tie order.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 8);
    ASSERT_EQ(state->calls, 9u);
    EXPECT_EQ(state->indices[8], 64u);
    EXPECT_EQ(state->block_sizes[8], 8u);
    EXPECT_EQ(state->input_counts[8], 5u);
    EXPECT_EQ(state->first_input_times[8], 58u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccConsumesRetainedAndConvertedHistoryPerSlice)
{
    auto feedback_graph = configured_retained_converted_event_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());

    std::size_t retained_inside_connections = 0;
    bool saw_exact = false;
    bool saw_converted = false;
    for (auto const& connection : analysis->event_connections) {
        if (connection.detach || connection.target_history != 8
            || connection.targets.empty()) {
            continue;
        }
        auto const target = connection.targets.front().bundle;
        ASSERT_LT(target, analysis->schedule.bundle_to_region.size());
        ASSERT_TRUE(analysis->schedule.bundle_to_region[target].has_value());
        auto const region = *analysis->schedule.bundle_to_region[target];
        ASSERT_LT(region, analysis->schedule.regions.size());
        if (!analysis->schedule.regions[region].cyclic) continue;
        ++retained_inside_connections;
        if (connection.source_type == iv::EventTypeId::trigger
            && connection.target_type == iv::EventTypeId::trigger
            && !connection.requires_conversion) {
            saw_exact = true;
        }
        if (connection.source_type == iv::EventTypeId::boundary
            && connection.target_type == iv::EventTypeId::trigger
            && connection.requires_conversion) {
            ASSERT_EQ(connection.conversion.step_count, 1u);
            EXPECT_EQ(
                connection.conversion.steps[0],
                iv::EventConversionStepId::boundary_to_trigger);
            saw_converted = true;
        }
    }
    EXPECT_EQ(retained_inside_connections, 2u);
    EXPECT_TRUE(saw_exact);
    EXPECT_TRUE(saw_converted);

    auto compiled = compile_graph(feedback_graph, 156);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    RetainedDualEventFeedbackStateMirror* state = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(RetainedDualEventFeedbackStateMirror)) {
            ASSERT_EQ(state, nullptr);
            state = static_cast<RetainedDualEventFeedbackStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(state->calls, 8u);
    EXPECT_EQ(state->scc_feedback_latency, 8u);
    EXPECT_EQ(state->marker, 0xa11ce808u);
    for (std::size_t slice = 0; slice < 8; ++slice) {
        auto const index = slice * 8u;
        EXPECT_EQ(state->indices[slice], index);
        EXPECT_EQ(state->block_sizes[slice], 8u);
        auto const expected_count = slice == 0 ? 1u : 2u;
        auto const expected_first = slice == 0 ? 2u : index - 6u;
        auto const expected_last = index + 2u;
        EXPECT_EQ(state->exact_counts[slice], expected_count);
        EXPECT_EQ(state->converted_counts[slice], expected_count);
        EXPECT_EQ(state->exact_first_times[slice], expected_first);
        EXPECT_EQ(state->converted_first_times[slice], expected_first);
        EXPECT_EQ(state->exact_last_times[slice], expected_last);
        EXPECT_EQ(state->converted_last_times[slice], expected_last);
    }

    // Carry restore happens once at SCC entry. The first slice of the next root
    // call therefore sees the prior event at 58 as history together with the
    // newly authored event at 66, for both exact and converted branches.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 8);
    ASSERT_EQ(state->calls, 9u);
    EXPECT_EQ(state->indices[8], 64u);
    EXPECT_EQ(state->block_sizes[8], 8u);
    EXPECT_EQ(state->exact_counts[8], 2u);
    EXPECT_EQ(state->converted_counts[8], 2u);
    EXPECT_EQ(state->exact_first_times[8], 58u);
    EXPECT_EQ(state->converted_first_times[8], 58u);
    EXPECT_EQ(state->exact_last_times[8], 66u);
    EXPECT_EQ(state->converted_last_times[8], 66u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccFansOutToAcyclicConsumer)
{
    auto feedback_graph =
        configured_event_feedback_scc_external_fanout_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [&](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            if (connection.detach || connection.sources.empty()
                || connection.targets.empty()) {
                return false;
            }
            auto const source = connection.sources.front().bundle;
            auto const target = connection.targets.front().bundle;
            if (source >= analysis->schedule.bundle_to_region.size()
                || target >= analysis->schedule.bundle_to_region.size()
                || !analysis->schedule.bundle_to_region[source]
                || !analysis->schedule.bundle_to_region[target]) {
                return false;
            }
            auto const source_region =
                *analysis->schedule.bundle_to_region[source];
            auto const target_region =
                *analysis->schedule.bundle_to_region[target];
            return source_region < analysis->schedule.regions.size()
                && target_region < analysis->schedule.regions.size()
                && analysis->schedule.regions[source_region].cyclic
                && !analysis->schedule.regions[target_region].cyclic;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_TRUE(fanout->requires_block_materialization);

    auto compiled = compile_graph(feedback_graph, 142);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventConsumerProbeStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(observer->calls, 1u);
    EXPECT_EQ(observer->last_index, 0u);
    EXPECT_EQ(observer->last_block_size, 64u);
    EXPECT_EQ(observer->event_count, 8u);
    EXPECT_EQ(observer->trigger_count, 8u);
    EXPECT_EQ(observer->first_time, 1u);
    EXPECT_EQ(observer->last_time, 57u);
    EXPECT_EQ(observer->marker, 0xe71e17u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 8);
    EXPECT_EQ(observer->calls, 2u);
    EXPECT_EQ(observer->last_index, 64u);
    EXPECT_EQ(observer->last_block_size, 8u);
    EXPECT_EQ(observer->event_count, 1u);
    EXPECT_EQ(observer->trigger_count, 1u);
    EXPECT_EQ(observer->first_time, 65u);
    EXPECT_EQ(observer->last_time, 65u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccRetainsUnfinalLatencyEvents)
{
    auto feedback_graph = configured_event_feedback_scc_external_fanout_graph(
        *revision,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        graph_jit_latent_event_feedback_a_id,
        graph_jit_event_feedback_b_id);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());

    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [&](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            if (connection.detach || connection.sources.empty()
                || connection.targets.empty()) {
                return false;
            }
            auto const source = connection.sources.front().bundle;
            auto const target = connection.targets.front().bundle;
            if (source >= analysis->schedule.bundle_to_region.size()
                || target >= analysis->schedule.bundle_to_region.size()
                || !analysis->schedule.bundle_to_region[source]
                || !analysis->schedule.bundle_to_region[target]) {
                return false;
            }
            auto const source_region =
                *analysis->schedule.bundle_to_region[source];
            auto const target_region =
                *analysis->schedule.bundle_to_region[target];
            return source_region < analysis->schedule.regions.size()
                && target_region < analysis->schedule.regions.size()
                && analysis->schedule.regions[source_region].cyclic
                && !analysis->schedule.regions[target_region].cyclic;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_EQ(fanout->source_history, 0u);
    EXPECT_EQ(fanout->source_latency, 16u);
    EXPECT_EQ(fanout->target_history, 0u);
    EXPECT_TRUE(fanout->requires_block_materialization);

    auto const detached = std::ranges::find_if(
        analysis->event_connections,
        [](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            return connection.detach.has_value();
        });
    ASSERT_NE(detached, analysis->event_connections.end());
    EXPECT_EQ(detached->source_history, 0u);
    EXPECT_EQ(detached->source_latency, 16u);
    EXPECT_EQ(detached->target_history, 0u);

    auto const fanout_index = static_cast<std::size_t>(
        std::distance(analysis->event_connections.begin(), fanout));
    auto const group = std::ranges::find_if(
        analysis->event_producer_groups,
        [&](iv::graph_jit::detail::EventProducerGroupPlan const& candidate) {
            return std::ranges::find(
                       candidate.connection_indices, fanout_index)
                != candidate.connection_indices.end();
        });
    ASSERT_NE(group, analysis->event_producer_groups.end());
    ASSERT_TRUE(group->storage_plan.has_value());
    EXPECT_EQ(
        group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(group->storage_requirements.retained_window_samples, 16u);

    auto compiled = compile_graph(feedback_graph, 151);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventConsumerProbeStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    std::optional<iv::NodeLayout::RegionHandle> feedback_carry;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.regions.size(); ++i) {
        auto const& region = compiled.compiled_graph->node_layout.regions[i];
        if (region.kind == iv::NodeLayout::Region::Kind::raw
            && region.migration_identity.starts_with(
                "graphjit.event.feedback:")) {
            ASSERT_FALSE(feedback_carry.has_value());
            EXPECT_NE(
                region.migration_identity.find("kind=compact_carry"),
                std::string::npos);
            feedback_carry = iv::NodeLayout::RegionHandle{.index = i};
        }
    }
    ASSERT_TRUE(feedback_carry.has_value());
    auto const feedback_carry_count = [&]() {
        auto const bytes = storage.region_bytes(*feedback_carry);
        EXPECT_GE(bytes.size(), sizeof(std::size_t));
        std::size_t value = 0;
        if (bytes.size() >= sizeof(std::size_t)) {
            std::memcpy(&value, bytes.data(), sizeof(value));
        }
        return value;
    };

    // The eight SCC slices author -7,1,...,49; their consumers read them at
    // 9,17,...,65 (latency 16). The detach copies each event when it becomes
    // final, at the slice that contains 9,17,...: the root call delivers 9 to
    // 57, 10 samples later. Only 67 is live across the root boundary; 65 is not
    // final until the next root call, whose first slice delivers it as 75.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(observer->calls, 1u);
    EXPECT_EQ(observer->last_index, 0u);
    EXPECT_EQ(observer->last_block_size, 64u);
    EXPECT_EQ(observer->event_count, 7u);
    EXPECT_EQ(observer->trigger_count, 7u);
    EXPECT_EQ(observer->first_time, 9u);
    EXPECT_EQ(observer->last_time, 57u);
    EXPECT_EQ(feedback_carry_count(), 1u);

    // Source carry restore brings back the event read at 65, which was not
    // final yet. New SCC slices then add 73,81,...,129. Feedback restore
    // independently brings back the delayed 67, and the first slice delivers
    // 65 as 75, once.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(observer->calls, 2u);
    EXPECT_EQ(observer->last_index, 64u);
    EXPECT_EQ(observer->last_block_size, 64u);
    EXPECT_EQ(observer->event_count, 8u);
    EXPECT_EQ(observer->trigger_count, 8u);
    EXPECT_EQ(observer->first_time, 65u);
    EXPECT_EQ(observer->last_time, 121u);
    EXPECT_EQ(feedback_carry_count(), 1u);

    // A short root call restores source event 129 and authors 137. The delayed
    // feedback carry retains exactly the one delivered event live beyond the
    // new root boundary: 139, from 129.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(128), 8);
    EXPECT_EQ(observer->calls, 3u);
    EXPECT_EQ(observer->last_index, 128u);
    EXPECT_EQ(observer->last_block_size, 8u);
    EXPECT_EQ(observer->event_count, 1u);
    EXPECT_EQ(observer->trigger_count, 1u);
    EXPECT_EQ(observer->first_time, 129u);
    EXPECT_EQ(observer->last_time, 129u);
    EXPECT_EQ(feedback_carry_count(), 1u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccPersistentRingRetainsUnfinalLatencyEvents)
{
    auto feedback_graph = configured_event_feedback_scc_external_fanout_graph(
        *revision,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        graph_jit_persistent_latent_event_feedback_a_id,
        graph_jit_event_feedback_b_id);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());

    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [&](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            if (connection.detach || connection.sources.empty()
                || connection.targets.empty()) {
                return false;
            }
            auto const source = connection.sources.front().bundle;
            auto const target = connection.targets.front().bundle;
            if (source >= analysis->schedule.bundle_to_region.size()
                || target >= analysis->schedule.bundle_to_region.size()
                || !analysis->schedule.bundle_to_region[source]
                || !analysis->schedule.bundle_to_region[target]) {
                return false;
            }
            auto const source_region =
                *analysis->schedule.bundle_to_region[source];
            auto const target_region =
                *analysis->schedule.bundle_to_region[target];
            return source_region < analysis->schedule.regions.size()
                && target_region < analysis->schedule.regions.size()
                && analysis->schedule.regions[source_region].cyclic
                && !analysis->schedule.regions[target_region].cyclic;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_EQ(fanout->source_history, 0u);
    EXPECT_EQ(fanout->source_latency, 320u);
    EXPECT_EQ(fanout->target_history, 0u);
    EXPECT_TRUE(fanout->requires_block_materialization);

    auto const fanout_index = static_cast<std::size_t>(
        std::distance(analysis->event_connections.begin(), fanout));
    auto const group = std::ranges::find_if(
        analysis->event_producer_groups,
        [&](iv::graph_jit::detail::EventProducerGroupPlan const& candidate) {
            return std::ranges::find(
                       candidate.connection_indices, fanout_index)
                != candidate.connection_indices.end();
        });
    ASSERT_NE(group, analysis->event_producer_groups.end());
    ASSERT_TRUE(group->storage_plan.has_value());
    EXPECT_EQ(
        group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(group->storage_requirements.retained_window_samples, 320u);
    EXPECT_GE(
        group->storage_requirements.retained_event_capacity,
        group->storage_requirements.current_event_capacity);

    auto compiled = compile_graph(feedback_graph, 152);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventConsumerProbeStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    std::optional<iv::NodeLayout::RegionHandle> source_ring;
    std::optional<iv::NodeLayout::RegionHandle> feedback_ring;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.regions.size(); ++i) {
        auto const& region = compiled.compiled_graph->node_layout.regions[i];
        if (region.kind != iv::NodeLayout::Region::Kind::raw) continue;
        if (region.migration_identity.starts_with("graphjit.event.feedback:")) {
            ASSERT_FALSE(feedback_ring.has_value());
            feedback_ring = iv::NodeLayout::RegionHandle{.index = i};
        } else if (region.migration_identity.find("kind=persistent_ring")
                       != std::string::npos
                   && region.migration_identity.find("latency=320")
                       != std::string::npos) {
            ASSERT_FALSE(source_ring.has_value());
            source_ring = iv::NodeLayout::RegionHandle{.index = i};
        }
    }
    ASSERT_TRUE(source_ring.has_value());
    ASSERT_TRUE(feedback_ring.has_value());

    auto const indices = [&](iv::NodeLayout::RegionHandle region) {
        auto const bytes = storage.region_bytes(region);
        EXPECT_GE(bytes.size(), 2 * sizeof(std::size_t));
        std::array<std::size_t, 2> value{};
        if (bytes.size() >= 2 * sizeof(std::size_t)) {
            std::memcpy(value.data(), bytes.data(), 2 * sizeof(std::size_t));
        }
        return value;
    };

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(observer->calls, 1u);
    EXPECT_EQ(observer->last_index, 0u);
    EXPECT_EQ(observer->last_block_size, 64u);
    EXPECT_EQ(observer->event_count, 7u);
    EXPECT_EQ(observer->trigger_count, 7u);
    EXPECT_EQ(observer->first_time, 9u);
    EXPECT_EQ(observer->last_time, 57u);
    EXPECT_EQ(indices(*source_ring)[0], 0u);
    EXPECT_EQ(indices(*source_ring)[1], 8u);
    // The detach copies each event when it becomes final: the event read at 65
    // is not final before the root call ends.
    EXPECT_EQ(indices(*feedback_ring)[1], 7u);

    // Root-entry pruning removes events before 64 but keeps the event read at
    // 65, which was not final yet. The first slice copies it into detached
    // feedback, once, with the seven events that become final in this call.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(observer->calls, 2u);
    EXPECT_EQ(observer->last_index, 64u);
    EXPECT_EQ(observer->last_block_size, 64u);
    EXPECT_EQ(observer->event_count, 8u);
    EXPECT_EQ(observer->trigger_count, 8u);
    EXPECT_EQ(observer->first_time, 65u);
    EXPECT_EQ(observer->last_time, 121u);
    EXPECT_EQ(indices(*source_ring)[0], 7u);
    EXPECT_EQ(indices(*source_ring)[1], 16u);
    EXPECT_EQ(indices(*feedback_ring)[1], 15u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(128), 8);
    EXPECT_EQ(observer->calls, 3u);
    EXPECT_EQ(observer->last_index, 128u);
    EXPECT_EQ(observer->last_block_size, 8u);
    EXPECT_EQ(observer->event_count, 1u);
    EXPECT_EQ(observer->trigger_count, 1u);
    EXPECT_EQ(observer->first_time, 129u);
    EXPECT_EQ(observer->last_time, 129u);
    EXPECT_EQ(indices(*source_ring)[0], 15u);
    EXPECT_EQ(indices(*source_ring)[1], 17u);
    EXPECT_EQ(indices(*feedback_ring)[1], 16u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccConvertsFanoutToAcyclicConsumer)
{
    auto feedback_graph = configured_event_feedback_scc_external_fanout_graph(
        *revision,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        graph_jit_boundary_event_feedback_a_id,
        graph_jit_boundary_event_feedback_b_id);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            return !connection.detach
                && connection.source_type == iv::EventTypeId::boundary
                && connection.target_type == iv::EventTypeId::trigger;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_TRUE(fanout->requires_conversion);
    EXPECT_TRUE(fanout->requires_block_materialization);
    ASSERT_EQ(fanout->conversion.step_count, 1u);
    EXPECT_EQ(
        fanout->conversion.steps[0],
        iv::EventConversionStepId::boundary_to_trigger);

    auto const group = std::ranges::find_if(
        analysis->event_producer_groups,
        [&](iv::graph_jit::detail::EventProducerGroupPlan const& candidate) {
            return std::ranges::find(
                       candidate.connection_indices,
                       static_cast<std::size_t>(
                           std::distance(
                               analysis->event_connections.begin(), fanout)))
                != candidate.connection_indices.end();
        });
    ASSERT_NE(group, analysis->event_producer_groups.end());
    ASSERT_TRUE(group->storage_plan.has_value());
    EXPECT_EQ(
        group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);

    auto compiled = compile_graph(feedback_graph, 148);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventConsumerProbeStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(observer->calls, 1u);
    EXPECT_EQ(observer->last_index, 0u);
    EXPECT_EQ(observer->last_block_size, 64u);
    EXPECT_EQ(observer->event_count, 8u);
    EXPECT_EQ(observer->trigger_count, 8u);
    EXPECT_EQ(observer->first_time, 1u);
    EXPECT_EQ(observer->last_time, 57u);
    EXPECT_EQ(observer->marker, 0xe71e17u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 8);
    EXPECT_EQ(observer->calls, 2u);
    EXPECT_EQ(observer->last_index, 64u);
    EXPECT_EQ(observer->last_block_size, 8u);
    EXPECT_EQ(observer->event_count, 1u);
    EXPECT_EQ(observer->trigger_count, 1u);
    EXPECT_EQ(observer->first_time, 65u);
    EXPECT_EQ(observer->last_time, 65u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccRetainsOutboundTargetHistory)
{
    auto feedback_graph = configured_event_feedback_scc_external_fanout_graph(
        *revision,
        "iv.test.graph_jit.state_context.retained_trigger_event_consumer");
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [&](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            if (connection.detach || connection.target_history != 8
                || connection.sources.empty() || connection.targets.empty()) {
                return false;
            }
            auto const source = connection.sources.front().bundle;
            auto const target = connection.targets.front().bundle;
            if (source >= analysis->schedule.bundle_to_region.size()
                || target >= analysis->schedule.bundle_to_region.size()
                || !analysis->schedule.bundle_to_region[source]
                || !analysis->schedule.bundle_to_region[target]) {
                return false;
            }
            auto const source_region =
                *analysis->schedule.bundle_to_region[source];
            auto const target_region =
                *analysis->schedule.bundle_to_region[target];
            return source_region < analysis->schedule.regions.size()
                && target_region < analysis->schedule.regions.size()
                && analysis->schedule.regions[source_region].cyclic
                && !analysis->schedule.regions[target_region].cyclic;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_EQ(fanout->source_history, 0u);
    EXPECT_EQ(fanout->source_latency, 0u);
    EXPECT_EQ(fanout->target_history, 8u);
    EXPECT_TRUE(fanout->requires_block_materialization);

    auto const fanout_index = static_cast<std::size_t>(
        std::distance(analysis->event_connections.begin(), fanout));
    auto const group = std::ranges::find_if(
        analysis->event_producer_groups,
        [&](iv::graph_jit::detail::EventProducerGroupPlan const& candidate) {
            return std::ranges::find(
                       candidate.connection_indices, fanout_index)
                != candidate.connection_indices.end();
        });
    ASSERT_NE(group, analysis->event_producer_groups.end());
    ASSERT_TRUE(group->storage_plan.has_value());
    EXPECT_EQ(
        group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(group->storage_requirements.retained_window_samples, 8u);

    auto compiled = compile_graph(feedback_graph, 149);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    RetainedEventConsumerProbeStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            compiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(RetainedEventConsumerProbeStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<RetainedEventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    std::optional<iv::NodeLayout::RegionHandle> feedback_carry;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.regions.size(); ++i) {
        auto const& region = compiled.compiled_graph->node_layout.regions[i];
        if (region.kind == iv::NodeLayout::Region::Kind::raw
            && region.migration_identity.starts_with(
                "graphjit.event.feedback:")) {
            ASSERT_FALSE(feedback_carry.has_value());
            EXPECT_NE(
                region.migration_identity.find("kind=compact_carry"),
                std::string::npos);
            feedback_carry = iv::NodeLayout::RegionHandle{.index = i};
        }
    }
    ASSERT_TRUE(feedback_carry.has_value());
    auto const feedback_carry_count = [&]() {
        auto const bytes = storage.region_bytes(*feedback_carry);
        EXPECT_GE(bytes.size(), sizeof(std::size_t));
        std::size_t value = 0;
        if (bytes.size() >= sizeof(std::size_t)) {
            std::memcpy(&value, bytes.data(), sizeof(value));
        }
        return value;
    };

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(observer->calls, 1u);
    EXPECT_EQ(observer->indices[0], 0u);
    EXPECT_EQ(observer->event_counts[0], 8u);
    EXPECT_EQ(observer->first_times[0], 1u);
    EXPECT_EQ(observer->second_times[0], 9u);
    EXPECT_EQ(observer->last_times[0], 57u);
    EXPECT_EQ(feedback_carry_count(), 1u);

    // The carry retains [56, 64), so event 57 becomes history for the next
    // root call. The SCC then appends eight current events before one root-exit
    // materialization selects [56, 128).
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    ASSERT_EQ(observer->calls, 2u);
    EXPECT_EQ(observer->indices[1], 64u);
    EXPECT_EQ(observer->event_counts[1], 9u);
    EXPECT_EQ(observer->first_times[1], 57u);
    EXPECT_EQ(observer->second_times[1], 65u);
    EXPECT_EQ(observer->last_times[1], 121u);

    // The restored event 57 is historical context, not newly authored output.
    // Detached feedback retains only the one delayed event that lies beyond
    // this root boundary; restored source history is never re-enqueued.
    EXPECT_EQ(feedback_carry_count(), 1u);

    // Changing the root-call size still uses the root history window, not an
    // SCC-slice-sized window. Only prior event 121 and current event 129 fit.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(128), 8);
    ASSERT_EQ(observer->calls, 3u);
    EXPECT_EQ(observer->indices[2], 128u);
    EXPECT_EQ(observer->event_counts[2], 2u);
    EXPECT_EQ(observer->first_times[2], 121u);
    EXPECT_EQ(observer->second_times[2], 129u);
    EXPECT_EQ(observer->last_times[2], 129u);
    EXPECT_EQ(feedback_carry_count(), 1u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccComposesLatencyHistoryAndConversion)
{
    auto feedback_graph = configured_event_feedback_scc_external_fanout_graph(
        *revision,
        "iv.test.graph_jit.state_context.persistent_event_feedback_consumer",
        graph_jit_persistent_latent_boundary_event_feedback_a_id,
        graph_jit_boundary_event_feedback_b_id);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());

    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [&](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            if (connection.detach || connection.sources.empty()
                || connection.targets.empty()) {
                return false;
            }
            auto const source = connection.sources.front().bundle;
            auto const target = connection.targets.front().bundle;
            if (source >= analysis->schedule.bundle_to_region.size()
                || target >= analysis->schedule.bundle_to_region.size()
                || !analysis->schedule.bundle_to_region[source]
                || !analysis->schedule.bundle_to_region[target]) {
                return false;
            }
            auto const source_region =
                *analysis->schedule.bundle_to_region[source];
            auto const target_region =
                *analysis->schedule.bundle_to_region[target];
            return source_region < analysis->schedule.regions.size()
                && target_region < analysis->schedule.regions.size()
                && analysis->schedule.regions[source_region].cyclic
                && !analysis->schedule.regions[target_region].cyclic;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_EQ(fanout->source_type, iv::EventTypeId::boundary);
    EXPECT_EQ(fanout->target_type, iv::EventTypeId::trigger);
    EXPECT_EQ(fanout->source_history, 0u);
    EXPECT_EQ(fanout->source_latency, 320u);
    EXPECT_EQ(fanout->target_history, 320u);
    EXPECT_TRUE(fanout->requires_conversion);
    EXPECT_TRUE(fanout->requires_block_materialization);
    ASSERT_EQ(fanout->conversion.step_count, 1u);
    EXPECT_EQ(
        fanout->conversion.steps[0],
        iv::EventConversionStepId::boundary_to_trigger);

    auto const fanout_index = static_cast<std::size_t>(
        std::distance(analysis->event_connections.begin(), fanout));
    auto const group = std::ranges::find_if(
        analysis->event_producer_groups,
        [&](iv::graph_jit::detail::EventProducerGroupPlan const& candidate) {
            return std::ranges::find(
                       candidate.connection_indices, fanout_index)
                != candidate.connection_indices.end();
        });
    ASSERT_NE(group, analysis->event_producer_groups.end());
    ASSERT_TRUE(group->storage_plan.has_value());
    EXPECT_EQ(
        group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    // The canonical producer timeline holds each event where it becomes final,
    // 320 positions after it was authored. It retains 320 positions of target
    // history behind the root window and the source's 320 latency positions,
    // which are not final yet, ahead of it.
    EXPECT_EQ(group->storage_requirements.retained_window_samples, 640u);
    EXPECT_GE(
        group->storage_requirements.retained_event_capacity,
        group->storage_requirements.current_event_capacity);

    auto compiled = compile_graph(feedback_graph, 153);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    PersistentEventFeedbackConsumerStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(PersistentEventFeedbackConsumerStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<PersistentEventFeedbackConsumerStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    std::optional<iv::NodeLayout::RegionHandle> source_ring;
    std::optional<iv::NodeLayout::RegionHandle> feedback_ring;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.regions.size(); ++i) {
        auto const& region = compiled.compiled_graph->node_layout.regions[i];
        if (region.kind != iv::NodeLayout::Region::Kind::raw) continue;
        if (region.migration_identity.starts_with("graphjit.event.feedback:")) {
            ASSERT_FALSE(feedback_ring.has_value());
            feedback_ring = iv::NodeLayout::RegionHandle{.index = i};
        } else if (region.migration_identity.find("kind=persistent_ring")
                       != std::string::npos
                   && region.migration_identity.find("latency=320")
                       != std::string::npos) {
            ASSERT_FALSE(source_ring.has_value());
            source_ring = iv::NodeLayout::RegionHandle{.index = i};
        }
    }
    ASSERT_TRUE(source_ring.has_value());
    ASSERT_TRUE(feedback_ring.has_value());

    auto const indices = [&](iv::NodeLayout::RegionHandle region) {
        auto const bytes = storage.region_bytes(region);
        EXPECT_GE(bytes.size(), 2 * sizeof(std::size_t));
        std::array<std::size_t, 2> value{};
        if (bytes.size() >= 2 * sizeof(std::size_t)) {
            std::memcpy(value.data(), bytes.data(), 2 * sizeof(std::size_t));
        }
        return value;
    };

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(observer->calls, 1u);
    EXPECT_EQ(observer->indices[0], 0u);
    EXPECT_EQ(observer->event_counts[0], 7u);
    EXPECT_EQ(observer->first_times[0], 9u);
    EXPECT_EQ(observer->last_times[0], 57u);
    EXPECT_EQ(observer->marker, 0xfeed320u);
    EXPECT_EQ(indices(*source_ring)[0], 0u);
    EXPECT_EQ(indices(*source_ring)[1], 8u);
    // The detach copies each event when it becomes final; the event read at 65
    // is not final before the root call ends.
    EXPECT_EQ(indices(*feedback_ring)[1], 7u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    ASSERT_EQ(observer->calls, 2u);
    EXPECT_EQ(observer->indices[1], 64u);
    EXPECT_EQ(observer->event_counts[1], 15u);
    EXPECT_EQ(observer->first_times[1], 9u);
    EXPECT_EQ(observer->last_times[1], 121u);
    EXPECT_EQ(indices(*source_ring)[0], 0u);
    EXPECT_EQ(indices(*source_ring)[1], 16u);
    EXPECT_EQ(indices(*feedback_ring)[1], 15u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(128), 64);
    ASSERT_EQ(observer->calls, 3u);
    EXPECT_EQ(observer->indices[2], 128u);
    EXPECT_EQ(observer->event_counts[2], 23u);
    EXPECT_EQ(observer->first_times[2], 9u);
    EXPECT_EQ(observer->last_times[2], 185u);
    EXPECT_EQ(indices(*source_ring)[0], 0u);
    EXPECT_EQ(indices(*source_ring)[1], 24u);
    EXPECT_EQ(indices(*feedback_ring)[1], 23u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccPersistentRingRetainsOutboundTargetHistory)
{
    auto feedback_graph = configured_event_feedback_scc_external_fanout_graph(
        *revision,
        "iv.test.graph_jit.state_context.persistent_event_feedback_consumer");
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [&](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            if (connection.detach || connection.target_history != 320
                || connection.sources.empty() || connection.targets.empty()) {
                return false;
            }
            auto const source = connection.sources.front().bundle;
            auto const target = connection.targets.front().bundle;
            if (source >= analysis->schedule.bundle_to_region.size()
                || target >= analysis->schedule.bundle_to_region.size()
                || !analysis->schedule.bundle_to_region[source]
                || !analysis->schedule.bundle_to_region[target]) {
                return false;
            }
            auto const source_region =
                *analysis->schedule.bundle_to_region[source];
            auto const target_region =
                *analysis->schedule.bundle_to_region[target];
            return source_region < analysis->schedule.regions.size()
                && target_region < analysis->schedule.regions.size()
                && analysis->schedule.regions[source_region].cyclic
                && !analysis->schedule.regions[target_region].cyclic;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_EQ(fanout->source_history, 0u);
    EXPECT_EQ(fanout->source_latency, 0u);
    EXPECT_EQ(fanout->target_history, 320u);
    EXPECT_TRUE(fanout->requires_block_materialization);

    auto const fanout_index = static_cast<std::size_t>(
        std::distance(analysis->event_connections.begin(), fanout));
    auto const group = std::ranges::find_if(
        analysis->event_producer_groups,
        [&](iv::graph_jit::detail::EventProducerGroupPlan const& candidate) {
            return std::ranges::find(
                       candidate.connection_indices, fanout_index)
                != candidate.connection_indices.end();
        });
    ASSERT_NE(group, analysis->event_producer_groups.end());
    ASSERT_TRUE(group->storage_plan.has_value());
    EXPECT_EQ(
        group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(group->storage_requirements.retained_window_samples, 320u);
    EXPECT_GE(
        group->storage_requirements.retained_event_capacity,
        group->storage_requirements.current_event_capacity);

    auto compiled = compile_graph(feedback_graph, 150);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    PersistentEventFeedbackConsumerStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            compiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(PersistentEventFeedbackConsumerStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<PersistentEventFeedbackConsumerStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    std::optional<iv::NodeLayout::RegionHandle> source_ring;
    std::optional<iv::NodeLayout::RegionHandle> feedback_carry;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.regions.size(); ++i) {
        auto const& region = compiled.compiled_graph->node_layout.regions[i];
        if (region.kind != iv::NodeLayout::Region::Kind::raw) continue;
        if (region.migration_identity.starts_with("graphjit.event.feedback:")) {
            ASSERT_FALSE(feedback_carry.has_value());
            EXPECT_NE(
                region.migration_identity.find("kind=compact_carry"),
                std::string::npos);
            feedback_carry = iv::NodeLayout::RegionHandle{.index = i};
        } else if (region.migration_identity.starts_with("graphjit.event:")
                   && region.migration_identity.find("kind=persistent_ring")
                       != std::string::npos) {
            ASSERT_FALSE(source_ring.has_value());
            source_ring = iv::NodeLayout::RegionHandle{.index = i};
        }
    }
    ASSERT_TRUE(source_ring.has_value());
    ASSERT_TRUE(feedback_carry.has_value());

    auto const write_index = [&](iv::NodeLayout::RegionHandle region) {
        auto const bytes = storage.region_bytes(region);
        EXPECT_GE(bytes.size(), 2 * sizeof(std::size_t));
        std::size_t value = 0;
        if (bytes.size() >= 2 * sizeof(std::size_t)) {
            std::memcpy(
                &value, bytes.data() + sizeof(std::size_t), sizeof(value));
        }
        return value;
    };
    auto const feedback_carry_count = [&]() {
        auto const bytes = storage.region_bytes(*feedback_carry);
        EXPECT_GE(bytes.size(), sizeof(std::size_t));
        std::size_t value = 0;
        if (bytes.size() >= sizeof(std::size_t)) {
            std::memcpy(&value, bytes.data(), sizeof(value));
        }
        return value;
    };

    for (std::size_t call = 0; call < 4; ++call) {
        auto const index = call * 64;
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(),
            &empty_tick_invocation,
            iv::GlobalIndex::from_bits(index),
            64);
        ASSERT_EQ(observer->calls, call + 1);
        EXPECT_EQ(observer->indices[call], index);
        EXPECT_EQ(observer->event_counts[call], (call + 1) * 8);
        EXPECT_EQ(observer->first_times[call], 1u);
        EXPECT_EQ(observer->last_times[call], index + 57);
        EXPECT_EQ(observer->marker, 0xfeed320u);

        // The canonical retained ring advances monotonically with authored
        // source events. Detached feedback stores only the one delayed event
        // crossing the current root boundary.
        EXPECT_EQ(write_index(*source_ring), (call + 1) * 8);
        EXPECT_EQ(feedback_carry_count(), 1u);
    }
}

TEST_F(GraphJitRuntimeFixture, EventDetachFeedbackFanoutSharesDelayedStream)
{
    auto feedback_graph = configured_event_feedback_fanout_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto const configured_events =
        feedback_graph->connections.configured_event_connections();
    EXPECT_EQ(
        std::ranges::count_if(
            configured_events,
            [](iv::ConfiguredEventConnection const& connection) {
                return std::ranges::any_of(
                    connection.sources,
                    [](auto const& source) {
                        return source.detach.has_value();
                    });
            }),
        2);

    auto compiled = compile_graph(feedback_graph, 126);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    EXPECT_EQ(
        std::ranges::count_if(
            compiled.compiled_graph->node_layout.regions,
            [](iv::NodeLayout::Region const& region) {
                return region.migration_identity.find(
                           "graphjit.event.feedback:")
                    != std::string::npos;
            }),
        1);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    std::vector<EventFeedbackBStateMirror*> consumers;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventFeedbackBStateMirror)) {
            consumers.push_back(static_cast<EventFeedbackBStateMirror*>(
                storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(consumers.size(), 2u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 24);
    for (auto const* state : consumers) {
        ASSERT_NE(state, nullptr);
        ASSERT_EQ(state->calls, 3u);
        EXPECT_EQ(state->scc_feedback_latency, 8u);
        EXPECT_EQ(state->indices[0], 0u);
        EXPECT_EQ(state->indices[1], 8u);
        EXPECT_EQ(state->indices[2], 16u);
        EXPECT_EQ(state->input_counts[0], 0u);
        EXPECT_EQ(state->input_counts[1], 1u);
        EXPECT_EQ(state->input_counts[2], 1u);
        EXPECT_EQ(state->first_input_times[1], 11u);
        EXPECT_EQ(state->first_input_times[2], 19u);
    }
}

TEST_F(GraphJitRuntimeFixture, EventDetachFeedbackRetainsBurstAcrossTinyBlocks)
{
    auto feedback_graph = configured_event_feedback_graph(
        *revision, graph_jit_event_feedback_burst_a_id);
    ASSERT_TRUE(feedback_graph);

    auto compiled = compile_graph(feedback_graph, 126);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const feedback_region = std::ranges::find_if(
        compiled.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.migration_identity.find(
                       "graphjit.event.feedback:")
                != std::string::npos;
        });
    ASSERT_NE(
        feedback_region,
        compiled.compiled_graph->node_layout.regions.end());
    EXPECT_NE(
        feedback_region->migration_identity.find("capacity=256"),
        std::string::npos);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventFeedbackBStateMirror* state_b = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventFeedbackBStateMirror)) {
            ASSERT_EQ(state_b, nullptr);
            state_b = static_cast<EventFeedbackBStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(state_b, nullptr);

    // Each one-sample root call publishes the declared 16-event/sample peak at
    // one timestamp. The exact ten-sample delayed live span therefore requires
    // 160 events, rounded to a 256-event compact carry.
    for (std::size_t index = 0; index < 15; ++index) {
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(),
            &empty_tick_invocation,
            iv::GlobalIndex::from_bits(index),
            1);
    }

    ASSERT_EQ(state_b->calls, 15u);
    EXPECT_EQ(state_b->scc_feedback_latency, 8u);
    for (std::size_t index = 0; index < 10; ++index) {
        EXPECT_EQ(state_b->input_counts[index], 0u);
    }
    for (std::size_t index = 10; index < 15; ++index) {
        EXPECT_EQ(state_b->input_counts[index], 16u);
        EXPECT_EQ(state_b->first_input_times[index], index);
    }
}

} // namespace iv::test::graph_jit_tests
