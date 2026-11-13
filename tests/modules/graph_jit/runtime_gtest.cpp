#include "test_support.h"

namespace iv::test::graph_jit_tests {

namespace {

// The snapshot record also carries ProjectGraph's reply (its publication), so
// the handler takes it as an lvalue.
void publish_definitions_snapshot(
    iv::ProjectGraph& project_graph,
    iv::NodeDefinitionsSnapshotChanged change)
{
    in_event_source([&] { return project_graph.handle_node_definitions_snapshot_changed(change); });
}

iv::ReflectedContinuousPortStorageBinding sample_storage_binding(
    std::span<iv::Sample> samples)
{
    auto geometry = iv::continuous_value_storage_geometry(
        iv::ContinuousValueTypeId::sample, std::nullopt);
    IV_ASSERT(geometry.has_value(),
        "Sample must have a registered continuous storage geometry");
    iv::ReflectedContinuousPortStorageBinding result{
        .geometry = *geometry,
        .value_capacity = samples.size(),
        .channel_layout = iv::mono_planar_channel_layout,
    };
    result.channels[0] = {
        .storage = reinterpret_cast<std::byte*>(samples.data()),
        .value_capacity = samples.size(),
        .value_stride_bytes = sizeof(iv::Sample),
    };
    return result;
}

constexpr char graph_jit_message_replacement_id[] =
    "iv.test.graph_jit.state_context.message_replacement";

constexpr char graph_jit_message_successor_id[] =
    "iv.test.graph_jit.state_context.message_successor";

constexpr char graph_jit_native_batch_tick_module_id[] =
    "iv.test.graph_jit.state_context.native_batch_tick_module";

constexpr char graph_jit_native_batch_aliasing_module_id[] =
    "iv.test.graph_jit.state_context.native_batch_aliasing_module";

constexpr char graph_jit_native_batch_replay_module_id[] =
    "iv.test.graph_jit.state_context.native_batch_replay_module";

constexpr char graph_jit_native_batch_activity_module_id[] =
    "iv.test.graph_jit.state_context.native_batch_activity_module";

constexpr char graph_jit_configured_module_id[] = "iv.test.graph_jit.state_context.configured_module";

constexpr char graph_jit_pointer_configured_module_id[] = "iv.test.graph_jit.state_context.pointer_configured_module";

constexpr char graph_jit_multiple_module_id[] = "iv.test.graph_jit.state_context.multiple_module";

constexpr char graph_jit_skippable_pair_module_id[] = "iv.test.graph_jit.state_context.skippable_pair_module";

constexpr char graph_jit_limited_block_module_id[] = "iv.test.graph_jit.state_context.limited_block_module";

constexpr char graph_jit_activity_module_id[] =
    "iv.test.graph_jit.state_context.activity_module";

constexpr char graph_jit_activity_disabled_module_id[] =
    "iv.test.graph_jit.state_context.activity_disabled_module";

constexpr char graph_jit_event_activity_module_id[] =
    "iv.test.graph_jit.state_context.event_activity_module";

constexpr char graph_jit_ported_module_id[] = "iv.test.graph_jit.state_context.ported_module";

constexpr char graph_jit_declaration_layout_module_id[] =
    "iv.test.graph_jit.state_context.declaration_layout_module";

struct NativeBatchTickProbeStateMirror {
    std::uint64_t tick_batch_calls = 0;
    std::uint64_t skip_batch_calls = 0;
    std::uint64_t last_lane_count = 0;
    std::uint64_t configured_id = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
};

struct NativeBatchAliasingProbeStateMirror {
    std::uint64_t tick_batch_calls = 0;
    std::uint64_t last_lane_count = 0;
    std::uint64_t configured_id = 0;
};

struct NativeBatchActivityProbeStateMirror {
    std::uint64_t tick_batch_calls = 0;
    std::uint64_t skip_batch_calls = 0;
    std::uint64_t tick_lane_total = 0;
    std::uint64_t skip_lane_total = 0;
};

struct DeclarationLayoutParentStateMirror {
    std::span<std::uint64_t> local;
    std::span<std::span<std::byte>> nested;
    std::uint64_t calls = 0;
    std::uint64_t local_sum = 0;
    std::uint64_t nested_sum = 0;
};

struct DeclarationLayoutLeafStateMirror {
    std::uint64_t marker = 0;
};

struct DeclarationLayoutLeafTockStateMirror {
    std::uint64_t marker = 0;
};

struct DeclarationLayoutProducerStateMirror {
    std::span<std::uint64_t> values;
    std::uint64_t calls = 0;
};

struct DeclarationLayoutConsumerStateMirror {
    std::span<std::uint64_t> imported;
    std::uint64_t initialize_sum = 0;
    std::uint64_t tick_sum = 0;
    std::uint64_t calls = 0;
};

struct ConfiguredProbeStateMirror {
    std::uint64_t calls = 0;
    std::size_t first = 0;
    std::size_t second = 0;
};

struct PointerConfiguredProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t first_value = 0;
    std::uint64_t second_value = 0;
    std::uint64_t null_seen = 0;
    std::uint32_t marker = 0;
    std::uint16_t tag = 0;
};

struct LimitedBlockProbeStateMirror {
    std::uint64_t tick_calls = 0;
    std::uint64_t skip_calls = 0;
    std::array<std::uint64_t, 8> tick_indices{};
    std::array<std::uint64_t, 8> tick_sizes{};
    std::array<std::uint64_t, 8> skip_indices{};
    std::array<std::uint64_t, 8> skip_sizes{};
};

void expect_lowering_failure(
    iv::GraphJitCompileResult const& result,
    std::string_view message_fragment)
{
    EXPECT_TRUE(result.attempted);
    EXPECT_FALSE(result.succeeded());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().stage, iv::GraphJitDiagnosticStage::lowering);
    EXPECT_NE(
        result.diagnostics.front().message.find(message_fragment),
        std::string::npos)
        << result.diagnostics.front().message;
}

} // namespace

