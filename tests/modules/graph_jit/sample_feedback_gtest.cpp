#include "test_support.h"

namespace iv::test::graph_jit_tests {

namespace {

constexpr char graph_jit_revising_sample_feedback_id[] = "iv.test.graph_jit.state_context.revising_sample_feedback";

struct SampleFeedbackAStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t scc_feedback_latency = 0;
    std::array<std::uint64_t, 24> indices{};
    std::array<std::uint64_t, 24> block_sizes{};
    std::array<float, 24> first_inputs{};
    std::array<float, 24> last_inputs{};
    std::uint32_t marker = 0;
};

struct SampleFeedbackBStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t scc_feedback_latency = 0;
    std::array<std::uint64_t, 24> indices{};
    std::array<std::uint64_t, 24> block_sizes{};
    std::array<float, 24> first_inputs{};
    std::array<float, 24> last_inputs{};
    std::uint64_t marker = 0;
    std::uint64_t distinct_padding = 0;
};

struct TemporalSampleFeedbackStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t scc_feedback_latency = 0;
    std::array<std::uint64_t, 24> indices{};
    std::array<std::uint64_t, 24> block_sizes{};
    std::array<float, 24> current_inputs{};
    std::array<float, 24> history_3_inputs{};
    std::array<float, 24> first_inputs{};
    std::array<float, 24> last_inputs{};
    std::uint32_t marker = 0;
};

struct RevisingSampleFeedbackStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 8> indices{};
    std::array<float, 8> first_inputs{};
    std::array<float, 8> last_inputs{};
    std::uint32_t marker = 0;
};

struct ProjectedRevisingSampleFeedbackStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 8> indices{};
    std::array<float, 8> first_left{};
    std::array<float, 8> first_right{};
    std::array<float, 8> last_left{};
    std::array<float, 8> last_right{};
    std::uint32_t marker = 0;
};

std::shared_ptr<iv::ConfiguredGraph const> configured_sample_feedback_graph(
    iv::PackageRevision const& revision,
    std::size_t latency = 6,
    iv::Sample initial_value = iv::Sample{-0.625f})
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit sample-feedback builder session");
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
        graph, graph_jit_sample_feedback_a_id, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, graph_jit_sample_feedback_b_id, std::nullopt, {});
    first(second);
    second(static_cast<iv::SamplePortRef>(first).detach(latency, initial_value));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const> configured_temporal_sample_feedback_graph(
    iv::PackageRevision const& revision,
    std::size_t latency = 6,
    iv::Sample initial_value = iv::Sample{-0.625f})
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit temporal sample-feedback builder session");
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
    auto node = iv::details::configure_package_definition_provider(
        graph, graph_jit_temporal_sample_feedback_id, std::nullopt, {});
    node(static_cast<iv::SamplePortRef>(node).detach(latency, initial_value));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const> configured_revising_sample_feedback_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit revising sample-feedback builder session");
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
    auto node = iv::details::configure_package_definition_provider(
        graph, graph_jit_revising_sample_feedback_id, std::nullopt, {});
    node(static_cast<iv::SamplePortRef>(node).detach(6, iv::Sample{-1.0f}));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_projected_revising_sample_feedback_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit projected revising sample-feedback builder session");
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

    iv::GraphBuilder builder(session.get());
    auto node = iv::details::configure_package_definition_provider(
        builder, graph_jit_projected_revising_sample_feedback_id, std::nullopt, {});
    node(static_cast<iv::SamplePortRef>(node).detach(6, iv::Sample{-1.0f}));
    builder.outputs();
    auto base = std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
    auto graph = std::make_shared<iv::ConfiguredGraph>(*base);

    std::vector<iv::ConfiguredSampleConnection> projected;
    bool split_detached = false;
    for (auto const& connection :
         base->connections.configured_sample_connections()) {
        if (!configured_sample_connection_is_detached(connection)) {
            projected.push_back(connection);
            continue;
        }
        if (split_detached
            || connection.source_channels.size() != 1
            || connection.target_channels.size() != 2) {
            throw std::runtime_error(
                "GraphJit projected revising sample-feedback fixture lost its detached connection shape");
        }
        split_detached = true;
        for (std::size_t target_index = 0;
             target_index < connection.target_channels.size(); ++target_index) {
            auto const target = connection.target_channels[target_index];
            if (target_index == 0) {
                projected.push_back(iv::ConfiguredSampleConnection{
                    .source_type = iv::ChannelTypeId::stereo,
                    .source_channels = {
                        connection.source_channels.front(),
                        connection.source_channels.front(),
                    },
                    .target_type = iv::ChannelTypeId::mono,
                    .target_channels = {target},
                });
            } else {
                projected.push_back(iv::ConfiguredSampleConnection{
                    .source_type = iv::ChannelTypeId::mono,
                    .source_channels = connection.source_channels,
                    .target_type = iv::ChannelTypeId::mono,
                    .target_channels = {target},
                });
            }
        }
    }
    if (!split_detached) {
        throw std::runtime_error(
            "GraphJit projected revising sample-feedback fixture lost its detach");
    }
    graph->connections = iv::GraphBuilderConnections::from_configured_connections(
        projected,
        base->connections.configured_event_connections());
    return graph;
}

