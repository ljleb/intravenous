#include "test_support.h"

namespace iv::test::graph_jit_tests {

namespace {

constexpr char graph_jit_manual_control_slice_module_id[] =
    "iv.test.graph_jit.state_context.manual_control_slice_module";

struct ManualControlSliceProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t value_count = 0;
    std::array<float, 12> values{};
    std::array<float, 6> history_before_slice{};
};

std::shared_ptr<iv::ConfiguredGraph const> configured_module_manual_fanout(
    iv::PackageRevision const& revision, bool dynamic, bool convert = true)
{
    using namespace iv;
    using Session = std::unique_ptr<iv::details::BuilderSession, decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(iv::details::iv_builder_session_create(), iv::details::iv_builder_session_destroy);
    if (!session) throw std::runtime_error("could not create module manual-control test session");
    auto const root = revision.package_root.generic_string();
    std::array packages{iv::details::BuilderPackageView{
        .package_root = root, .definitions = revision.provider_definitions,
        .config_pointer_fields = revision.config_pointer_fields,
        .retained_globals = revision.retained_globals,
        .node_state_structures = revision.node_state_structures,
    }};
    iv::details::set_builder_packages(session.get(), packages);
    iv::GraphBuilder graph(session.get());
    auto configure = [&]<class... Args>(std::string_view id, Args&&... values) {
        auto const definition = std::ranges::find_if(
            revision.provider_definitions, [&](auto const& entry) {
                return std::string_view(entry.id, entry.id_size) == id;
            });
        if (definition == revision.provider_definitions.end() || !definition->signature) {
            throw std::runtime_error("module manual-control fixture definition has no signature");
        }
        auto const* signature = definition->signature();
        auto arguments = iv::details::configuration_arguments(std::forward<Args>(values)...);
        if (!signature || signature->argument_count != arguments.size()) {
            throw std::runtime_error("module manual-control fixture argument count mismatch");
        }
        // As in configured_module_graph_with_arguments(), host code has no
        // finalized type identities. These callers supply each parameter's
        // exact C++ type and use the package's identity for that parameter.
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            arguments[i].type = signature->parameter_types[i];
        }
        return iv::details::configure_package_definition_provider(
            graph, id, std::nullopt, arguments);
    };
    auto module = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto value = boundary.input<"value">(iv::Sample{.25f});
        auto converted = iv::details::configure_package_definition_provider(graph,
            "iv.test.graph_jit.state_context.manual_control_slice_probe", std::nullopt, {});
        auto delayed = iv::details::configure_package_definition_provider(graph,
            "iv.test.graph_jit.state_context.manual_control_slice_probe", std::nullopt, {});
        if (convert) {
            // A real feedback cycle carries a zero ordinary contribution. The
            // knob is added at the module input before conversion or detach.
            // Both probe nodes were created first to keep their state indices.
            auto feedback = configure("iv.test.graph_jit.state_context.feedback_gain",
                std::uint64_t{0}, static_cast<std::uint64_t const*>(nullptr));
            auto zero_feedback = configure("iv.test.graph_jit.state_context.feedback_gain",
                std::uint64_t{0}, static_cast<std::uint64_t const*>(nullptr));
            auto zero = configure("iv.test.graph_jit.state_context.constant", iv::Sample{0});
            auto one = configure("iv.test.graph_jit.state_context.constant", iv::Sample{1});
            converted(graph.tile<iv::stereo>(value, zero));
            feedback.connect_input("in", static_cast<iv::SamplePortRef>(value).detach(2, iv::Sample{-1}));
            feedback.connect_input("gain", static_cast<iv::SamplePortRef>(one));
            zero_feedback.connect_input("in", static_cast<iv::SamplePortRef>(feedback));
            zero_feedback.connect_input("gain", static_cast<iv::SamplePortRef>(zero));
            delayed(feedback); // Read the delayed output outside the cycle.
            boundary.outputs(iv::PortName<"feedback", iv::NamedPortKind::sample>{} = zero_feedback);
        } else {
            converted(value);
            delayed(value);
            boundary.outputs();
        }
    }, "manual-module");
    if (convert) module(static_cast<iv::SamplePortRef>(module));
    auto const module_handle = module.node_bundle_handle();
    graph.outputs();
    auto configured = iv::details::take_built_graph(session.get());
    configured.manual_controls.push_back({
        .id = {.node = {.instance_id = "module"}, .input_name = "value"},
        .input = {module_handle, iv::PortKind::sample, 0}, .settled_value = .25f,
        .participation = iv::ManualControlParticipation::participate_with_connections,
        .dynamic = dynamic,
    });
    return std::make_shared<iv::ConfiguredGraph const>(std::move(configured));
}

} // namespace