TEST(ContinuousValueActivity, RegistryOwnsSemanticEqualityAndClearTests)
{
    auto const& sample = iv::continuous_value_type_info(
        iv::ContinuousValueTypeId::sample);
    iv::Sample const positive_zero{0.0f};
    iv::Sample const negative_zero{-0.0f};
    ASSERT_NE(sample.values_equal, nullptr);
    ASSERT_NE(sample.value_is_clear, nullptr);
    EXPECT_TRUE(sample.values_equal(&positive_zero, &negative_zero, 1));
    EXPECT_TRUE(sample.value_is_clear(&negative_zero, 1));

    auto const& fft = iv::continuous_value_type_info(
        iv::ContinuousValueTypeId::fft_block);
    std::array<iv::FFTBin, 2> first{};
    std::array<iv::FFTBin, 2> second{};
    ASSERT_NE(fft.values_equal, nullptr);
    ASSERT_NE(fft.value_is_clear, nullptr);
    EXPECT_TRUE(fft.values_equal(first.data(), second.data(), first.size()));
    EXPECT_TRUE(fft.value_is_clear(first.data(), first.size()));
    second[1] = iv::FFTBin{1.0f, -2.0f};
    EXPECT_FALSE(fft.values_equal(first.data(), second.data(), first.size()));
    EXPECT_FALSE(fft.value_is_clear(second.data(), second.size()));
}

TEST(GraphJit, RejectsInvalidKernelConfiguration)
{
    EXPECT_THROW(
        iv::GraphJit(iv::GraphJitConfig{.sample_rate = 0, .block_size = 256}),
        std::invalid_argument);
    EXPECT_THROW(
        iv::GraphJit(iv::GraphJitConfig{.sample_rate = 48000, .block_size = 0}),
        std::invalid_argument);
}

TEST(GraphJit, EmptyGraphCompilesAndMaterializesRootOperation)
{
    iv::GraphJit jit;
    auto graph = std::make_shared<iv::ConfiguredGraph const>();
    auto definitions = std::make_shared<iv::NodeDefinitionsSnapshot>();
    definitions->generation = 12;

    auto const result = jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 34,
        .graph = std::move(graph),
        .definitions = std::move(definitions),
    });

    EXPECT_TRUE(result.attempted);
    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(result.diagnostics.empty());
    ASSERT_TRUE(result.compiled_graph);
    EXPECT_EQ(result.compiled_graph->project_generation, 34u);
    EXPECT_EQ(result.compiled_graph->definitions_generation, 12u);
    EXPECT_EQ(result.compiled_graph->node_layout.storage_size, 0u);
    EXPECT_EQ(result.compiled_graph->node_layout.max_block_size, 256u);
    EXPECT_TRUE(result.compiled_graph->node_layout.nodes.empty());
    EXPECT_TRUE(result.compiled_graph->background_evaluation_plan.empty());
    EXPECT_TRUE(result.compiled_graph->root_operations.valid());
    EXPECT_FALSE(result.compiled_graph->background_operations.valid());

    EXPECT_NO_THROW(result.compiled_graph->root_operations.tick_block(nullptr, &empty_tick_invocation, index(0), 256));
}

TEST(GraphJit, MissingPinnedInputsAreCompileDiagnostics)
{
    iv::GraphJit jit;
    auto const result = jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 1,
    });

    EXPECT_TRUE(result.attempted);
    EXPECT_FALSE(result.succeeded());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().stage, iv::GraphJitDiagnosticStage::input_capture);
}

TEST(GraphJitReflectedAbi, ReflectedSpanRoundTripsPointerAndExactExtent)
{
    std::array<std::byte, 7> bytes{};
    iv::ReflectedSpan<std::byte> reflected{std::span<std::byte>{bytes}};

    EXPECT_EQ(reflected.data(), bytes.data());
    EXPECT_EQ(reflected.size(), bytes.size());
    EXPECT_FALSE(reflected.empty());

    std::span<std::byte> round_trip = reflected;
    EXPECT_EQ(round_trip.data(), bytes.data());
    EXPECT_EQ(round_trip.size(), bytes.size());

    iv::ReflectedSpan<std::byte> empty;
    EXPECT_EQ(empty.data(), nullptr);
    EXPECT_EQ(empty.size(), 0u);
    EXPECT_TRUE(empty.empty());
}