std::shared_ptr<iv::ConfiguredGraph const> configured_converted_sample_feedback_graph(
    iv::PackageRevision const& revision,
    std::size_t latency = 6,
    iv::Sample initial_value = iv::Sample{-0.25f})
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit converted sample-feedback builder session");
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
    auto node = iv::details::configure_package_definition_provider(
        graph, graph_jit_converted_sample_feedback_id, std::nullopt, {});
    node(static_cast<iv::SamplePortRef>(node).detach(latency, initial_value));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const> configured_projected_sample_feedback_graph(
    iv::PackageRevision const& revision,
    std::size_t latency = 6,
    iv::Sample initial_value = iv::Sample{-0.25f})
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit projected sample-feedback builder session");
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

    iv::GraphBuilder builder(session.get());
    auto temporal = iv::details::configure_package_definition_provider(
        builder, graph_jit_temporal_sample_feedback_id, std::nullopt, {});
    auto converted = iv::details::configure_package_definition_provider(
        builder, graph_jit_converted_sample_feedback_id, std::nullopt, {});
    temporal(converted);
    converted(static_cast<iv::SamplePortRef>(temporal).detach(
        latency, initial_value));
    builder.outputs();
    auto base = std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
    auto graph = std::make_shared<iv::ConfiguredGraph>(*base);

    std::vector<iv::ConfiguredSampleConnection> projected;
    bool split_detached = false;
    for (auto const& connection :
         base->connections.configured_sample_connections()) {
        if (!configured_sample_connection_is_detached(connection)) {
            projected.push_back(connection);
            continue;
        }
        if (split_detached
            || connection.source_channels.size() != 1
            || connection.target_channels.size() != 2) {
            throw std::runtime_error(
                "GraphJit projected sample-feedback fixture lost its detached connection shape");
        }
        split_detached = true;
        for (std::size_t target_index = 0;
             target_index < connection.target_channels.size(); ++target_index) {
            auto const target = connection.target_channels[target_index];
            if (target_index == 0) {
                // Force the normalized projection path to preserve a real
                // channel-count conversion boundary. The same mono source is
                // gathered as semantic stereo and stereo->mono conversion
                // therefore produces the same value while exercising mixing.
                projected.push_back(iv::ConfiguredSampleConnection{
                    .source_type = iv::ChannelTypeId::stereo,
                    .source_channels = {
                        connection.source_channels.front(),
                        connection.source_channels.front(),
                    },
                    .target_type = iv::ChannelTypeId::mono,
                    .target_channels = {target},
                });
            } else {
                projected.push_back(iv::ConfiguredSampleConnection{
                    .source_type = iv::ChannelTypeId::mono,
                    .source_channels = connection.source_channels,
                    .target_type = iv::ChannelTypeId::mono,
                    .target_channels = {target},
                });
            }
        }
    }
    if (!split_detached) {
        throw std::runtime_error(
            "GraphJit projected sample-feedback fixture lost its detach");
    }
    graph->connections = iv::GraphBuilderConnections::from_configured_connections(
        projected,
        base->connections.configured_event_connections());
    return graph;
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_permuted_unequal_latency_projected_sample_feedback_graph(
    iv::PackageRevision const& revision,
    std::size_t latency = 6,
    iv::Sample initial_value = iv::Sample{-0.25f})
{
    auto base = configured_unequal_latency_projected_sample_feedback_graph(
        revision, latency, initial_value);
    auto graph = std::make_shared<iv::ConfiguredGraph>(*base);
    auto const configured = base->connections.configured_sample_connections();
    auto connections = std::vector<iv::ConfiguredSampleConnection>(
        configured.begin(), configured.end());

    std::vector<std::size_t> detached_indices;
    for (std::size_t i = 0; i < connections.size(); ++i) {
        if (!configured_sample_connection_is_detached(connections[i])) continue;
        if (connections[i].target_channels.size() != 1) {
            throw std::runtime_error(
                "GraphJit permuted projected feedback fixture lost its mono target projection");
        }
        detached_indices.push_back(i);
    }
    if (detached_indices.size() != 2) {
        throw std::runtime_error(
            "GraphJit permuted projected feedback fixture lost its detached contributions");
    }

    std::swap(
        connections[detached_indices[0]].target_channels.front(),
        connections[detached_indices[1]].target_channels.front());
    graph->connections = iv::GraphBuilderConnections::from_configured_connections(
        connections,
        base->connections.configured_event_connections());
    return graph;
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_sample_feedback_scc_fanout_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit sample-feedback SCC-fanout builder session");
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
        graph, graph_jit_sample_feedback_a_id, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, graph_jit_sample_feedback_b_id, std::nullopt, {});
    auto identity_observer = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.sample_consumer",
        std::nullopt,
        {});
    auto converted_history_observer =
        iv::details::configure_package_definition_provider(
            graph,
            "iv.test.graph_jit.state_context.stereo_history_consumer",
            std::nullopt,
            {});

    first(second);
    second(static_cast<iv::SamplePortRef>(first).detach(
        6, iv::Sample{-0.625f}));
    identity_observer(first);
    converted_history_observer(first);
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

} // namespace