TEST_F(GraphJitRuntimeFixture,
       DynamicManualControlInterpolatesAcrossAllPrimitiveSlices)
{
    auto const base = configured_module_graph(
        *revision, graph_jit_manual_control_slice_module_id);
    ASSERT_TRUE(base);

    auto with_manual_control = [&](bool dynamic) {
        auto graph = std::make_shared<iv::ConfiguredGraph>(*base);
        std::optional<iv::NodeBundleHandle> target;
        for (iv::NodeBundleHandle bundle = 0;
             bundle < graph->node_bundles.size(); ++bundle) {
            if (!graph->node_bundles.bundle(bundle).is_concrete()) continue;
            auto const inputs = graph->node_bundles
                .typed_ports(bundle).sample_inputs();
            if (inputs.size() == 1 && inputs.front().name == "value") {
                target = bundle;
                break;
            }
        }
        EXPECT_TRUE(target.has_value());
        if (target) {
            graph->manual_controls.push_back(iv::ConfiguredManualControl{
                .id = {
                    .node = {.instance_id = "instance:manual"},
                    .input_name = "value",
                },
                .input = {*target, iv::PortKind::sample, 0},
                .settled_value = iv::Sample{0.25f},
                .dynamic = dynamic,
            });
        }
        return std::shared_ptr<iv::ConfiguredGraph const>(std::move(graph));
    };

    auto constant = compile_graph(with_manual_control(false), 270);
    ASSERT_TRUE(constant.succeeded())
        << (constant.diagnostics.empty()
                ? ""
                : constant.diagnostics.front().message);
    auto dynamic = compile_graph(with_manual_control(true), 270);
    ASSERT_TRUE(dynamic.succeeded())
        << (dynamic.diagnostics.empty()
                ? ""
                : dynamic.diagnostics.front().message);
    ASSERT_EQ(constant.timings.size(), 1u);
    ASSERT_EQ(dynamic.timings.size(), 1u);
    EXPECT_EQ(constant.timings.front().region_specializations_compiled, 1u);
    EXPECT_EQ(constant.timings.front().region_specializations_reused, 0u);
    EXPECT_EQ(dynamic.timings.front().region_specializations_compiled, 1u);
    EXPECT_EQ(dynamic.timings.front().region_specializations_reused, 0u);
    EXPECT_EQ(constant.timings.front().region_specializations_skipped, 0u);
    EXPECT_EQ(dynamic.timings.front().region_specializations_skipped, 0u);
    ASSERT_EQ(constant.compiled_graph->manual_controls.size(), 1u);
    ASSERT_EQ(dynamic.compiled_graph->manual_controls.size(), 1u);
    EXPECT_FALSE(constant.compiled_graph->manual_controls[0].configured.dynamic);
    EXPECT_TRUE(dynamic.compiled_graph->manual_controls[0].configured.dynamic);
    EXPECT_EQ(
        constant.compiled_graph->node_layout.storage_size,
        dynamic.compiled_graph->node_layout.storage_size);
    ASSERT_EQ(
        constant.compiled_graph->node_layout.regions.size(),
        dynamic.compiled_graph->node_layout.regions.size());
    for (std::size_t region = 0;
         region < constant.compiled_graph->node_layout.regions.size();
         ++region) {
        auto const& lhs = constant.compiled_graph->node_layout.regions[region];
        auto const& rhs = dynamic.compiled_graph->node_layout.regions[region];
        EXPECT_EQ(lhs.kind, rhs.kind);
        EXPECT_EQ(lhs.storage_offset, rhs.storage_offset);
        EXPECT_EQ(lhs.size, rhs.size);
        EXPECT_EQ(lhs.alignment, rhs.alignment);
        EXPECT_EQ(lhs.migration_identity, rhs.migration_identity);
    }

    auto constant_storage = constant.compiled_graph
        ->node_layout.create_storage(resources);
    constant_storage.initialize();
    auto* constant_state = static_cast<ManualControlSliceProbeStateMirror*>(
        constant_storage.state_ptr(0));
    ASSERT_NE(constant_state, nullptr);
    constant.compiled_graph->root_operations.tick_block(
        constant_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        4);
    EXPECT_EQ(constant_state->calls, 2u);
    ASSERT_EQ(constant_state->value_count, 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(constant_state->values[i], 0.25f);
    }
    EXPECT_FLOAT_EQ(constant_state->history_before_slice[0], 0.25f);
    EXPECT_FLOAT_EQ(constant_state->history_before_slice[1], 0.25f);

    // Code-only activation keeps this NodeStorage. Constant code maintained
    // the canonical shadow timeline while exposing an immutable input to the
    // node, so dynamic code sees the correct preceding history immediately.
    auto const state_offset = dynamic.compiled_graph
        ->manual_controls.front().state_storage_offset;
    EXPECT_EQ(
        state_offset,
        constant.compiled_graph->manual_controls.front().state_storage_offset);
    auto* runtime_state = reinterpret_cast<iv::ManualControlRuntimeState*>(
        constant_storage.buffer().data() + state_offset);
    std::atomic_ref<std::uint32_t>(runtime_state->target_bits).store(
        std::bit_cast<std::uint32_t>(1.0f), std::memory_order_relaxed);

    dynamic.compiled_graph->root_operations.tick_block(
        constant_storage.buffer().data(),
        &empty_tick_invocation,
        index(4),
        4);
    EXPECT_EQ(constant_state->calls, 4u);
    ASSERT_EQ(constant_state->value_count, 8u);
    EXPECT_FLOAT_EQ(constant_state->history_before_slice[2], 0.25f);
    EXPECT_FLOAT_EQ(constant_state->history_before_slice[3], 0.625f);
    EXPECT_FLOAT_EQ(constant_state->values[4], 0.4375f);
    EXPECT_FLOAT_EQ(constant_state->values[5], 0.625f);
    EXPECT_FLOAT_EQ(constant_state->values[6], 0.8125f);
    EXPECT_FLOAT_EQ(constant_state->values[7], 1.0f);
    EXPECT_FLOAT_EQ(
        std::bit_cast<float>(runtime_state->effective_bits), 1.0f);

    dynamic.compiled_graph->root_operations.tick_block(
        constant_storage.buffer().data(),
        &empty_tick_invocation,
        index(8),
        4);
    EXPECT_EQ(constant_state->calls, 6u);
    ASSERT_EQ(constant_state->value_count, 12u);
    for (std::size_t i = 8; i < 12; ++i) {
        EXPECT_FLOAT_EQ(constant_state->values[i], 1.0f);
    }

    auto connected_graph = std::make_shared<iv::ConfiguredGraph>(
        *configured_module_graph(*revision, graph_jit_reused_sample_arena_module_id));
    std::vector<iv::NodeBundleHandle> connected_targets;
    for (iv::NodeBundleHandle bundle = 0;
         bundle < connected_graph->node_bundles.size(); ++bundle) {
        if (!connected_graph->node_bundles.bundle(bundle).is_concrete()) continue;
        auto const inputs = connected_graph->node_bundles
            .typed_ports(bundle).sample_inputs();
        if (inputs.size() == 1 && inputs.front().name == "in") {
            connected_targets.push_back(bundle);
        }
    }
    ASSERT_EQ(connected_targets.size(), 2u);
    connected_graph->manual_controls.push_back(iv::ConfiguredManualControl{
        .id = {
            .node = {.instance_id = "instance:connected"},
            .input_name = "in",
        },
        .input = {connected_targets[0], iv::PortKind::sample, 0},
        .settled_value = iv::Sample{0.5f},
        .participation =
            iv::ManualControlParticipation::participate_with_connections,
    });
    connected_graph->manual_controls.push_back(iv::ConfiguredManualControl{
        .id = {
            .node = {.instance_id = "instance:disconnected-only"},
            .input_name = "in",
        },
        .input = {connected_targets[1], iv::PortKind::sample, 0},
        .settled_value = iv::Sample{0.5f},
        .participation =
            iv::ManualControlParticipation::disconnected_only,
    });
    auto connected = compile_graph(std::move(connected_graph), 271);
    ASSERT_TRUE(connected.succeeded())
        << (connected.diagnostics.empty()
                ? ""
                : connected.diagnostics.front().message);
    auto connected_storage = connected.compiled_graph
        ->node_layout.create_storage(resources);
    connected_storage.initialize();
    connected.compiled_graph->root_operations.tick_block(
        connected_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        4);
    std::vector<SampleConsumerProbeStateMirror const*> consumer_states;
    for (std::size_t node = 0;
         node < connected.compiled_graph->node_layout.nodes.size(); ++node) {
        if (connected.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(SampleConsumerProbeStateMirror)) {
            consumer_states.push_back(
                static_cast<SampleConsumerProbeStateMirror const*>(
                    connected_storage.state_ptr(node)));
        }
    }
    ASSERT_EQ(consumer_states.size(), 2u);
    auto sums = std::array{
        consumer_states[0]->sum,
        consumer_states[1]->sum,
    };
    std::ranges::sort(sums);
    EXPECT_EQ(sums, (std::array<float, 2>{6.0f, 8.0f}));
}

