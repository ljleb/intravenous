#include "test_support.h"

namespace iv::test::graph_jit_tests {

iv::GlobalIndex index(std::int64_t value)
{
    return iv::GlobalIndex::from_signed(value);
}

iv::GlobalIndexRange range(std::int64_t begin, std::int64_t end)
{
    return iv::GlobalIndexRange::from_begin_and_end(index(begin), index(end));
}

iv::Sample sample_value_from_bytes(std::span<std::byte const> bytes)
{
    EXPECT_EQ(bytes.size(), sizeof(iv::Sample));
    iv::Sample result{};
    if (bytes.size() == sizeof(result)) {
        std::memcpy(&result, bytes.data(), sizeof(result));
    }
    return result;
}

bool configured_sample_connection_is_detached(
    iv::ConfiguredSampleConnection const& connection)
{
    return std::ranges::any_of(
        connection.source_channels,
        [](iv::ConfiguredSampleConnectionSource const& source) {
            return source.detach.has_value();
        });
}

std::expected<iv::graph_jit::detail::ConnectionAnalysisPlan, std::string>
build_connection_analysis_plan(
    iv::ConfiguredGraph const& graph,
    std::size_t kernel_block_size,
    iv::RealtimeStorageCostModel const& cost_model)
{
    auto port_realization = iv::graph_jit::resolve_port_realization(graph);
    if (!port_realization) {
        return std::unexpected(std::move(port_realization.error()));
    }
    return iv::graph_jit::detail::build_connection_analysis_plan(
        graph, *port_realization, kernel_block_size, cost_model);
}

iv::graph_jit::TickInvocationCall const empty_tick_invocation{};

std::shared_ptr<iv::NodeDefinitionsSnapshot const> make_graph_jit_snapshot(
    std::shared_ptr<iv::PackageRevision const> revision,
    std::uint64_t generation)
{
    auto snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>();
    snapshot->generation = generation;
    snapshot->package_revisions.push_back(revision);
    for (auto const& leaf : revision->leaf_definitions) {
        snapshot->by_id.emplace(leaf.definition_id, iv::NodeDefinitionEntry{
            .definition_id = leaf.definition_id,
            .kind = iv::NodeDefinitionKind::leaf,
            .version = generation,
            .definition = iv::LeafNodeDefinition{
                .definition_id = leaf.definition_id,
                .package_id = leaf.package_id,
                .package_root = leaf.package_root,
                .provider = leaf.provider,
                .compiler_record = leaf.compiler_record,
                .module_refs = leaf.module_refs,
            },
        });
    }
    return snapshot;
}

std::shared_ptr<iv::ConfiguredGraph const> configured_module_graph(
    iv::PackageRevision const& revision,
    std::string_view definition_id)
{
    auto const found = std::ranges::find(
        revision.module_definitions,
        definition_id,
        &iv::PackageModuleDefinition::definition_id);
    if (found == revision.module_definitions.end()) return {};
    return found->configured_graph;
}

// Builds a module definition's graph the way the package loader builds its
// default graph (module/loader.cpp), but with construction arguments. Two graphs
// built from one module this way give the nodes they share the same virtual
// nodes, whatever the arguments.
std::shared_ptr<iv::ConfiguredGraph const> configured_module_graph_with_arguments(
    iv::PackageRevision const& revision,
    std::string_view definition_id,
    std::span<iv::details::ConfigurationArgument> arguments)
{
    auto const found = std::ranges::find(
        revision.module_definitions,
        definition_id,
        &iv::PackageModuleDefinition::definition_id);
    if (found == revision.module_definitions.end()) {
        throw std::runtime_error("unknown GraphJit test module definition");
    }
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error("could not create GraphJit module builder session");
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
    iv::details::select_builder_package(
        session.get(),
        iv::details::builder_package_index(session.get(), package_root));
    iv::details::begin_builder_module(session.get(), definition_id);
    struct ModuleCallScope {
        iv::details::BuilderSession* session = nullptr;
        ~ModuleCallScope() { iv::details::end_builder_module(session); }
    } const module_call{session.get()};

    iv::GraphBuilder graph(session.get());
    auto const* signature = found->provider.signature();
    if (!signature) {
        throw std::runtime_error("GraphJit test module has no construction signature");
    }
    if (arguments.size() > signature->argument_count) {
        throw std::runtime_error("too many GraphJit test module arguments");
    }
    // Configuration type identities exist only in package code, so host test
    // code takes each argument's identity from the module's own signature. The
    // caller passes a value of that parameter's C++ type.
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        arguments[index].type = signature->parameter_types[index];
    }
    iv::details::validate_registered_signature(
        definition_id, *signature, arguments);
    found->provider.module_build(graph, arguments);
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

void expect_single_node_canonical_regions(
    iv::NodeLayout const& layout,
    iv::TockStateLayout const& tock_layout,
    std::size_t state_size,
    std::size_t background_state_size)
{
    ASSERT_EQ(layout.nodes.size(), 1u);
    ASSERT_EQ(tock_layout.nodes.size(), 1u);
    auto const& node = layout.nodes.front();
    auto const& tock_node = tock_layout.nodes.front();
    EXPECT_EQ(node.state_size, state_size);
    EXPECT_EQ(tock_node.state_size, background_state_size);
    EXPECT_TRUE(layout.imported_arrays.empty());
    EXPECT_TRUE(layout.exported_arrays.empty());
    EXPECT_EQ(layout.regions.size(), 1u);

    auto const state_region = std::ranges::find_if(
        layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::state;
        });
    ASSERT_NE(state_region, layout.regions.end());
    EXPECT_EQ(state_region->owner_node, 0u);
    EXPECT_EQ(state_region->size, state_size);
    if (state_size != 0) {
        ASSERT_GE(node.state_offset, 0);
        EXPECT_EQ(
            state_region->storage_offset,
            static_cast<std::size_t>(node.state_offset));
    }