TEST_F(GraphJitRuntimeFixture, ExactSampleDetachFeedback)
{
    // Its nodes need identities for their State and storage to migrate.
    auto feedback_graph =
        with_node_identities(configured_sample_feedback_graph(*revision));
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const detached = std::ranges::find_if(
        analysis->sample_connections,
        [](iv::graph_jit::detail::SampleConnectionPlan const& connection) {
            return connection.detach.has_value();
        });
    ASSERT_NE(detached, analysis->sample_connections.end());
    ASSERT_TRUE(detached->detach.has_value());
    EXPECT_EQ(detached->detach->loop_extra_latency, 6u);
    ASSERT_TRUE(detached->detach_initial_value.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(
            *detached->detach_initial_value)),
        -0.625f);
    ASSERT_TRUE(detached->detach_region.has_value());
    ASSERT_LT(*detached->detach_region, analysis->schedule.regions.size());
    auto const& region = analysis->schedule.regions[*detached->detach_region];
    ASSERT_TRUE(region.cyclic);
    EXPECT_EQ(region.maximum_block_size, 4u);
    EXPECT_EQ(region.scc_feedback_latency, 4u);

    auto compiled = compile_graph(feedback_graph, 122);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    SampleFeedbackAStateMirror* state_a = nullptr;
    SampleFeedbackBStateMirror* state_b = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            compiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(SampleFeedbackAStateMirror)) {
            ASSERT_EQ(state_a, nullptr);
            state_a = static_cast<SampleFeedbackAStateMirror*>(
                storage.state_ptr(i));
        } else if (state_size == sizeof(SampleFeedbackBStateMirror)) {
            ASSERT_EQ(state_b, nullptr);
            state_b = static_cast<SampleFeedbackBStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(state_a, nullptr);
    ASSERT_NE(state_b, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 13);

    ASSERT_EQ(state_a->calls, 4u);
    ASSERT_EQ(state_b->calls, 4u);
    EXPECT_EQ(state_a->scc_feedback_latency, 4u);
    EXPECT_EQ(state_b->scc_feedback_latency, 4u);
    EXPECT_EQ(state_a->marker, 0x5a11ce01u);
    EXPECT_EQ(state_b->marker, 0x5b22ce02ull);

    std::array<std::uint64_t, 4> const expected_indices{0, 4, 8, 12};
    std::array<std::uint64_t, 4> const expected_sizes{4, 4, 4, 1};
    std::array<float, 4> const expected_first{-0.625f, -0.625f, 0.375f, 1.375f};
    std::array<float, 4> const expected_last{-0.625f, 0.375f, 0.375f, 1.375f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state_a->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state_b->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state_a->block_sizes[slice], expected_sizes[slice]);
        EXPECT_EQ(state_b->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state_a->first_inputs[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state_b->first_inputs[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state_a->last_inputs[slice], expected_last[slice]);
        EXPECT_FLOAT_EQ(state_b->last_inputs[slice], expected_last[slice]);
    }

    // The detached branch stores A's produced value at its absolute sample
    // index and reads it six samples later. Recompile at an awkward boundary:
    // the next root call must observe A[7..12], proving the feedback timeline was
    // migrated rather than reinitialized to the authored -0.625 value.
    auto recompiled = compile_graph(feedback_graph, 123);
    ASSERT_TRUE(recompiled.succeeded())
        << (recompiled.diagnostics.empty()
                ? ""
                : recompiled.diagnostics.front().message);
    auto migrated_storage =
        recompiled.compiled_graph->node_layout.create_storage(resources);
    migrated_storage.initialize(&storage);
    SampleFeedbackAStateMirror* migrated_a = nullptr;
    SampleFeedbackBStateMirror* migrated_b = nullptr;
    for (std::size_t i = 0;
         i < recompiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            recompiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(SampleFeedbackAStateMirror)) {
            migrated_a = static_cast<SampleFeedbackAStateMirror*>(
                migrated_storage.state_ptr(i));
        } else if (state_size == sizeof(SampleFeedbackBStateMirror)) {
            migrated_b = static_cast<SampleFeedbackBStateMirror*>(
                migrated_storage.state_ptr(i));
        }
    }
    ASSERT_NE(migrated_a, nullptr);
    ASSERT_NE(migrated_b, nullptr);

    recompiled.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(13), 6);
    // Both nodes keep their State: these are their fifth and sixth calls.
    ASSERT_EQ(migrated_a->calls, 6u);
    ASSERT_EQ(migrated_b->calls, 6u);
    EXPECT_EQ(migrated_a->indices[4], 13u);
    EXPECT_EQ(migrated_b->indices[4], 13u);
    EXPECT_EQ(migrated_a->block_sizes[4], 4u);
    EXPECT_EQ(migrated_b->block_sizes[4], 4u);
    EXPECT_FLOAT_EQ(migrated_a->first_inputs[4], 1.375f);
    EXPECT_FLOAT_EQ(migrated_b->first_inputs[4], 1.375f);
    EXPECT_FLOAT_EQ(migrated_a->last_inputs[4], 1.375f);
    EXPECT_FLOAT_EQ(migrated_b->last_inputs[4], 1.375f);
    EXPECT_EQ(migrated_a->indices[5], 17u);
    EXPECT_EQ(migrated_b->indices[5], 17u);
    EXPECT_EQ(migrated_a->block_sizes[5], 2u);
    EXPECT_EQ(migrated_b->block_sizes[5], 2u);
    EXPECT_FLOAT_EQ(migrated_a->first_inputs[5], 1.375f);
    EXPECT_FLOAT_EQ(migrated_b->first_inputs[5], 1.375f);
    EXPECT_FLOAT_EQ(migrated_a->last_inputs[5], 2.375f);
    EXPECT_FLOAT_EQ(migrated_b->last_inputs[5], 2.375f);
}

TEST_F(GraphJitRuntimeFixture, SampleFeedbackSccFansOutToAcyclicIdentityAndConvertedHistoryConsumers)
{
    auto feedback_graph = configured_sample_feedback_scc_fanout_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());

    std::size_t fanout_leaving_cycle = 0;
    for (auto const& connection : analysis->sample_connections) {
        if (connection.detach || connection.source_channel_timings.empty()) continue;
        auto const source_bundle =
            connection.source_channel_timings.front().source.bundle;
        auto const target_bundle = connection.target_port.node_bundle_handle;
        if (source_bundle >= analysis->schedule.bundle_to_region.size()
            || target_bundle >= analysis->schedule.bundle_to_region.size()
            || !analysis->schedule.bundle_to_region[source_bundle]
            || !analysis->schedule.bundle_to_region[target_bundle]) {
            continue;
        }
        auto const source_region =
            *analysis->schedule.bundle_to_region[source_bundle];
        auto const target_region =
            *analysis->schedule.bundle_to_region[target_bundle];
        if (source_region >= analysis->schedule.regions.size()
            || target_region >= analysis->schedule.regions.size()) {
            continue;
        }
        if (analysis->schedule.regions[source_region].cyclic
            && !analysis->schedule.regions[target_region].cyclic) {
            ++fanout_leaving_cycle;
        }
    }
    EXPECT_EQ(fanout_leaving_cycle, 2u);

    auto compiled = compile_graph(feedback_graph, 140);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 8);

    // Both probe states are 40 bytes, so state size alone cannot distinguish
    // them. Identify the stereo/history probe by its post-execution marker and
    // treat the other 40-byte state as the identity consumer.
    SampleConsumerProbeStateMirror* identity = nullptr;
    StereoHistoryConsumerStateMirror* converted_history = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            != sizeof(SampleConsumerProbeStateMirror)) {
            continue;
        }
        auto* candidate = storage.state_ptr(i);
        auto* history_candidate =
            static_cast<StereoHistoryConsumerStateMirror*>(candidate);
        if (history_candidate->marker == 0xa1b2c3d4e5f60718ull) {
            ASSERT_EQ(converted_history, nullptr);
            converted_history = history_candidate;
        } else {
            ASSERT_EQ(identity, nullptr);
            identity = static_cast<SampleConsumerProbeStateMirror*>(candidate);
        }
    }
    ASSERT_NE(identity, nullptr);
    ASSERT_NE(converted_history, nullptr);

    // A's output is 0.375 + floor(sample / 6). The acyclic identity consumer
    // executes once per root call, after the feedback SCC has completed all of
    // its internal 4-sample slices, and must still see the whole root window.
    ASSERT_EQ(identity->calls, 2u);
    EXPECT_EQ(identity->last_index, 64u);
    EXPECT_EQ(identity->last_block_size, 8u);
    EXPECT_FLOAT_EQ(identity->first, 10.375f);
    EXPECT_FLOAT_EQ(identity->last, 11.375f);
    EXPECT_FLOAT_EQ(identity->sum, 89.0f);

    // The sibling branch simultaneously exercises mono->stereo conversion and
    // five samples of target history across the SCC boundary. At sample 64,
    // history 5 is sample 59, whose value is 9.375.
    ASSERT_EQ(converted_history->calls, 2u);
    EXPECT_EQ(converted_history->last_index, 64u);
    EXPECT_FLOAT_EQ(converted_history->current_left, 10.375f);
    EXPECT_FLOAT_EQ(converted_history->current_right, 10.375f);
    EXPECT_FLOAT_EQ(converted_history->history_5_left, 9.375f);
    EXPECT_FLOAT_EQ(converted_history->history_5_right, 9.375f);
    EXPECT_EQ(converted_history->marker, 0xa1b2c3d4e5f60718ull);
}