TEST_F(GraphJitRuntimeFixture, ModuleManualControlPreservesConversionFanoutAndDetachAcrossCodeActivation)
{
    auto constant = compile_graph(configured_module_manual_fanout(*revision, false), 390);
    ASSERT_TRUE(constant.succeeded()) << (constant.diagnostics.empty() ? "" : constant.diagnostics.front().message);
    auto dynamic = compile_graph(configured_module_manual_fanout(*revision, true), 390);
    ASSERT_TRUE(dynamic.succeeded()) << (dynamic.diagnostics.empty() ? "" : dynamic.diagnostics.front().message);
    auto const& steady = *constant.compiled_graph;
    auto const& live = *dynamic.compiled_graph;
    ASSERT_EQ(steady.manual_controls.size(), 2u);
    ASSERT_EQ(live.manual_controls.size(), 2u);
    ASSERT_EQ(steady.node_layout.regions.size(), live.node_layout.regions.size());
    EXPECT_EQ(steady.node_layout.storage_size, live.node_layout.storage_size);
    for (std::size_t index = 0; index < steady.node_layout.regions.size(); ++index) {
        auto const& a = steady.node_layout.regions[index];
        auto const& b = live.node_layout.regions[index];
        EXPECT_EQ(a.storage_offset, b.storage_offset);
        EXPECT_EQ(a.size, b.size);
        EXPECT_EQ(a.migration_identity, b.migration_identity);
    }
    auto storage = steady.node_layout.create_storage(resources);
    storage.initialize();
    auto* converted = static_cast<ManualControlSliceProbeStateMirror*>(storage.state_ptr(0));
    auto* delayed = static_cast<ManualControlSliceProbeStateMirror*>(storage.state_ptr(1));
    ASSERT_NE(converted, nullptr);
    ASSERT_NE(delayed, nullptr);
    steady.root_operations.tick_block(storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    ASSERT_EQ(converted->value_count, 4u);
    ASSERT_EQ(delayed->value_count, 4u);
    for (std::size_t i = 0; i < 4; ++i) EXPECT_FLOAT_EQ(converted->values[i], .125f);
    EXPECT_FLOAT_EQ(delayed->values[0], -1.f);
    EXPECT_FLOAT_EQ(delayed->values[1], -1.f);
    EXPECT_FLOAT_EQ(delayed->values[2], .25f);
    EXPECT_FLOAT_EQ(delayed->values[3], .25f);
    for (auto const& binding : live.manual_controls) {
        auto* state = reinterpret_cast<iv::ManualControlRuntimeState*>(storage.buffer().data() + binding.state_storage_offset);
        std::atomic_ref<std::uint32_t>(state->target_bits).store(std::bit_cast<std::uint32_t>(1.f), std::memory_order_relaxed);
    }
    live.root_operations.tick_block(storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    std::array expected_converted{.21875f, .3125f, .40625f, .5f};
    std::array expected_delayed{.25f, .25f, .4375f, .625f};
    ASSERT_EQ(converted->value_count, 8u);
    ASSERT_EQ(delayed->value_count, 8u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(converted->values[4 + i], expected_converted[i]);
        EXPECT_FLOAT_EQ(delayed->values[4 + i], expected_delayed[i]);
    }
    live.root_operations.tick_block(storage.buffer().data(), &empty_tick_invocation, index(8), 4);
    EXPECT_FLOAT_EQ(delayed->values[8], .8125f);
    EXPECT_FLOAT_EQ(delayed->values[9], 1.f);
    EXPECT_FLOAT_EQ(delayed->values[10], 1.f);
    EXPECT_FLOAT_EQ(delayed->values[11], 1.f);
}

TEST_F(GraphJitRuntimeFixture, UniformSettledModuleManualInputKeepsImmutableInputBinding)
{
    auto compiled = compile_graph(configured_module_manual_fanout(*revision, false, false), 391);
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    ASSERT_EQ(graph.manual_controls.size(), 2u);
    // This fixture variant is ordinary fanout without the feedback branch.
    auto const target = graph.manual_controls[0].configured.input;
    auto const& requirements = graph.realtime_port_state_requirements.states;
    auto const& realizations = graph.realtime_port_state_realizations.sample_states;
    auto requirement = std::ranges::find_if(requirements, [&](auto const& entry) {
        return entry.configured_port == target && entry.direction == iv::graph_jit::PortDirection::input;
    });
    ASSERT_NE(requirement, requirements.end());
    auto const index_in_requirements = static_cast<std::size_t>(std::distance(requirements.begin(), requirement));
    auto realization = std::ranges::find_if(realizations, [&](auto const& entry) {
        return entry.requirement_index == index_in_requirements;
    });
    ASSERT_NE(realization, realizations.end());
    EXPECT_EQ(realization->retained_access, iv::graph_jit::SampleRealtimePortStateAccess::immutable_constant);
    auto storage = graph.node_layout.create_storage(resources);
    storage.initialize();
    graph.root_operations.tick_block(storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    auto* state = static_cast<ManualControlSliceProbeStateMirror*>(storage.state_ptr(0));
    ASSERT_EQ(state->value_count, 4u);
    for (std::size_t value = 0; value < 4; ++value) EXPECT_FLOAT_EQ(state->values[value], .25f);
}

} // namespace iv::test::graph_jit_tests