TEST(GraphJitSharedRuntimeFixture, BuildPackage)
{
    std::shared_ptr<iv::PackageRevision const> revision;
    ASSERT_NO_THROW(revision = load_graph_jit_runtime_revision());
    ASSERT_TRUE(revision);
    auto has_leaf_definition = [&](std::string_view definition_id) {
        return std::ranges::any_of(
            revision->leaf_definitions,
            [&](auto const& definition) {
                return definition.definition_id == definition_id;
            });
    };
    auto has_module_definition = [&](std::string_view definition_id) {
        return std::ranges::any_of(
            revision->module_definitions,
            [&](auto const& definition) {
                return definition.definition_id == definition_id;
            });
    };
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.mono_interleaved_consumer"));
    EXPECT_TRUE(has_leaf_definition(graph_jit_message_replacement_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_message_successor_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.background_evaluation_call"));
    EXPECT_TRUE(has_module_definition(graph_jit_background_execution_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_multi_region_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_native_batch_tick_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_native_batch_aliasing_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_native_batch_feedback_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_constant_gain_feedback_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_immutable_history_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_distinct_immutable_history_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_native_batch_replay_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_native_batch_background_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_native_batch_activity_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_native_batch_reverse_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_declaration_layout_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_activity_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_activity_disabled_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_activity_type_default_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_event_activity_module_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.stereo_ramp_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.stereo_planar_consumer"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.stereo_sample_consumer"));
    EXPECT_TRUE(has_module_definition(graph_jit_sample_fanout_conversion_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_sample_revision_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_persistent_sample_revision_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_composed_sample_revision_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_stereo_conversion_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_stereo_history_conversion_module_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.history_ramp_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.history_consumer"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.stereo_history_consumer"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.large_history_ramp_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.large_history_consumer"));
    EXPECT_TRUE(has_module_definition(graph_jit_history_fanout_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_persistent_history_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_large_history_rewire_module_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.five_sample_delay"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.latency_compensation_probe"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.interleaved_latency_compensation_probe"));
    EXPECT_TRUE(has_leaf_definition(graph_jit_sample_feedback_a_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_sample_feedback_b_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_projected_revising_sample_feedback_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_multi_branch_sample_feedback_id));
    EXPECT_TRUE(has_module_definition(graph_jit_latency_compensation_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_latency_conversion_fanout_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_composed_latency_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_composed_history_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_projected_composition_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_fft_value_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_fractional_index_value_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_persisted_fft_value_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_persisted_non_project_rate_sample_module_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.trigger_event_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.limited_trigger_event_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.trigger_event_consumer"));
    EXPECT_TRUE(has_leaf_definition(graph_jit_latent_event_feedback_a_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_persistent_latent_event_feedback_a_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_persistent_latent_boundary_event_feedback_a_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_boundary_event_feedback_a_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_boundary_event_feedback_b_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_retained_dual_event_feedback_a_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_dual_event_feedback_b_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.limited_trigger_event_consumer"));
    EXPECT_TRUE(has_module_definition(graph_jit_direct_event_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_transient_event_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_converted_event_fanout_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_retained_event_module_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.persistent_event_ring_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.persistent_event_ring_consumer"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.persistent_event_feedback_consumer"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.retained_midi_event_consumer"));
    EXPECT_TRUE(has_module_definition(graph_jit_persistent_event_ring_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_retained_converted_event_fanout_module_id));

    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    ASSERT_TRUE(session);
    auto const package_root = revision->package_root.generic_string();
    std::array packages{iv::details::BuilderPackageView{
        .package_root = package_root,
        .definitions = revision->provider_definitions,
        .config_pointer_fields = revision->config_pointer_fields,
        .retained_globals = revision->retained_globals,
        .node_message_handlers = revision->node_message_handlers,
        .node_state_structures = revision->node_state_structures,
    }};
    iv::details::set_builder_packages(session.get(), packages);

    iv::GraphBuilder graph(session.get());
    (void)iv::details::configure_package_definition_provider(
        graph, graph_jit_message_replacement_id, std::nullopt, {});
    graph.outputs();
    auto handlers = iv::details::take_builder_message_handlers(session.get());
    (void)iv::details::take_built_graph(session.get());
    ASSERT_EQ(handlers.size(), 1u);
    ASSERT_NE(handlers[0].event_type, nullptr);
    ASSERT_NE(handlers[0].invoke, nullptr);
    ASSERT_TRUE(handlers[0].receiver);

    auto event = iv::evaluate_configuration_json_value(
        nlohmann::ordered_json{{"delta", 2}},
        *handlers[0].event_type);
    struct ReplacementCapture {
        iv::NodeAddress address{};
        std::string definition_id{};
        std::string arguments_json{};
    } capture;
    iv::NodeMessageContextOperations const operations{
        .context = &capture,
        .replace = [](
            void* opaque,
            iv::NodeAddress const& address,
            std::string definition_id,
            std::string arguments_json) {
            auto& value = *static_cast<ReplacementCapture*>(opaque);
            value.address = address;
            value.definition_id = std::move(definition_id);
            value.arguments_json = std::move(arguments_json);
        },
    };
    iv::NodeAddress const address{.instance_id = "instance:message"};
    handlers[0].invoke(
        handlers[0].receiver.get(),
        operations,
        address,
        handlers[0].module_local_refs.get(),
        event.data());

    EXPECT_EQ(capture.address, address);
    EXPECT_EQ(capture.definition_id, graph_jit_message_successor_id);
    EXPECT_EQ(
        nlohmann::ordered_json::parse(capture.arguments_json),
        nlohmann::ordered_json::array({42}));
}

TEST_F(
    GraphJitRuntimeFixture,
    DeclarationOwnedStorageAndNestedNodesPreservePrimitiveStateMapping)
{
    auto compiled = compile(graph_jit_declaration_layout_module_id, 105);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->root_operations.valid());

    auto const& layout = compiled.compiled_graph->node_layout;
    auto const& tock_layout = compiled.compiled_graph->tock_state_layout;
    ASSERT_EQ(layout.nodes.size(), 5u);
    ASSERT_EQ(tock_layout.nodes.size(), 5u);

    auto storage = layout.create_storage(resources);
    storage.initialize();
    auto tock_states = tock_layout.create_storage(resources);
    tock_states.initialize();

    auto* parent = static_cast<DeclarationLayoutParentStateMirror*>(
        storage.state_ptr(0));
    auto* producer = static_cast<DeclarationLayoutProducerStateMirror*>(
        storage.state_ptr(3));
    auto* consumer = static_cast<DeclarationLayoutConsumerStateMirror*>(
        storage.state_ptr(4));
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(producer, nullptr);
    ASSERT_NE(consumer, nullptr);
    ASSERT_EQ(parent->local.size(), 3u);
    ASSERT_EQ(parent->nested.size(), 2u);
    ASSERT_EQ(producer->values.size(), 3u);
    ASSERT_EQ(consumer->imported.size(), 3u);
    EXPECT_EQ(consumer->imported.data(), producer->values.data());
    EXPECT_EQ(consumer->initialize_sum, 41u);

    auto const* first_leaf =
        reinterpret_cast<DeclarationLayoutLeafStateMirror const*>(
            parent->nested[0].data());
    auto const* second_leaf =
        reinterpret_cast<DeclarationLayoutLeafStateMirror const*>(
            parent->nested[1].data());
    ASSERT_NE(first_leaf, nullptr);
    ASSERT_NE(second_leaf, nullptr);
    EXPECT_EQ(first_leaf->marker, 17u);
    EXPECT_EQ(second_leaf->marker, 17u);

    auto const* first_tock_leaf =
        static_cast<DeclarationLayoutLeafTockStateMirror const*>(
            tock_states.state_ptr(1));
    auto const* second_tock_leaf =
        static_cast<DeclarationLayoutLeafTockStateMirror const*>(
            tock_states.state_ptr(2));
    ASSERT_NE(first_tock_leaf, nullptr);
    ASSERT_NE(second_tock_leaf, nullptr);
    EXPECT_EQ(first_tock_leaf->marker, 23u);
    EXPECT_EQ(second_tock_leaf->marker, 23u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(7), 16);
    EXPECT_EQ(parent->calls, 1u);
    EXPECT_EQ(parent->local_sum, 10u);
    EXPECT_EQ(parent->nested_sum, 34u);
    EXPECT_EQ(producer->calls, 1u);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_EQ(consumer->tick_sum, 41u);
}

TEST_F(GraphJitRuntimeFixture, ConfiguredValuesAndPointerRelocations)
{
    auto configured_graph = configured_module_graph(*revision, graph_jit_configured_module_id);
    ASSERT_TRUE(configured_graph);
    auto configured = jit->compile(iv::GraphJitCompileRequest{
        .project_generation = 104,
        .graph = configured_graph,
        .definitions = definitions,
    });
    ASSERT_TRUE(configured.succeeded())
        << (configured.diagnostics.empty() ? "" : configured.diagnostics.front().message);
    EXPECT_EQ(configured.compiled_graph->project_generation, 104u);
    EXPECT_EQ(configured.compiled_graph->definitions_generation, 91u);
    EXPECT_EQ(configured.compiled_graph->configured_graph, configured_graph);
    ASSERT_EQ(configured.compiled_graph->package_revisions.size(), 1u);
    EXPECT_EQ(configured.compiled_graph->package_revisions.front(), revision);
    expect_single_node_canonical_regions(
        configured.compiled_graph->node_layout,
        configured.compiled_graph->tock_state_layout,
        sizeof(ConfiguredProbeStateMirror),
        0);
    auto configured_storage = configured.compiled_graph->node_layout.create_storage(resources);
    configured_storage.initialize();
    auto* configured_state = static_cast<ConfiguredProbeStateMirror*>(
        configured_storage.state_ptr(0));
    ASSERT_NE(configured_state, nullptr);
    configured.compiled_graph->root_operations.tick_block(
        configured_storage.buffer().data(), &empty_tick_invocation, index(11), 32);
    EXPECT_EQ(configured_state->calls, 1u);
    EXPECT_EQ(configured_state->first, std::size_t{0x12345678u});
    EXPECT_EQ(configured_state->second, std::size_t{0xabcdef01u});

    auto pointer_graph = configured_module_graph(
        *revision, graph_jit_pointer_configured_module_id);
    ASSERT_TRUE(pointer_graph);
    std::size_t pointer_relocation_count = 0;
    pointer_graph->node_bundles.for_each_configured_bundle(
        [&](iv::ConfiguredNodeBundleView const& view) {
            if (view.kind != iv::ConfiguredNodeBundleKind::concrete
                || !view.config_relocations) {
                return;
            }
            pointer_relocation_count += view.config_relocations->size();
        });
    ASSERT_EQ(pointer_relocation_count, 3u);
    auto pointer_configured = compile_graph(pointer_graph, 105);
    ASSERT_TRUE(pointer_configured.succeeded())
        << (pointer_configured.diagnostics.empty()
                ? ""
                : pointer_configured.diagnostics.front().message);
    expect_single_node_canonical_regions(
        pointer_configured.compiled_graph->node_layout,
        pointer_configured.compiled_graph->tock_state_layout,
        sizeof(PointerConfiguredProbeStateMirror),
        0);
    auto pointer_storage =
        pointer_configured.compiled_graph->node_layout.create_storage(resources);
    pointer_storage.initialize();
    auto* pointer_state = static_cast<PointerConfiguredProbeStateMirror*>(
        pointer_storage.state_ptr(0));
    ASSERT_NE(pointer_state, nullptr);
    pointer_configured.compiled_graph->root_operations.tick_block(
        pointer_storage.buffer().data(), &empty_tick_invocation, index(13), 16);
    EXPECT_EQ(pointer_state->calls, 1u);
    EXPECT_EQ(pointer_state->first_value, 0x2222222222222222ull);
    EXPECT_EQ(pointer_state->second_value, 0x4444444444444444ull);
    EXPECT_EQ(pointer_state->null_seen, 1u);
    EXPECT_EQ(pointer_state->marker, 0x89abcdefu);
    EXPECT_EQ(pointer_state->tag, 0x4567u);

}

TEST_F(GraphJitRuntimeFixture, MultipleNodesAndBlockSlicing)
{
    auto multiple_graph = configured_module_graph(
        *revision, graph_jit_multiple_module_id);
    ASSERT_TRUE(multiple_graph);
    auto disconnected_multiple_graph =
        std::make_shared<iv::ConfiguredGraph>(*multiple_graph);
    disconnected_multiple_graph->connections = {};
    auto multiple = compile_graph(disconnected_multiple_graph, 106);
    ASSERT_TRUE(multiple.succeeded())
        << (multiple.diagnostics.empty() ? "" : multiple.diagnostics.front().message);
    ASSERT_EQ(multiple.compiled_graph->node_layout.nodes.size(), 2u);
    EXPECT_EQ(
        multiple.compiled_graph->node_layout.nodes[0].state_size,
        sizeof(StatefulProbeStateMirror));
    EXPECT_EQ(
        multiple.compiled_graph->tock_state_layout.nodes[0].state_size,
        sizeof(StatefulProbeBackgroundStateMirror));
    EXPECT_EQ(
        multiple.compiled_graph->node_layout.nodes[1].state_size,
        sizeof(ConfiguredProbeStateMirror));
    auto multiple_storage = multiple.compiled_graph->node_layout.create_storage(resources);
    multiple_storage.initialize();
    auto* multiple_stateful_state = static_cast<StatefulProbeStateMirror*>(
        multiple_storage.state_ptr(0));
    auto* multiple_configured_state = static_cast<ConfiguredProbeStateMirror*>(
        multiple_storage.state_ptr(1));
    ASSERT_NE(multiple_stateful_state, nullptr);
    ASSERT_NE(multiple_configured_state, nullptr);
    multiple.compiled_graph->root_operations.tick_block(
        multiple_storage.buffer().data(), &empty_tick_invocation, index(19), 32);
    EXPECT_EQ(multiple_stateful_state->tick_calls, 1u);
    EXPECT_EQ(multiple_stateful_state->last_index, 19u);
    EXPECT_EQ(multiple_stateful_state->last_block_size, 32u);
    EXPECT_EQ(multiple_configured_state->calls, 1u);
    EXPECT_EQ(multiple_configured_state->first, 1u);
    EXPECT_EQ(multiple_configured_state->second, 2u);

    auto skippable_pair_graph = configured_module_graph(
        *revision, graph_jit_skippable_pair_module_id);
    ASSERT_TRUE(skippable_pair_graph);
    auto disconnected_skippable_pair_graph =
        std::make_shared<iv::ConfiguredGraph>(*skippable_pair_graph);
    disconnected_skippable_pair_graph->connections = {};
    auto skippable_pair = compile_graph(disconnected_skippable_pair_graph, 107);
    ASSERT_TRUE(skippable_pair.succeeded())
        << (skippable_pair.diagnostics.empty()
                ? ""
                : skippable_pair.diagnostics.front().message);
    ASSERT_EQ(skippable_pair.compiled_graph->node_layout.nodes.size(), 2u);
    auto skippable_pair_storage =
        skippable_pair.compiled_graph->node_layout.create_storage(resources);
    skippable_pair_storage.initialize();
    auto* pair_state_0 = static_cast<StatefulProbeStateMirror*>(
        skippable_pair_storage.state_ptr(0));
    auto* pair_state_1 = static_cast<StatefulProbeStateMirror*>(
        skippable_pair_storage.state_ptr(1));
    ASSERT_NE(pair_state_0, nullptr);
    ASSERT_NE(pair_state_1, nullptr);
    skippable_pair.compiled_graph->root_operations.tick_block(
        skippable_pair_storage.buffer().data(), &empty_tick_invocation, index(23), 32);
    EXPECT_EQ(pair_state_0->tick_calls, 1u);
    EXPECT_EQ(pair_state_1->tick_calls, 1u);
    auto limited = compile(graph_jit_limited_block_module_id, 108);
    ASSERT_TRUE(limited.succeeded())
        << (limited.diagnostics.empty() ? "" : limited.diagnostics.front().message);
    expect_single_node_canonical_regions(
        limited.compiled_graph->node_layout,
        limited.compiled_graph->tock_state_layout,
        sizeof(LimitedBlockProbeStateMirror),
        0);
    auto limited_storage = limited.compiled_graph->node_layout.create_storage(resources);
    limited_storage.initialize();
    auto* limited_state = static_cast<LimitedBlockProbeStateMirror*>(
        limited_storage.state_ptr(0));
    ASSERT_NE(limited_state, nullptr);

    limited.compiled_graph->root_operations.tick_block(
        limited_storage.buffer().data(), &empty_tick_invocation, index(200), 64);
    EXPECT_EQ(limited_state->tick_calls, 4u);
    EXPECT_EQ(
        limited_state->tick_indices,
        (std::array<std::uint64_t, 8>{200, 216, 232, 248, 0, 0, 0, 0}));
    EXPECT_EQ(
        limited_state->tick_sizes,
        (std::array<std::uint64_t, 8>{16, 16, 16, 16, 0, 0, 0, 0}));

    limited.compiled_graph->root_operations.tick_block(
        limited_storage.buffer().data(), &empty_tick_invocation, index(300), 8);
    EXPECT_EQ(limited_state->tick_calls, 5u);
    EXPECT_EQ(limited_state->tick_indices[4], 300u);
    EXPECT_EQ(limited_state->tick_sizes[4], 8u);

}

TEST_F(GraphJitRuntimeFixture, NativeTickCallbacksReceiveReadyFrontierBatch)
{
    // The interpreter batches the steps of an acyclic ready group that have
    // no region specialization; compiling without them exercises it here.
    iv::GraphJit interpreter_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .specialize_regions = false,
    });
    auto compiled = interpreter_jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 110,
        .graph = configured_module_graph(
            *revision, graph_jit_native_batch_tick_module_id),
        .definitions = definitions,
    });
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->root_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* first = static_cast<NativeBatchTickProbeStateMirror*>(
        storage.state_ptr(0));
    auto* second = static_cast<NativeBatchTickProbeStateMirror*>(
        storage.state_ptr(1));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(41), 32);
    EXPECT_EQ(first->tick_batch_calls, 1u);
    EXPECT_EQ(first->skip_batch_calls, 0u);
    EXPECT_EQ(first->last_lane_count, 2u);
    EXPECT_EQ(first->configured_id, 11u);
    EXPECT_EQ(first->last_index, 41u);
    EXPECT_EQ(first->last_block_size, 32u);
    EXPECT_EQ(second->tick_batch_calls, 1u);
    EXPECT_EQ(second->skip_batch_calls, 0u);
    EXPECT_EQ(second->last_lane_count, 2u);
    EXPECT_EQ(second->configured_id, 22u);

}

TEST_F(GraphJitRuntimeFixture, NativeTickBatchScalarizesAliasedCallbackStorage)
{
    // The interpreter's native batch path; see
    // NativeTickCallbacksReceiveReadyFrontierBatch.
    iv::GraphJit interpreter_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .specialize_regions = false,
    });
    auto compiled = interpreter_jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 111,
        .graph = configured_module_graph(
            *revision, graph_jit_native_batch_aliasing_module_id),
        .definitions = definitions,
    });
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->root_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* first = static_cast<NativeBatchAliasingProbeStateMirror*>(
        storage.state_ptr(0));
    auto* second = static_cast<NativeBatchAliasingProbeStateMirror*>(
        storage.state_ptr(1));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(41), 32);
    EXPECT_EQ(first->tick_batch_calls, 1u);
    EXPECT_EQ(first->last_lane_count, 1u);
    EXPECT_EQ(first->configured_id, 11u);
    EXPECT_EQ(second->tick_batch_calls, 1u);
    EXPECT_EQ(second->last_lane_count, 1u);
    EXPECT_EQ(second->configured_id, 22u);
}

TEST_F(GraphJitRuntimeFixture, IndependentFeedbackRegionsBatchEachExecutionPhase)
{
    // The interpreter's cyclic batching runs every region that has no region
    // specialization; compiling without them exercises it here.
    iv::GraphJit interpreter_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .specialize_regions = false,
    });
    auto compiled = interpreter_jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 112,
        .graph = configured_module_graph(
            *revision, graph_jit_native_batch_feedback_module_id),
        .definitions = definitions,
    });
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->root_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 4u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    std::array<NativeBatchFeedbackProbeStateMirror*, 4> states{};
    for (std::size_t node = 0; node < states.size(); ++node) {
        states[node] = static_cast<NativeBatchFeedbackProbeStateMirror*>(
            storage.state_ptr(node));
        ASSERT_NE(states[node], nullptr);
    }

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);

    for (auto const* state : states) {
        EXPECT_EQ(state->tick_batch_calls, 4u);
        EXPECT_EQ(state->last_lane_count, 2u);
    }
    EXPECT_EQ(states[0]->configured_id, 1u);
    EXPECT_EQ(states[1]->configured_id, 2u);
    EXPECT_EQ(states[2]->configured_id, 10u);
    EXPECT_EQ(states[3]->configured_id, 20u);
    EXPECT_FLOAT_EQ(states[0]->last_output, 10.0f);
    EXPECT_FLOAT_EQ(states[1]->last_output, 12.0f);
    EXPECT_FLOAT_EQ(states[2]->last_output, 100.0f);
    EXPECT_FLOAT_EQ(states[3]->last_output, 120.0f);

    // Each compact carry is restored once before the complete sliced region
    // and committed once after it. The next root call must continue from the
    // final value of the preceding call, not from an intermediate slice.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    for (auto const* state : states) {
        EXPECT_EQ(state->tick_batch_calls, 8u);
        EXPECT_EQ(state->last_lane_count, 2u);
    }
    EXPECT_FLOAT_EQ(states[0]->last_output, 22.0f);
    EXPECT_FLOAT_EQ(states[1]->last_output, 24.0f);
    EXPECT_FLOAT_EQ(states[2]->last_output, 220.0f);
    EXPECT_FLOAT_EQ(states[3]->last_output, 240.0f);
}