TEST_F(GraphJitRuntimeFixture, MultipleSampleDetachBranchesShareProducerHomeAndFallbackCopy)
{
    auto feedback_graph = configured_multi_branch_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 3u);
    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 3u);
    ASSERT_EQ(sample_plan->persistent_allocations.size(), 2u);

    auto const canonical =
        sample_plan->producer_groups.front()->canonical_representation;
    ASSERT_LT(canonical, sample_plan->representations.size());
    EXPECT_EQ(
        sample_plan->representations[canonical].storage,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);

    auto const timeline_for_latency = [&](std::size_t latency)
        -> iv::graph_jit::detail::SampleFeedbackTimelinePlan const* {
        auto const it = std::ranges::find_if(
            sample_plan->feedback_timelines,
            [&](auto const& timeline) {
                return timeline.loop_extra_latency == latency;
            });
        return it == sample_plan->feedback_timelines.end()
            ? nullptr
            : std::addressof(*it);
    };
    auto const* fast = timeline_for_latency(4);
    auto const* initialized = timeline_for_latency(6);
    auto const* slow = timeline_for_latency(8);
    ASSERT_NE(fast, nullptr);
    ASSERT_NE(initialized, nullptr);
    ASSERT_NE(slow, nullptr);

    EXPECT_EQ(
        fast->writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::producer_home);
    EXPECT_EQ(fast->timeline_representation, canonical);
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(fast->initial_value)),
        0.0f);
    EXPECT_EQ(
        slow->writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::producer_home);
    EXPECT_EQ(slow->timeline_representation, canonical);
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(slow->initial_value)),
        0.0f);

    EXPECT_EQ(
        initialized->writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::copy);
    EXPECT_NE(initialized->timeline_representation, canonical);
    EXPECT_EQ(initialized->writer.source_representation, canonical);
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(
            initialized->initial_value)),
        -2.0f);

    auto compiled = compile_graph(feedback_graph, 135);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<MultiBranchSampleFeedbackStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 13);

    ASSERT_EQ(state->calls, 4u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);
    EXPECT_EQ(state->marker, 0x6d756c74u);

    std::array<std::uint64_t, 4> const expected_indices{0, 4, 8, 12};
    std::array<std::uint64_t, 4> const expected_sizes{4, 4, 4, 1};
    std::array<float, 4> const expected_first_fast{0.0f, 1.0f, 2.0f, 3.0f};
    std::array<float, 4> const expected_last_fast{0.0f, 1.0f, 2.0f, 3.0f};
    std::array<float, 4> const expected_first_slow{0.0f, 0.0f, 1.0f, 2.0f};
    std::array<float, 4> const expected_last_slow{0.0f, 0.0f, 1.0f, 2.0f};
    std::array<float, 4> const expected_first_initialized{-2.0f, -2.0f, 1.0f, 2.0f};
    std::array<float, 4> const expected_last_initialized{-2.0f, 1.0f, 2.0f, 2.0f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->first_fast[slice], expected_first_fast[slice]);
        EXPECT_FLOAT_EQ(state->last_fast[slice], expected_last_fast[slice]);
        EXPECT_FLOAT_EQ(state->first_slow[slice], expected_first_slow[slice]);
        EXPECT_FLOAT_EQ(state->last_slow[slice], expected_last_slow[slice]);
        EXPECT_FLOAT_EQ(
            state->first_initialized[slice], expected_first_initialized[slice]);
        EXPECT_FLOAT_EQ(
            state->last_initialized[slice], expected_last_initialized[slice]);
    }
}