    EXPECT_EQ(std::ranges::count_if(
        layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::state;
        }), 1u);
}

// `anonymous` with each of its concrete nodes in a virtual node of its own, as
// authored module code would place it, so that the nodes and what they own
// keep their identity across compilations of the result.
std::shared_ptr<iv::ConfiguredGraph const> with_node_identities(
    std::shared_ptr<iv::ConfiguredGraph const> const& anonymous)
{
    auto graph = std::make_shared<iv::ConfiguredGraph>(*anonymous);
    for (std::size_t bundle = 0; bundle < graph->node_bundles.size(); ++bundle) {
        auto const& configured = graph->node_bundles.bundle(bundle);
        if (!configured.is_concrete()
            || !configured.virtual_node_handles().empty()) {
            continue;
        }
        auto const record = graph->virtual_nodes.create(
            "test-node-" + std::to_string(bundle), configured.type_identity());
        graph->virtual_nodes.attach_bundle_member(
            graph->node_bundles, record, bundle);
    }
    return graph;
}

std::shared_ptr<iv::ConfiguredGraph const> configured_multi_branch_sample_feedback_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit multi-branch sample-feedback builder session");
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
        graph, graph_jit_multi_branch_sample_feedback_id, std::nullopt, {});
    auto const output = static_cast<iv::SamplePortRef>(node);
    node.connect_input(0, output.detach(4, iv::Sample{0.0f}));
    node.connect_input(1, output.detach(8, iv::Sample{0.0f}));
    node.connect_input(2, output.detach(6, iv::Sample{-2.0f}));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