TEST_F(GraphJitRuntimeFixture, ActivityPartitionsReadyFrontierIntoTickAndSkipBatches)
{
    auto compiled = compile(graph_jit_native_batch_activity_module_id, 113);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->root_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 3u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* short_ttl = static_cast<NativeBatchActivityProbeStateMirror*>(
        storage.state_ptr(0));
    auto* long_ttl = static_cast<NativeBatchActivityProbeStateMirror*>(
        storage.state_ptr(1));
    ASSERT_NE(short_ttl, nullptr);
    ASSERT_NE(long_ttl, nullptr);

    // Without inputs, each lane becomes dormant once its cleared outputs have
    // been quiet for its TTL. Block 64 splits one ready group into a one-lane
    // Tick batch and a one-lane skip batch. Blocks 128 and 192 skip both lanes
    // in one batch.
    for (auto const begin : {0u, 64u, 128u, 192u}) {
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(),
            &empty_tick_invocation,
            index(begin),
            64);
    }
    EXPECT_EQ(short_ttl->tick_batch_calls, 1u);
    EXPECT_EQ(short_ttl->skip_batch_calls, 3u);
    EXPECT_EQ(short_ttl->tick_lane_total, 2u);
    EXPECT_EQ(short_ttl->skip_lane_total, 5u);
    EXPECT_EQ(long_ttl->tick_batch_calls, 2u);
    EXPECT_EQ(long_ttl->skip_batch_calls, 2u);
    EXPECT_EQ(long_ttl->tick_lane_total, 3u);
    EXPECT_EQ(long_ttl->skip_lane_total, 4u);
}