TEST_F(GraphJitRuntimeFixture, SampleDetachFeedbackPreservesSourceLatencyAndTargetHistory)
{
    auto feedback_graph = configured_temporal_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 1u);
    auto const& connection = analysis->sample_connections.front();
    ASSERT_TRUE(connection.detach.has_value());
    EXPECT_EQ(connection.detach->loop_extra_latency, 6u);
    EXPECT_EQ(connection.source_latency, 2u);
    EXPECT_EQ(connection.read_latency, 2u);
    EXPECT_EQ(connection.target_history, 3u);
    ASSERT_TRUE(connection.detach_initial_value.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(
            *connection.detach_initial_value)),
        -0.625f);

    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);
    auto const& producer = analysis->sample_producer_groups.front();
    // Feedback delay/history remains branch-local, but the canonical producer
    // must retain its own authored latency horizon because OutputPort::update()
    // may revise those already-authored frames on a later invocation.
    EXPECT_EQ(producer.storage_requirements.retained_frames, 2u);
    ASSERT_TRUE(producer.storage_plan.has_value());
    EXPECT_EQ(
        producer.storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 1u);
    ASSERT_EQ(sample_plan->persistent_allocations.size(), 2u);
    ASSERT_EQ(sample_plan->producer_authored_extents.size(), 1u);
    EXPECT_EQ(
        sample_plan->producer_authored_extents.front().source_representation,
        sample_plan->producer_groups.front()->canonical_representation);
    EXPECT_EQ(
        sample_plan->producer_authored_extents.front().maximum_revision_frames,
        2u);
    auto const& timeline = sample_plan->feedback_timelines.front();
    EXPECT_EQ(
        timeline.writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::copy);
    EXPECT_EQ(timeline.writer.revision_frames, 2u);
    auto const ring_representation = timeline.timeline_representation;
    ASSERT_LT(ring_representation, sample_plan->representations.size());
    auto const persistent_index =
        sample_plan->representations[ring_representation].persistent_allocation;
    ASSERT_LT(persistent_index, sample_plan->persistent_allocations.size());
    auto const& persistent = sample_plan->persistent_allocations[persistent_index];
    EXPECT_EQ(persistent.retained_frames, 11u);
    EXPECT_EQ(persistent.frame_capacity, 128u);

    auto compiled = compile_graph(feedback_graph, 124);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<TemporalSampleFeedbackStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 17);

    ASSERT_EQ(state->calls, 5u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);
    EXPECT_EQ(state->marker, 0x7e4fba11u);

    std::array<std::uint64_t, 5> const expected_indices{0, 4, 8, 12, 16};
    std::array<std::uint64_t, 5> const expected_sizes{4, 4, 4, 4, 1};
    std::array<float, 5> const expected_current{
        -0.625f, -0.625f, 0.375f, 0.375f, 1.375f};
    std::array<float, 5> const expected_history_3{
        -0.625f, -0.625f, -0.625f, 0.375f, 0.375f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->current_inputs[slice], expected_current[slice]);
        EXPECT_FLOAT_EQ(
            state->history_3_inputs[slice], expected_history_3[slice]);
        EXPECT_FLOAT_EQ(state->first_inputs[slice], expected_current[slice]);
        EXPECT_FLOAT_EQ(state->last_inputs[slice], expected_current[slice]);
    }
}

TEST_F(GraphJitRuntimeFixture, SampleDetachFeedbackRecopiesAuthoredLatencyHorizon)
{
    auto feedback_graph = configured_revising_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 1u);
    auto const& connection = analysis->sample_connections.front();
    ASSERT_TRUE(connection.detach.has_value());
    EXPECT_EQ(connection.source_latency, 2u);
    EXPECT_EQ(connection.detach->loop_extra_latency, 6u);
    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);
    EXPECT_EQ(
        analysis->sample_producer_groups.front().storage_requirements.retained_frames,
        2u);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 1u);
    auto const& timeline = sample_plan->feedback_timelines.front();
    EXPECT_EQ(
        timeline.writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::copy);
    EXPECT_EQ(timeline.writer.revision_frames, 2u);

    auto compiled = compile_graph(feedback_graph, 152);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<RevisingSampleFeedbackStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(8), 4);

    ASSERT_EQ(state->calls, 3u);
    EXPECT_EQ(state->indices[0], 0u);
    EXPECT_EQ(state->indices[1], 4u);
    EXPECT_EQ(state->indices[2], 8u);
    EXPECT_FLOAT_EQ(state->first_inputs[0], -1.0f);
    EXPECT_FLOAT_EQ(state->last_inputs[0], -1.0f);
    EXPECT_FLOAT_EQ(state->first_inputs[1], -1.0f);
    EXPECT_FLOAT_EQ(state->last_inputs[1], -1.0f);
    EXPECT_FLOAT_EQ(state->first_inputs[2], 0.0f);
    EXPECT_FLOAT_EQ(state->last_inputs[2], 103.0f);
    EXPECT_EQ(state->marker, 0x5a17e001u);
}

TEST_F(GraphJitRuntimeFixture, ProjectedSampleDetachFeedbackRecomputesRevisionHorizon)
{
    auto feedback_graph =
        configured_projected_revising_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 1u);
    auto const& connection = analysis->sample_connections.front();
    EXPECT_FALSE(connection.detach.has_value());
    ASSERT_TRUE(iv::graph_jit::detail::has_detached_source(connection));
    EXPECT_EQ(connection.source_latency, 2u);
    EXPECT_FALSE(connection.canonical_source_port.has_value());
    ASSERT_EQ(connection.source_channel_timings.size(), 3u);
    ASSERT_EQ(connection.routed_contributions.size(), 2u);
    for (auto const& contribution : connection.routed_contributions) {
        ASSERT_TRUE(contribution.detach.has_value());
        EXPECT_EQ(contribution.detach->loop_extra_latency, 6u);
    }

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 2u);
    ASSERT_EQ(sample_plan->compositions.size(), 1u);
    for (auto const& timeline : sample_plan->feedback_timelines) {
        EXPECT_EQ(
            timeline.writer.kind,
            iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::composition);
        EXPECT_EQ(timeline.writer.revision_frames, 2u);
        ASSERT_EQ(timeline.writer.composition_contributions.size(), 1u);
    }

    auto compiled = compile_graph(feedback_graph, 155);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<ProjectedRevisingSampleFeedbackStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(8), 4);

    ASSERT_EQ(state->calls, 3u);
    EXPECT_EQ(state->indices[0], 0u);
    EXPECT_EQ(state->indices[1], 4u);
    EXPECT_EQ(state->indices[2], 8u);
    EXPECT_FLOAT_EQ(state->first_left[0], -1.0f);
    EXPECT_FLOAT_EQ(state->first_right[0], -1.0f);
    EXPECT_FLOAT_EQ(state->last_left[0], -1.0f);
    EXPECT_FLOAT_EQ(state->last_right[0], -1.0f);
    EXPECT_FLOAT_EQ(state->first_left[1], -1.0f);
    EXPECT_FLOAT_EQ(state->first_right[1], -1.0f);
    EXPECT_FLOAT_EQ(state->last_left[1], -1.0f);
    EXPECT_FLOAT_EQ(state->last_right[1], -1.0f);
    EXPECT_FLOAT_EQ(state->first_left[2], 0.0f);
    EXPECT_FLOAT_EQ(state->first_right[2], 0.0f);
    EXPECT_FLOAT_EQ(state->last_left[2], 103.0f);
    EXPECT_FLOAT_EQ(state->last_right[2], 103.0f);
    EXPECT_EQ(state->marker, 0x52e71e55u);
}