// Replaces the converted node's detached input connection, from the temporal
// node, by two: its left channel mixes the temporal and converted outputs,
// whose latencies differ, and its right channel reads the temporal output. Both
// keep the base connection's detach().
std::shared_ptr<iv::ConfiguredGraph const>
project_unequal_latency_sample_feedback(
    std::shared_ptr<iv::ConfiguredGraph const> const& base)
{
    auto graph = std::make_shared<iv::ConfiguredGraph>(*base);

    std::optional<iv::SampleOutputChannelId> converted_output;
    std::optional<iv::ConfiguredSampleConnection> detached;
    for (auto const& connection :
         base->connections.configured_sample_connections()) {
        if (configured_sample_connection_is_detached(connection)) {
            detached = connection;
        } else if (connection.source_channels.size() == 1) {
            converted_output = connection.source_channels.front().channel;
        }
    }
    if (!converted_output || !detached
        || detached->source_channels.size() != 1
        || detached->target_channels.size() != 2) {
        throw std::runtime_error(
            "GraphJit unequal-latency projected sample-feedback fixture lost its base connection shape");
    }

    std::vector<iv::ConfiguredSampleConnection> projected;
    for (auto const& connection :
         base->connections.configured_sample_connections()) {
        if (!configured_sample_connection_is_detached(connection))
            projected.push_back(connection);
    }
    projected.push_back(iv::ConfiguredSampleConnection{
        .source_type = iv::ChannelTypeId::stereo,
        .source_channels = {
            detached->source_channels.front(),
            iv::ConfiguredSampleConnectionSource{
                .channel = *converted_output,
                .detach = detached->source_channels.front().detach,
            },
        },
        .target_type = iv::ChannelTypeId::mono,
        .target_channels = {detached->target_channels[0]},
    });
    projected.push_back(iv::ConfiguredSampleConnection{
        .source_type = iv::ChannelTypeId::mono,
        .source_channels = detached->source_channels,
        .target_type = iv::ChannelTypeId::mono,
        .target_channels = {detached->target_channels[1]},
    });
    graph->connections = iv::GraphBuilderConnections::from_configured_connections(
        projected,
        base->connections.configured_event_connections());
    return graph;
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_unequal_latency_projected_sample_feedback_graph(
    iv::PackageRevision const& revision,
    std::size_t latency,
    iv::Sample initial_value)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit unequal-latency projected sample-feedback builder session");
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
    return project_unequal_latency_sample_feedback(
        std::make_shared<iv::ConfiguredGraph const>(
            iv::details::take_built_graph(session.get())));
}

std::shared_ptr<iv::ConfiguredGraph const> configured_merged_retained_event_graph(
    iv::PackageRevision const& revision,
    bool persistent_ring)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) throw std::runtime_error("could not create merged retained-event session");
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
    auto const source_id = persistent_ring
        ? "iv.test.graph_jit.state_context.persistent_event_ring_source"
        : "iv.test.graph_jit.state_context.retained_trigger_event_source";
    auto const sink_id = persistent_ring
        ? "iv.test.graph_jit.state_context.persistent_event_ring_consumer"
        : "iv.test.graph_jit.state_context.retained_trigger_event_consumer";
    auto first = iv::details::configure_package_definition_provider(
        graph, source_id, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, source_id, std::nullopt, {});
    auto sink = iv::details::configure_package_definition_provider(
        graph, sink_id, std::nullopt, {});
    auto const first_port = first.event_port();
    auto const second_port = second.event_port();
    std::array sources{first_port.sources().front(), second_port.sources().front()};
    sink.connect_event_input(
        0, iv::EventPortRef(graph, first_port.type, sources));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

// One consumer (history 8) reads the fan-in of a trigger source with latency 8
// and one without latency.
std::shared_ptr<iv::ConfiguredGraph const>
configured_unequal_latency_event_fan_in_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create unequal-latency event fan-in session");
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
    auto latent = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.retained_trigger_event_source",
        std::nullopt,
        {});
    auto plain = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto sink = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.retained_trigger_event_consumer",
        std::nullopt,
        {});
    auto const latent_port = latent.event_port();
    auto const plain_port = plain.event_port();
    std::array sources{
        latent_port.sources().front(), plain_port.sources().front()};
    sink.connect_event_input(
        0, iv::EventPortRef(graph, latent_port.type, sources));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const> configured_event_feedback_graph(
    iv::PackageRevision const& revision,
    std::string_view first_definition)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit event-feedback builder session");
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
        graph, graph_jit_event_feedback_b_id, std::nullopt, {});
    first.connect_event_input(0, second.event_port());
    second.connect_event_input(0, first.event_port().detach(10));
    graph.outputs();
    // Its regions are located by migration identity, which needs node identities.
    return with_node_identities(std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get())));
}