TEST_F(GraphJitRuntimeFixture, BatchOnlyTickCallbackExecutesThroughReplayScalarAdapter)
{
    auto compiled = compile(graph_jit_native_batch_replay_module_id, 112);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->background_operations.valid());

    auto const& plan = compiled.compiled_graph->background_evaluation_plan;
    ASSERT_EQ(plan.runtime.replay_invocations.size(), 1u);
    auto const replay_node = plan.runtime.replay_invocations.front().node;
    ASSERT_LT(replay_node, plan.nodes.size());

    std::array<iv::Sample, 8> samples{};
    std::array output_bindings{iv::ReflectedContinuousOutputPortBinding{
        .storage = sample_storage_binding(samples),
    }};
    std::array replay_regions{iv::GlobalIndexRange{
        .begin = index(40),
        .extent = 3,
    }};
    auto const no_op = +[](void*) {};
    std::vector<iv::graph_jit::BackgroundNodeCall> node_frames(
        plan.nodes.size());
    node_frames[replay_node] = iv::graph_jit::BackgroundNodeCall{
        .activity = iv::graph_jit::BackgroundNodeActivity::evaluate,
        .prepare_operations = no_op,
        .finalize_operations = no_op,
        .replay = iv::ReflectedNodeTickContext{
            .continuous_output_bindings = output_bindings,
            .sample_rate = 88200,
        },
        .replay_regions = replay_regions,
    };
    iv::graph_jit::BackgroundEvaluationCall batch{.nodes = node_frames};

    compiled.compiled_graph->background_operations.evaluate(&batch);

    EXPECT_EQ(samples, (std::array<iv::Sample, 8>{
        51.0f, 52.0f, 53.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}));
}