TEST_F(GraphJitRuntimeFixture, ConvertedSampleDetachFeedback)
{
    auto feedback_graph = configured_converted_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 1u);
    auto const& connection = analysis->sample_connections.front();
    ASSERT_TRUE(connection.detach.has_value());
    EXPECT_EQ(connection.detach->loop_extra_latency, 6u);
    EXPECT_TRUE(connection.requires_conversion);
    ASSERT_TRUE(connection.canonical_source_layout.has_value());
    EXPECT_EQ(
        connection.canonical_source_layout->channel_type,
        iv::ChannelTypeId::mono);
    EXPECT_EQ(connection.target_layout.channel_type, iv::ChannelTypeId::stereo);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 1u);
    EXPECT_TRUE(sample_plan->materializations.empty());
    auto const& timeline = sample_plan->feedback_timelines.front();
    EXPECT_EQ(
        timeline.writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::copy);
    auto const ring = timeline.timeline_representation;
    EXPECT_EQ(
        sample_plan->representations[ring].channel_layout.channel_type,
        iv::ChannelTypeId::mono);
    EXPECT_FALSE(sample_plan->connection_representations.front().has_value());
    ASSERT_TRUE(sample_plan->connection_channel_bindings.front().has_value());
    auto const& converted_channels =
        *sample_plan->connection_channel_bindings.front();
    ASSERT_EQ(converted_channels.size(), 2u);
    for (auto const& channel : converted_channels) {
        EXPECT_EQ(channel.representation, ring);
        EXPECT_EQ(channel.representation_channel, 0u);
        EXPECT_EQ(channel.frame_delay, 0u);
    }

    auto compiled = compile_graph(feedback_graph, 125);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<ConvertedSampleFeedbackStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 17);

    ASSERT_EQ(state->calls, 5u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);
    EXPECT_EQ(state->marker, 0xc04e7ed1u);

    std::array<std::uint64_t, 5> const expected_indices{0, 4, 8, 12, 16};
    std::array<std::uint64_t, 5> const expected_sizes{4, 4, 4, 4, 1};
    std::array<float, 5> const expected_first{
        -0.25f, -0.25f, 0.75f, 1.75f, 1.75f};
    std::array<float, 5> const expected_last{
        -0.25f, 0.75f, 0.75f, 1.75f, 1.75f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->first_left[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state->first_right[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state->last_left[slice], expected_last[slice]);
        EXPECT_FLOAT_EQ(state->last_right[slice], expected_last[slice]);
    }
}

TEST_F(GraphJitRuntimeFixture, ZeroInitializedConvertedFeedbackWritesDirectlyToProducerHome)
{
    auto feedback_graph = configured_converted_sample_feedback_graph(
        *revision, 6, iv::Sample{0.0f});
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 1u);
    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);
    EXPECT_EQ(
        analysis->sample_producer_groups.front().storage_requirements.retained_frames,
        0u);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 1u);
    EXPECT_EQ(
        sample_plan->feedback_timelines.front().writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::producer_home);
    EXPECT_EQ(sample_plan->feedback_timelines.front().writer.revision_frames, 0u);
    ASSERT_EQ(sample_plan->persistent_allocations.size(), 1u);
    EXPECT_TRUE(sample_plan->materializations.empty());
    ASSERT_EQ(sample_plan->representations.size(), 1u);

    auto const canonical =
        sample_plan->producer_groups.front()->canonical_representation;
    EXPECT_TRUE(
        sample_plan->representations[canonical].canonical_producer_representation);
    EXPECT_EQ(
        sample_plan->representations[canonical].storage,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(
        sample_plan->representations[canonical].channel_layout.channel_type,
        iv::ChannelTypeId::mono);
    EXPECT_FALSE(sample_plan->connection_representations.front().has_value());
    ASSERT_TRUE(sample_plan->connection_channel_bindings.front().has_value());
    auto const& converted_channels =
        *sample_plan->connection_channel_bindings.front();
    ASSERT_EQ(converted_channels.size(), 2u);
    for (auto const& channel : converted_channels) {
        EXPECT_EQ(channel.representation, canonical);
        EXPECT_EQ(channel.representation_channel, 0u);
        EXPECT_EQ(channel.frame_delay, 0u);
    }

    auto compiled = compile_graph(feedback_graph, 126);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<ConvertedSampleFeedbackStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 17);

    ASSERT_EQ(state->calls, 5u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);
    EXPECT_EQ(state->marker, 0xc04e7ed1u);

    std::array<std::uint64_t, 5> const expected_indices{0, 4, 8, 12, 16};
    std::array<std::uint64_t, 5> const expected_sizes{4, 4, 4, 4, 1};
    std::array<float, 5> const expected_first{0.0f, 0.0f, 1.0f, 2.0f, 2.0f};
    std::array<float, 5> const expected_last{0.0f, 1.0f, 1.0f, 2.0f, 2.0f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->first_left[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state->first_right[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state->last_left[slice], expected_last[slice]);
        EXPECT_FLOAT_EQ(state->last_right[slice], expected_last[slice]);
    }
}