// With `observed`, an acyclic consumer (history 320), created after the
// loop's nodes so that their identities stay, also reads the staged fan-in.
std::shared_ptr<iv::ConfiguredGraph const>
configured_staged_event_fan_in_graph(
    iv::PackageRevision const& revision, bool observed)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create staged event fan-in builder session");
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
        graph,
        graph_jit_retained_fan_in_event_feedback_a_id,
        std::nullopt,
        {});
    auto second = iv::details::configure_package_definition_provider(
        graph, graph_jit_event_feedback_b_id, std::nullopt, {});

    auto const ingress_port = ingress.event_port();
    auto const feedback_port = second.event_port();
    std::array sources{
        feedback_port.sources().front(),
        ingress_port.sources().front(),
    };
    first.connect_event_input(
        0, iv::EventPortRef(graph, feedback_port.type, sources));
    second.connect_event_input(0, first.event_port().detach(10));
    if (observed) {
        auto observer = iv::details::configure_package_definition_provider(
            graph,
            "iv.test.graph_jit.state_context.persistent_event_feedback_consumer",
            std::nullopt,
            {});
        observer.connect_event_input(
            0, iv::EventPortRef(graph, feedback_port.type, sources));
    }
    graph.outputs();
    // Its regions are located by migration identity, which needs node identities.
    return with_node_identities(std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get())));
}

std::size_t count_raw_regions(iv::NodeLayout const& layout)
{
    return static_cast<std::size_t>(std::ranges::count_if(
        layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw;
        }));
}

float ramp_value(std::int64_t position)
{
    return position < 0 ? 0.0f : static_cast<float>(position);
}

// The stable identities of a graph's concrete nodes, sorted. An edit test uses
// them to check that the nodes it expects to survive keep their identity.
std::vector<iv::graph_jit::StableConcreteNodeId> stable_node_identities(
    iv::ConfiguredGraph const& graph)
{
    auto analysis = build_connection_analysis_plan(graph, 64);
    if (!analysis) throw std::runtime_error(analysis.error());
    std::vector<iv::graph_jit::StableConcreteNodeId> identities;
    for (auto const& node : analysis->nodes) {
        if (!node.stable_identity) {
            throw std::runtime_error("GraphJit test node has no stable identity");
        }
        identities.push_back(*node.stable_identity);
    }
    std::ranges::sort(identities);
    return identities;
}

// Compiles `graph` and expects the realization of its one Sequential input
// record with `history` values of history and `in_flight` in-flight values to
// have `access`. A reproduction test uses it to show which realization it
// reaches.
void expect_sequential_input_access(
    iv::GraphJit& jit,
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> const& definitions,
    std::shared_ptr<iv::ConfiguredGraph const> const& graph,
    std::size_t history,
    std::size_t in_flight,
    iv::graph_jit::SampleRealtimePortStateAccess access,
    std::uint64_t generation)
{
    ASSERT_TRUE(graph);
    auto compiled = jit.compile(iv::GraphJitCompileRequest{
        .project_generation = generation,
        .graph = graph,
        .definitions = definitions,
    });
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& requirements =
        compiled.compiled_graph->realtime_port_state_requirements.states;
    std::vector<iv::graph_jit::SampleRealtimePortStateAccess> found;
    for (auto const& realization :
         compiled.compiled_graph->realtime_port_state_realizations
             .sample_states) {
        ASSERT_LT(realization.requirement_index, requirements.size());
        auto const& requirement = requirements[realization.requirement_index];
        if (requirement.role
                == iv::graph_jit::RealtimePortStateRole::sequential_input_history
            && requirement.extent_samples == history
            && requirement.in_flight_samples == in_flight) {
            found.push_back(realization.retained_access);
        }
    }
    ASSERT_EQ(found.size(), 1u)
        << "Sequential input records with history " << history
        << " and " << in_flight << " in-flight values";
    EXPECT_EQ(found.front(), access);
}

} // namespace iv::test::graph_jit_tests