TEST_F(GraphJitRuntimeFixture, ActivityTtlIsOptInAndWakesOnInputChange)
{
    auto enabled = compile(graph_jit_activity_module_id, 120);
    ASSERT_TRUE(enabled.succeeded())
        << (enabled.diagnostics.empty()
                ? ""
                : enabled.diagnostics.front().message);
    ASSERT_EQ(enabled.compiled_graph->node_layout.nodes.size(), 3u);
    std::vector<std::size_t> activity_region_sizes;
    for (auto const& region : enabled.compiled_graph->node_layout.regions) {
        if (region.kind == iv::NodeLayout::Region::Kind::raw
            && region.migration_identity.starts_with(
                "graphjit.activity:")) {
            activity_region_sizes.push_back(region.size);
        }
    }
    ASSERT_EQ(activity_region_sizes.size(), 2u);
    EXPECT_EQ(
        std::ranges::count(
            activity_region_sizes,
            sizeof(iv::graph_jit::detail::PrimitiveActivityState)),
        1u);
    EXPECT_EQ(
        std::ranges::count_if(
            activity_region_sizes,
            [](std::size_t size) {
                return size
                    > sizeof(iv::graph_jit::detail::PrimitiveActivityState);
            }),
        1u);
    auto enabled_storage =
        enabled.compiled_graph->node_layout.create_storage(resources);
    enabled_storage.initialize();
    auto* first_enabled_state = static_cast<ActivityProbeStateMirror*>(
        enabled_storage.state_ptr(1));
    auto* second_enabled_state = static_cast<ActivityProbeStateMirror*>(
        enabled_storage.state_ptr(2));
    ASSERT_NE(first_enabled_state, nullptr);
    ASSERT_NE(second_enabled_state, nullptr);

    for (auto const begin : {0u, 64u, 128u, 192u, 256u}) {
        enabled.compiled_graph->root_operations.tick_block(
            enabled_storage.buffer().data(),
            &empty_tick_invocation,
            index(begin),
            64);
    }
    EXPECT_EQ(first_enabled_state->tick_calls, 4u);
    EXPECT_EQ(first_enabled_state->skip_calls, 1u);
    EXPECT_EQ(second_enabled_state->tick_calls, 4u);
    EXPECT_EQ(second_enabled_state->skip_calls, 1u);

    auto disabled = compile(graph_jit_activity_disabled_module_id, 121);
    ASSERT_TRUE(disabled.succeeded())
        << (disabled.diagnostics.empty()
                ? ""
                : disabled.diagnostics.front().message);
    ASSERT_EQ(disabled.compiled_graph->node_layout.nodes.size(), 2u);
    auto disabled_storage =
        disabled.compiled_graph->node_layout.create_storage(resources);
    disabled_storage.initialize();
    auto* disabled_state = static_cast<ActivityProbeStateMirror*>(
        disabled_storage.state_ptr(1));
    ASSERT_NE(disabled_state, nullptr);

    disabled.compiled_graph->root_operations.tick_block(
        disabled_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        64);
    disabled.compiled_graph->root_operations.tick_block(
        disabled_storage.buffer().data(),
        &empty_tick_invocation,
        index(64),
        64);
    EXPECT_EQ(disabled_state->tick_calls, 2u);
    EXPECT_EQ(disabled_state->skip_calls, 0u);

    auto type_default = compile(graph_jit_activity_type_default_module_id, 122);
    ASSERT_TRUE(type_default.succeeded())
        << (type_default.diagnostics.empty()
                ? ""
                : type_default.diagnostics.front().message);
    auto type_default_storage =
        type_default.compiled_graph->node_layout.create_storage(resources);
    type_default_storage.initialize();
    auto* type_default_state = static_cast<ActivityProbeStateMirror*>(
        type_default_storage.state_ptr(1));
    ASSERT_NE(type_default_state, nullptr);
    type_default.compiled_graph->root_operations.tick_block(
        type_default_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        64);
    type_default.compiled_graph->root_operations.tick_block(
        type_default_storage.buffer().data(),
        &empty_tick_invocation,
        index(64),
        64);
    EXPECT_EQ(type_default_state->tick_calls, 1u);
    EXPECT_EQ(type_default_state->skip_calls, 1u);
}