TEST_F(GraphJitRuntimeFixture, ProjectedSampleDetachFeedback)
{
    auto feedback_graph = configured_projected_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 2u);
    auto detached = std::ranges::find_if(
        analysis->sample_connections,
        [](auto const& connection) {
            return iv::graph_jit::detail::has_detached_source(connection);
        });
    ASSERT_NE(detached, analysis->sample_connections.end());
    EXPECT_FALSE(detached->detach.has_value());
    EXPECT_FALSE(detached->canonical_source_port.has_value());
    EXPECT_FALSE(detached->canonical_source_layout.has_value());
    ASSERT_EQ(detached->source_channel_timings.size(), 3u);
    EXPECT_EQ(detached->source_channel_timings[0].source.channel, 0u);
    EXPECT_EQ(detached->source_channel_timings[1].source.channel, 0u);
    EXPECT_EQ(detached->source_channel_timings[2].source.channel, 0u);
    EXPECT_EQ(detached->source_channel_timings[0].read_latency, 2u);
    EXPECT_EQ(detached->source_channel_timings[1].read_latency, 2u);
    EXPECT_EQ(detached->source_channel_timings[2].read_latency, 2u);
    EXPECT_TRUE(detached->projection_contributions.empty());
    ASSERT_EQ(detached->routed_contributions.size(), 2u);
    EXPECT_EQ(
        detached->routed_contributions[0].source_coefficients,
        (std::vector<float>{0.5f, 0.5f, 0.0f}));
    EXPECT_EQ(
        detached->routed_contributions[0].target_coefficients,
        (std::vector<float>{1.0f, 0.0f}));
    EXPECT_EQ(
        detached->routed_contributions[1].source_coefficients,
        (std::vector<float>{0.0f, 0.0f, 1.0f}));
    EXPECT_EQ(
        detached->routed_contributions[1].target_coefficients,
        (std::vector<float>{0.0f, 1.0f}));
    EXPECT_EQ(detached->target_layout.channel_type, iv::ChannelTypeId::stereo);
    for (auto const& contribution : detached->routed_contributions) {
        ASSERT_TRUE(contribution.detach.has_value());
        EXPECT_EQ(contribution.detach->loop_extra_latency, 6u);
        ASSERT_TRUE(contribution.detach_region.has_value());
    }
    auto const detach_region =
        *detached->routed_contributions.front().detach_region;
    ASSERT_LT(detach_region, analysis->schedule.regions.size());
    auto const& region = analysis->schedule.regions[detach_region];
    EXPECT_TRUE(region.cyclic);
    EXPECT_EQ(region.maximum_block_size, 4u);
    EXPECT_EQ(region.scc_feedback_latency, 4u);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 2u);
    ASSERT_EQ(sample_plan->compositions.size(), 1u);
    auto const detached_index = static_cast<std::size_t>(
        std::distance(analysis->sample_connections.begin(), detached));
    ASSERT_LT(detached_index, sample_plan->connection_representations.size());
    ASSERT_TRUE(sample_plan->connection_representations[detached_index].has_value());
    auto const composed =
        *sample_plan->connection_representations[detached_index];
    ASSERT_LT(composed, sample_plan->representations.size());
    EXPECT_EQ(
        sample_plan->representations[composed].storage,
        iv::RealtimeBufferStorageKind::transient_stack);
    EXPECT_EQ(
        sample_plan->representations[composed].channel_layout.channel_type,
        iv::ChannelTypeId::stereo);
    for (auto const& timeline : sample_plan->feedback_timelines) {
        EXPECT_EQ(timeline.connection_index, detached_index);
        EXPECT_EQ(timeline.channel_layout.channel_type, iv::ChannelTypeId::mono);
        EXPECT_EQ(timeline.retained_frames, 8u);
        EXPECT_EQ(timeline.loop_extra_latency, 6u);
        EXPECT_EQ(
            timeline.writer.kind,
            iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::composition);
        EXPECT_EQ(timeline.writer.revision_frames, 2u);
        ASSERT_EQ(timeline.writer.composition_contributions.size(), 1u);
        auto const representation = timeline.timeline_representation;
        ASSERT_LT(representation, sample_plan->representations.size());
        EXPECT_EQ(
            sample_plan->representations[representation].storage,
            iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    }
    auto const mixed_timeline = std::ranges::find_if(
        sample_plan->feedback_timelines,
        [](auto const& timeline) {
            return timeline.writer.composition_contributions.front()
                    .sources.size() == 2u;
        });
    ASSERT_NE(mixed_timeline, sample_plan->feedback_timelines.end());
    EXPECT_EQ(
        mixed_timeline->writer.composition_contributions.front()
            .source_layout.channel_type,
        iv::ChannelTypeId::stereo);

    auto compiled = compile_graph(feedback_graph, 127);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 17);

    // TemporalSampleFeedback and ConvertedSampleFeedback intentionally have
    // same-sized state mirrors, so state_size cannot identify the converted
    // node in this two-node fixture. Identify it by the node-specific marker
    // written by tick_block instead.
    ConvertedSampleFeedbackStateMirror* state = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            != sizeof(ConvertedSampleFeedbackStateMirror)) {
            continue;
        }
        auto* candidate = static_cast<ConvertedSampleFeedbackStateMirror*>(
            storage.state_ptr(i));
        if (candidate != nullptr && candidate->marker == 0xc04e7ed1u) {
            ASSERT_EQ(state, nullptr);
            state = candidate;
        }
    }
    ASSERT_NE(state, nullptr);

    ASSERT_EQ(state->calls, 5u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);
    EXPECT_EQ(state->marker, 0xc04e7ed1u);

    std::array<std::uint64_t, 5> const expected_indices{0, 4, 8, 12, 16};
    std::array<std::uint64_t, 5> const expected_sizes{4, 4, 4, 4, 1};
    std::array<float, 5> const expected_first{
        -0.25f, -0.25f, 1.75f, 1.75f, 3.75f};
    std::array<float, 5> const expected_last{
        -0.25f, -0.25f, 1.75f, 1.75f, 3.75f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->first_left[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state->first_right[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state->last_left[slice], expected_last[slice]);
        EXPECT_FLOAT_EQ(state->last_right[slice], expected_last[slice]);
    }
}

TEST_F(GraphJitRuntimeFixture, ProjectedSampleDetachFeedbackAlignsUnequalMixingLatencies)
{
    auto feedback_graph =
        configured_unequal_latency_projected_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto detached = std::ranges::find_if(
        analysis->sample_connections,
        [](auto const& connection) {
            return iv::graph_jit::detail::has_detached_source(connection);
        });
    ASSERT_NE(detached, analysis->sample_connections.end());
    ASSERT_EQ(detached->routed_contributions.size(), 2u);
    auto const mixed = std::ranges::find_if(
        detached->routed_contributions,
        [](auto const& contribution) {
            return std::ranges::count_if(
                contribution.source_coefficients,
                [](float coefficient) { return coefficient != 0.0f; }) == 2u;
        });
    ASSERT_NE(mixed, detached->routed_contributions.end());
    std::vector<std::size_t> mixed_source_indices;
    for (std::size_t source = 0;
         source < mixed->source_coefficients.size(); ++source) {
        if (mixed->source_coefficients[source] != 0.0f)
            mixed_source_indices.push_back(source);
    }
    ASSERT_EQ(mixed_source_indices.size(), 2u);
    auto const first_latency = detached->source_channel_timings[
        mixed_source_indices[0]].read_latency;
    auto const second_latency = detached->source_channel_timings[
        mixed_source_indices[1]].read_latency;
    EXPECT_EQ(std::min(first_latency, second_latency), 0u);
    EXPECT_EQ(std::max(first_latency, second_latency), 2u);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    auto const aligned_timeline = std::ranges::find_if(
        sample_plan->feedback_timelines,
        [](auto const& timeline) {
            return std::ranges::any_of(
                timeline.writer.composition_contributions,
                [](auto const& contribution) {
                    return contribution.feedback_alignment_representation
                        != iv::graph_jit::detail::no_sample_representation;
                });
        });
    ASSERT_NE(aligned_timeline, sample_plan->feedback_timelines.end());
    auto const& aligned =
        aligned_timeline->writer.composition_contributions.front();
    ASSERT_NE(
        aligned.feedback_alignment_representation,
        iv::graph_jit::detail::no_sample_representation);
    EXPECT_EQ(aligned.feedback_alignment_write_latency, 0u);

    auto compiled = compile_graph(feedback_graph, 128);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 17);

    ConvertedSampleFeedbackStateMirror* state = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            != sizeof(ConvertedSampleFeedbackStateMirror)) {
            continue;
        }
        auto* candidate = static_cast<ConvertedSampleFeedbackStateMirror*>(
            storage.state_ptr(i));
        if (candidate != nullptr && candidate->marker == 0xc04e7ed1u) {
            ASSERT_EQ(state, nullptr);
            state = candidate;
        }
    }
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->calls, 5u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);

    std::array<std::uint64_t, 5> const expected_indices{0, 4, 8, 12, 16};
    std::array<std::uint64_t, 5> const expected_sizes{4, 4, 4, 4, 1};
    std::array<float, 5> const expected_first_left{
        -0.25f, -0.25f, 1.25f, 1.375f, 3.0f};
    std::array<float, 5> const expected_last_left{
        -0.25f, 0.25f, 1.25f, 2.25f, 3.0f};
    std::array<float, 5> const expected_first_right{
        -0.25f, -0.25f, 1.75f, 1.75f, 3.5f};
    std::array<float, 5> const expected_last_right{
        -0.25f, -0.25f, 1.75f, 2.0f, 3.5f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->first_left[slice], expected_first_left[slice]);
        EXPECT_FLOAT_EQ(state->last_left[slice], expected_last_left[slice]);
        EXPECT_FLOAT_EQ(state->first_right[slice], expected_first_right[slice]);
        EXPECT_FLOAT_EQ(state->last_right[slice], expected_last_right[slice]);
    }
}

TEST_F(GraphJitRuntimeFixture, ProjectedSampleDetachFeedbackPermutesTargetChannels)
{
    auto feedback_graph =
        configured_permuted_unequal_latency_projected_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto detached = std::ranges::find_if(
        analysis->sample_connections,
        [](auto const& connection) {
            return iv::graph_jit::detail::has_detached_source(connection);
        });
    ASSERT_NE(detached, analysis->sample_connections.end());
    ASSERT_EQ(detached->routed_contributions.size(), 2u);
    EXPECT_EQ(
        detached->routed_contributions[0].target_coefficients,
        (std::vector<float>{0.0f, 1.0f}));
    EXPECT_EQ(
        detached->routed_contributions[1].target_coefficients,
        (std::vector<float>{1.0f, 0.0f}));

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 2u);
    ASSERT_EQ(sample_plan->compositions.size(), 1u);
    auto const& final = sample_plan->compositions.front().contributions.front();
    EXPECT_EQ(
        final.transform.coefficients,
        (std::vector<float>{0.0f, 1.0f, 1.0f, 0.0f}));

    auto compiled = compile_graph(feedback_graph, 129);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 17);

    ConvertedSampleFeedbackStateMirror* state = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            != sizeof(ConvertedSampleFeedbackStateMirror)) {
            continue;
        }
        auto* candidate = static_cast<ConvertedSampleFeedbackStateMirror*>(
            storage.state_ptr(i));
        if (candidate != nullptr && candidate->marker == 0xc04e7ed1u) {
            ASSERT_EQ(state, nullptr);
            state = candidate;
        }
    }
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->calls, 5u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);

    std::array<std::uint64_t, 5> const expected_indices{0, 4, 8, 12, 16};
    std::array<std::uint64_t, 5> const expected_sizes{4, 4, 4, 4, 1};
    // This is the unequal-latency feedback waveform with its semantic target
    // projection swapped. If lowering accidentally canonicalizes contribution
    // order instead of honoring target_channels, these pairs are reversed.
    std::array<float, 5> const expected_first_left{
        -0.25f, -0.25f, 1.75f, 1.75f, 3.5f};
    std::array<float, 5> const expected_last_left{
        -0.25f, -0.25f, 1.75f, 2.0f, 3.5f};
    std::array<float, 5> const expected_first_right{
        -0.25f, -0.25f, 1.25f, 1.375f, 3.0f};
    std::array<float, 5> const expected_last_right{
        -0.25f, 0.25f, 1.25f, 2.25f, 3.0f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->first_left[slice], expected_first_left[slice]);
        EXPECT_FLOAT_EQ(state->last_left[slice], expected_last_left[slice]);
        EXPECT_FLOAT_EQ(state->first_right[slice], expected_first_right[slice]);
        EXPECT_FLOAT_EQ(state->last_right[slice], expected_last_right[slice]);
    }
}

} // namespace iv::test::graph_jit_tests