TEST_F(GraphJitRuntimeFixture, EventActivityWakesOnANonemptyInputWindow)
{
    auto compiled = compile(graph_jit_event_activity_module_id, 258);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<ActivityProbeStateMirror*>(
        storage.state_ptr(1));
    ASSERT_NE(state, nullptr);

    for (auto const begin : {0u, 64u, 128u}) {
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(),
            &empty_tick_invocation,
            index(begin),
            64);
    }
    EXPECT_EQ(state->tick_calls, 2u);
    EXPECT_EQ(state->skip_calls, 1u);
}

TEST_F(GraphJitRuntimeFixture, RetainedStereoToMonoHistoryRecordsItsSources)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_stereo_history_conversion_module_id);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);
    ASSERT_TRUE(analysis->sample_producer_groups.front().storage_plan);
    EXPECT_EQ(
        analysis->sample_producer_groups.front().storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);

    auto compiled = compile_graph(graph, 229);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& requirements = compiled.compiled_graph
        ->realtime_port_state_requirements.states;
    auto const& realizations = compiled.compiled_graph
        ->realtime_port_state_realizations.sample_states;
    ASSERT_EQ(requirements.size(), 1u);
    ASSERT_EQ(realizations.size(), 1u);
    auto const& realization = realizations.front();
    ASSERT_LT(realization.requirement_index, requirements.size());
    EXPECT_EQ(
        requirements[realization.requirement_index].role,
        iv::graph_jit::RealtimePortStateRole::sequential_input_history);
    EXPECT_EQ(
        requirements[realization.requirement_index].extent_samples,
        5u);
    EXPECT_EQ(
        realization.authored_storage.storage,
        iv::graph_jit::SampleRealtimePortStateStorage::callback_transient);
    EXPECT_EQ(
        realization.authored_storage.channel_layout.channel_type,
        iv::ChannelTypeId::mono);
    EXPECT_EQ(
        realization.retained_access,
        iv::graph_jit::SampleRealtimePortStateAccess::materialized);
    ASSERT_TRUE(realization.materialization);
    EXPECT_EQ(
        realization.materialization->source_layout.channel_type,
        iv::ChannelTypeId::stereo);
    EXPECT_EQ(
        realization.materialization->target_layout.channel_type,
        iv::ChannelTypeId::mono);
    EXPECT_EQ(realization.materialization->retained_before, 5u);
    EXPECT_EQ(realization.materialization->latest_read_latency, 0u);
    ASSERT_EQ(realization.retained_sources.size(), 2u);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        auto const& source = realization.retained_sources[channel];
        EXPECT_EQ(
            source.storage,
            iv::graph_jit::SampleRealtimePortStateStorage::compact_carry);
        EXPECT_EQ(source.channel_layout.channel_type, iv::ChannelTypeId::stereo);
        EXPECT_EQ(source.representation_channel, channel);
        EXPECT_EQ(source.timeline_offset_frames, 0);
        ASSERT_TRUE(source.node_storage_offset);
        EXPECT_FALSE(source.callback_arena_offset);
    }
}

TEST_F(GraphJitRuntimeFixture, RootGraphBoundaryPortsRejected)
{
    auto ported_graph = configured_module_graph(*revision, graph_jit_ported_module_id);
    ASSERT_TRUE(ported_graph);
    auto ported = compile_graph(ported_graph, 119);
    expect_lowering_failure(ported, "root graph must not declare boundary ports");

    auto disconnected_ported_graph =
        std::make_shared<iv::ConfiguredGraph>(*ported_graph);
    disconnected_ported_graph->connections = {};
    auto disconnected_ported = compile_graph(disconnected_ported_graph, 120);
    expect_lowering_failure(
        disconnected_ported, "root graph must not declare boundary ports");
}

TEST_F(GraphJitRuntimeFixture, CompiledGraphsRetainPackageAndOrcOwnership)
{
    auto stateful = compile(graph_jit_stateful_module_id, 130);
    ASSERT_TRUE(stateful.succeeded())
        << (stateful.diagnostics.empty() ? "" : stateful.diagnostics.front().message);
    auto stateful_survivor = stateful.compiled_graph;
    auto stateful_storage = stateful_survivor->node_layout.create_storage(resources);
    stateful_storage.initialize();
    auto stateful_tock_states =
        stateful_survivor->tock_state_layout.create_storage(resources);
    stateful_tock_states.initialize();
    auto* state = static_cast<StatefulProbeStateMirror*>(stateful_storage.state_ptr(0));
    auto* background = static_cast<StatefulProbeBackgroundStateMirror*>(
        stateful_tock_states.state_ptr(0));
    ASSERT_NE(state, nullptr);
    ASSERT_NE(background, nullptr);
    stateful_survivor->root_operations.tick_block(
        stateful_storage.buffer().data(), &empty_tick_invocation, index(17), 32);
    stateful_survivor->root_operations.tick_block(
        stateful_storage.buffer().data(), &empty_tick_invocation, index(73), 64);

    auto pointer_graph = configured_module_graph(
        *revision, graph_jit_pointer_configured_module_id);
    ASSERT_TRUE(pointer_graph);
    auto pointer_result = compile_graph(pointer_graph, 131);
    ASSERT_TRUE(pointer_result.succeeded())
        << (pointer_result.diagnostics.empty()
                ? ""
                : pointer_result.diagnostics.front().message);
    auto pointer_survivor = pointer_result.compiled_graph;

    auto revision_weak = std::weak_ptr<iv::PackageRevision const>{revision};
    stateful = {};
    pointer_result = {};
    definitions.reset();
    revision.reset();
    jit.reset();
    EXPECT_FALSE(revision_weak.expired());
    ASSERT_EQ(stateful_survivor->package_revisions.size(), 1u);

    stateful_survivor->root_operations.tick_block(
        stateful_storage.buffer().data(), &empty_tick_invocation, index(137), 32);
    EXPECT_EQ(state->tick_calls, 3u);
    EXPECT_EQ(state->skip_calls, 0u);
    EXPECT_EQ(state->last_index, 137u);
    EXPECT_EQ(state->last_block_size, 32u);
    EXPECT_EQ(background->tick_calls, 0u);
    EXPECT_EQ(background->skip_calls, 0u);
    EXPECT_EQ(background->last_index, 0u);
    EXPECT_EQ(background->last_block_size, 0u);

    auto pointer_survivor_storage =
        pointer_survivor->node_layout.create_storage(resources);
    pointer_survivor_storage.initialize();
    auto* pointer_survivor_state = static_cast<PointerConfiguredProbeStateMirror*>(
        pointer_survivor_storage.state_ptr(0));
    ASSERT_NE(pointer_survivor_state, nullptr);
    pointer_survivor->root_operations.tick_block(
        pointer_survivor_storage.buffer().data(), &empty_tick_invocation, index(149), 32);
    EXPECT_EQ(pointer_survivor_state->calls, 1u);
    EXPECT_EQ(pointer_survivor_state->first_value, 0x2222222222222222ull);
    EXPECT_EQ(pointer_survivor_state->second_value, 0x4444444444444444ull);
    EXPECT_EQ(pointer_survivor_state->null_seen, 1u);
    EXPECT_EQ(pointer_survivor_state->marker, 0x89abcdefu);
    EXPECT_EQ(pointer_survivor_state->tag, 0x4567u);
}

TEST(GraphJitProjectGraphBridge, EmptyRootGenerationCompilesSuccessfully)
{
    iv::NodeInstances instances;
    iv::GraphConnections connections;
    iv::ProjectGraph project_graph;
    iv::GraphJit graph_jit;
    iv::GraphExecutor graph_executor;
    auto instances_scope = iv::project_graph_node_instances_bridge::bind(
        project_graph, instances);
    auto connections_scope = iv::project_graph_graph_connections_bridge::bind(
        project_graph, connections);
    auto graph_jit_scope = iv::project_graph_graph_jit_bridge::bind(
        project_graph, graph_jit);
    auto graph_executor_scope = iv::project_graph_graph_executor_bridge::bind(
        project_graph, graph_executor);

    auto snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>();
    snapshot->generation = 1;
    EXPECT_NO_THROW(publish_definitions_snapshot(project_graph, 
        iv::NodeDefinitionsSnapshotChanged{.snapshot = snapshot}));

    auto const generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->generation, 1u);
    EXPECT_EQ(generation->definitions_generation, 1u);
    EXPECT_TRUE(generation->graph_jit_attempted);
    ASSERT_TRUE(generation->compiled_graph);
    EXPECT_TRUE(generation->graph_jit_diagnostics.empty());
    EXPECT_TRUE(generation->graph_executor_attempted);
    EXPECT_TRUE(generation->graph_executor_accepted);
    EXPECT_EQ(graph_executor.pending_generation(), 1u);
    EXPECT_EQ(generation->compiled_graph->project_generation, generation->generation);
    EXPECT_EQ(
        generation->compiled_graph->definitions_generation,
        generation->definitions_generation);
    EXPECT_EQ(generation->compiled_graph->node_layout.storage_size, 0u);
    EXPECT_TRUE(generation->compiled_graph->root_operations.valid());
    EXPECT_TRUE(graph_executor.activate_pending(index(0)));
    EXPECT_EQ(graph_executor.active_generation(), 1u);
}

TEST(GraphJitProjectGraphBridge, UnboundSingletonReturnLeavesCompilationUnattempted)
{
    iv::NodeInstances instances;
    iv::GraphConnections connections;
    iv::ProjectGraph project_graph;
    auto instances_scope = iv::project_graph_node_instances_bridge::bind(
        project_graph, instances);
    auto connections_scope = iv::project_graph_graph_connections_bridge::bind(
        project_graph, connections);

    auto snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>();
    snapshot->generation = 1;
    EXPECT_NO_THROW(publish_definitions_snapshot(project_graph, 
        iv::NodeDefinitionsSnapshotChanged{.snapshot = snapshot}));

    auto const generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_FALSE(generation->graph_jit_attempted);
    EXPECT_FALSE(generation->compiled_graph);
    EXPECT_TRUE(generation->graph_jit_diagnostics.empty());
}

} // namespace iv::test::graph_jit_tests
