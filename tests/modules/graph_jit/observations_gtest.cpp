#include "test_support.h"
#include <intravenous/runtime/observation_resolution.h>
#include <intravenous/runtime/socket_rpc_response_builders.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

namespace iv::test::graph_jit_tests {
namespace {
std::vector<NodeBundleHandle> concrete_bundles(ConfiguredGraph const& graph)
{
    std::vector<NodeBundleHandle> result;
    for (NodeBundleHandle bundle = 0; bundle < graph.node_bundles.size(); ++bundle)
        if (graph.node_bundles.bundle(bundle).is_concrete()) result.push_back(bundle);
    return result;
}
}

TEST_F(GraphJitRuntimeFixture, LiveOutputObservationUsesTheProducedWindowWithoutChangingDsp)
{
    auto graph = configured_module_graph(*revision, "iv.test.graph_jit.state_context.transient_sample_module");
    auto const bundles = concrete_bundles(*graph);
    ASSERT_GE(bundles.size(), 2u);
    auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
    auto compiled = jit->compile(GraphJitCompileRequest{
        .project_generation = 701, .graph = graph, .definitions = definitions,
        .observations = {.revision = 9, .probes = {{.id = 17, .operation = &operation,
            .kind = ObservationSourceKind::output, .bundle = bundles.front(), .port = 0, .available = true}}},
    });
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(3)));
    executor.tick_block(index(3), 32);
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->count, 1u);
    EXPECT_EQ(frame->requirements_revision, 9u);
    EXPECT_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().samples, 32u);
    EXPECT_DOUBLE_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().peak, 34.0);
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST_F(GraphJitRuntimeFixture, SummedOutputObservationReducesTheMixedSamplesOnce)
{
    auto member = configured_module_graph(*revision, "iv.test.graph_jit.state_context.transient_sample_module");
    auto const bundles = concrete_bundles(*member);
    ASSERT_GE(bundles.size(), 2u);
    GraphBuilder builder;
    auto const left = builder.embed(*member, "left").node_bundle(bundles.front());
    auto const right = builder.embed(*member, "right").node_bundle(bundles.front());
    auto graph = std::make_shared<ConfiguredGraph const>(builder.finish());
    auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
    auto compiled = jit->compile(GraphJitCompileRequest{
        .project_generation = 749, .graph = graph, .definitions = definitions,
        .observations = {.revision = 9, .probes = {{.id = 17, .operation = &operation,
            .kind = ObservationSourceKind::output, .channels = {0},
            .output_sources = {{left, 0, 0}, {right, 0, 0}}, .available = true}}},
    });
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(3)));
    executor.tick_block(index(3), 32);
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->count, 1u);
    auto const level = frame->values[0].value.load<observation_details::SampleLevelState>();
    EXPECT_EQ(level.samples, 32u);
    EXPECT_DOUBLE_EQ(level.peak, 68.0);
}

TEST_F(GraphJitRuntimeFixture, LiveInputObservationReadsTheEffectiveValueAfterOverride)
{
    for (bool override_connections : {false, true}) {
        auto graph = std::make_shared<ConfiguredGraph>(
            *configured_module_graph(*revision, "iv.test.graph_jit.state_context.transient_sample_module"));
        auto const bundles = concrete_bundles(*graph);
        ASSERT_GE(bundles.size(), 2u);
        if (override_connections) {
            graph->manual_controls.push_back({
                .id = {.node = {.instance_id = "sink"}, .port_name = "in"},
                .input = {bundles[1], PortKind::sample, 0},
                .settled_value = .25f,
                .override_connections = true, .enabled = false,
            });
        }
        auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
        auto compiled = jit->compile(GraphJitCompileRequest{
            .project_generation = 740, .graph = graph, .definitions = definitions,
            .observations = {.revision = 31, .probes = {{.id = 41, .operation = &operation,
                .kind = ObservationSourceKind::input, .bundle = bundles[1], .port = 0,
                .channels = {0}, .available = true}}},
        });
        ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
        GraphExecutor executor(resources);
        ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
        ASSERT_TRUE(executor.activate_pending(index(3)));
        executor.tick_block(index(3), 32);
        auto const* frame = executor.collect_observations();
        ASSERT_NE(frame, nullptr);
        ASSERT_EQ(frame->count, 1u);
        ASSERT_TRUE(frame->values[0].value.captured);
        auto const level = frame->values[0].value.load<observation_details::SampleLevelState>();
        EXPECT_EQ(level.samples, 32u);
        EXPECT_DOUBLE_EQ(level.peak, override_connections ? .25 : 34.0);
    }
}

TEST_F(GraphJitRuntimeFixture, ObservationSinkWithBackgroundDeliveryImportsItsSequentialInitializer)
{
    auto graph = configured_module_graph(*revision,
        "iv.test.graph_jit.state_context.background_execution_module");
    auto const bundles = concrete_bundles(*graph);
    ASSERT_EQ(bundles.size(), 1u);
    auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
    // The compiler-level probe adds its ordinary Sequential sink. A Tock
    // source forces background delivery and generated neutral initialization
    // for the sink; a purely realtime source does not exercise that import.
    auto compiled = jit->compile(GraphJitCompileRequest{
        .project_generation = 733, .graph = graph, .definitions = definitions,
        .observations = {.revision = 29, .probes = {{.id = 71, .operation = &operation,
            .kind = ObservationSourceKind::output, .bundle = bundles.front(), .port = 0, .available = true}}},
    });
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& plan = compiled.compiled_graph->background_evaluation_plan;
    auto input = std::ranges::find_if(plan.ports, [](auto const& port) {
        return port.direction == graph_jit::PortDirection::input && port.name == "observed";
    });
    ASSERT_NE(input, plan.ports.end());
    ASSERT_TRUE(input->tick_sequential_input);
    auto initialize = compiled.compiled_graph->root_operations.initialize_sequential_input;
    ASSERT_NE(initialize, nullptr);
    std::array<Sample, 3> neutral{Sample{3}, Sample{4}, Sample{5}};
    EXPECT_TRUE(initialize(static_cast<std::size_t>(input - plan.ports.begin()),
        reinterpret_cast<std::byte*>(neutral.data()), sizeof(neutral)));
    for (auto value : neutral) EXPECT_FLOAT_EQ(value.value, 0);

    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool activated = false;
    while (!(activated = executor.activate_pending(index(0))) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_TRUE(activated);
    executor.tick_block(index(0), 32);
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->count, 1u);
    EXPECT_TRUE(frame->values[0].value.captured);
    auto const result = frame->values[0].value.load<observation_details::SampleLevelState>();
    EXPECT_EQ(result.samples, 32u);
    EXPECT_DOUBLE_EQ(result.peak, 12);
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST_F(GraphJitRuntimeFixture, LiveStateObservationRunsAfterSpecializedCallbacksAndInvalidatesOnRemoval)
{
    auto graph = configured_module_graph(*revision, "iv.test.graph_jit.state_context.direct_sample_module");
    auto const bundles = concrete_bundles(*graph);
    ASSERT_GE(bundles.size(), 2u);
    // The consumer's reflected float State field is read through the same
    // registry codec as the generic frontend's numeric field operation.
    std::optional<NodeStateFieldStructure> field;
    std::size_t bundle_index = 0;
    std::string metadata;
    graph->node_bundles.for_each_configured_bundle([&](ConfiguredNodeBundleView const& bundle) {
        if (bundle_index++ != bundles[1]) return;
        metadata += bundle.operations ? "operations;" : "no operations;";
        if (!bundle.operations || !bundle.operations->runtime.state_structures) return;
        auto const& structure = bundle.operations->runtime.state_structures->state;
        metadata += structure ? "State;" : "no State;";
        if (structure) for (auto const& candidate : structure->fields) {
            metadata += candidate.name + ":" + candidate.type_name + ";";
            if (candidate.type_name == "c:f") { field = candidate; break; }
        }
    });
    ASSERT_TRUE(field) << "bundle " << bundles[1] << " State fields: " << metadata;
    auto compiled = jit->compile(GraphJitCompileRequest{
        .project_generation = 702, .graph = graph, .definitions = definitions,
        .observations = {.revision = 10, .probes = {{.id = 21,
            .operation = &observation_details::float_latest, .kind = ObservationSourceKind::state_field,
            .bundle = bundles[1], .state_field_offset = field->bit_offset / 8,
            .state_field_size = field->size_bits / 8, .available = true}}},
    });
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(3)));
    executor.tick_block(index(3), 16);
    auto const* collected = executor.collect_observations();
    ASSERT_NE(collected, nullptr);
    auto frame = *collected;
    EXPECT_TRUE(frame.values[0].value.captured);
    float observed;
    std::memcpy(&observed, frame.values[0].value.bytes.data(), sizeof(observed));
    EXPECT_FLOAT_EQ(observed, 3.0f);
    auto without = jit->handle_project_graph_transaction(ProjectGraphGraphJitRequest{
        .successor = {
            .project_generation = 703, .graph = std::make_shared<ConfiguredGraph const>(), .definitions = definitions,
            .observations = {.revision = 11, .probes = {{.id = 21}}},
        },
        .predecessor = compiled.compiled_graph,
    });
    ASSERT_TRUE(without.succeeded()) << (without.diagnostics.empty() ? "" : without.diagnostics.front().message);
    ASSERT_EQ(executor.stage(without.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(19)));
    executor.tick_block(index(19), 16);
    auto const* latest = executor.collect_observations();
    ASSERT_NE(latest, nullptr);
    EXPECT_GT(latest->activation_id, frame.activation_id);
    EXPECT_EQ(latest->values[0].status, ObservationValueStatus::unavailable);
}

TEST_F(GraphJitRuntimeFixture, LiveOutputObservationSelectsThePhysicalChannelInAnInterleavedOutput)
{
    auto graph = configured_module_graph(*revision, "iv.test.graph_jit.state_context.stereo_conversion_module");
    auto bundles = concrete_bundles(*graph);
    ASSERT_FALSE(bundles.empty());
    auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
    auto compiled = jit->compile(GraphJitCompileRequest{
        .project_generation = 704, .graph = graph, .definitions = definitions,
        .observations = {.revision = 12, .probes = {{.id = 22, .operation = &operation,
            .kind = ObservationSourceKind::output, .bundle = bundles.front(), .port = 0,
            .channels = {1}, .available = true}}},
    });
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(3)));
    executor.tick_block(index(3), 32);
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().samples, 32u);
    EXPECT_DOUBLE_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().peak, 1034.0);
}

TEST_F(GraphJitRuntimeFixture, ModuleOutputMeterUsesItsResolvedSampleTypeAndPublicAddress)
{
    auto const module = definitions->by_id.find("iv.test.graph_jit.state_context.ported_module");
    ASSERT_NE(module, definitions->by_id.end());
    ASSERT_EQ(module->second.kind, NodeDefinitionKind::module);
    GraphExecutor executor(resources);
    NodeInstances instances;
    GraphConnections connections;
    // Keep bindings alive until ProjectGraph has joined its control worker.
    std::optional<project_graph_node_instances_bridge::scope> instances_scope;
    std::optional<project_graph_graph_connections_bridge::scope> connections_scope;
    std::optional<project_graph_graph_jit_bridge::scope> jit_scope;
    std::optional<project_graph_graph_executor_bridge::scope> executor_scope;
    ProjectGraph project;
    instances_scope.emplace(project, instances);
    connections_scope.emplace(project, connections);
    jit_scope.emplace(project, *jit);
    executor_scope.emplace(project, executor);
    NodeDefinitionsSnapshotChanged changed{.snapshot = definitions};
    in_event_source([&] { project.handle_node_definitions_snapshot_changed(changed); });
    ProjectStringBuilder create;
    in_event_source([&] { project.handle_project_create_node_instance({
        .instance_id = "module-meter", .definition_id = "iv.test.graph_jit.state_context.ported_module",
    }, create); });
    ASSERT_EQ(create.build(), "module-meter");
    ObservationGraphRequest describe{.describe_nodes = {{.instance_id = "module-meter"}}};
    in_event_source([&] { project.handle_observation_request(describe); });
    ASSERT_TRUE(describe.error.empty()) << describe.error;
    ASSERT_TRUE(describe.capabilities);
    ASSERT_TRUE(std::ranges::any_of(*describe.capabilities, [](auto const& capability) {
        return capability["kind"] == "output" && capability["member"] == "value";
    })) << describe.capabilities->dump();
    ObservationGraphRequest subscribe{.requirements = ObservationRequirements{.revision = 13,
        .probes = {{.id = 23, .source = {.node = {.instance_id = "module-meter"},
            .member = "value"}, .operation = "level"}}}};
    in_event_source([&] { project.handle_observation_request(subscribe); });
    ASSERT_TRUE(subscribe.error.empty()) << subscribe.error;
    ASSERT_TRUE(project.wait_for_compilation());
    ASSERT_TRUE(project.current_generation());
    ASSERT_TRUE(project.current_generation()->compiled_graph);
    ASSERT_EQ(project.current_generation()->compiled_graph->observations.probes.size(), 1u);
    ASSERT_TRUE(project.current_generation()->compiled_graph->observations.probes.front().available);
    for (std::int64_t block = 0; block < 4; ++block) {
        (void)executor.activate_pending(index(block * 64));
        ASSERT_NE(executor.active_generation(), 0u);
        executor.tick_block(index(block * 64), 64);
    }
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->count, 1u);
    EXPECT_EQ(frame->requirements_revision, 13u);
    EXPECT_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().samples, 64u);
    EXPECT_DOUBLE_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().peak, .25);
}

TEST_F(GraphJitRuntimeFixture, StereoObservationKeepsIndependentLevelAndSignedValueResults)
{
    auto graph = configured_module_graph(*revision, "iv.test.graph_jit.state_context.stereo_conversion_module");
    auto const bundles = concrete_bundles(*graph);
    ASSERT_FALSE(bundles.empty());
    auto const operations = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations;
    auto const level = std::ranges::find(operations, "level", &ObservationOperation::id);
    auto const value = std::ranges::find(operations, "value", &ObservationOperation::id);
    ASSERT_NE(level, operations.end());
    ASSERT_NE(value, operations.end());
    auto compiled = jit->compile(GraphJitCompileRequest{
        .project_generation = 705, .graph = graph, .definitions = definitions,
        .observations = {.revision = 14, .probes = {
            {.id = 1, .operation = &*level, .bundle = bundles.front(), .port = 0, .channels = {0}, .available = true},
            {.id = 2, .operation = &*level, .bundle = bundles.front(), .port = 0, .channels = {1}, .available = true},
            {.id = 3, .operation = &*value, .bundle = bundles.front(), .port = 0, .channels = {0}, .available = true},
            {.id = 4, .operation = &*value, .bundle = bundles.front(), .port = 0, .channels = {1}, .available = true},
        }},
    });
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(-35)));
    executor.tick_block(index(-35), 32);
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->count, 4u);
    auto const left = frame->values[0].value.load<observation_details::SampleLevelState>();
    auto const right = frame->values[1].value.load<observation_details::SampleLevelState>();
    EXPECT_EQ(left.samples, 32u);
    EXPECT_EQ(right.samples, 32u);
    EXPECT_DOUBLE_EQ(left.peak, 35);
    EXPECT_DOUBLE_EQ(right.peak, 996);
    EXPECT_FLOAT_EQ(frame->values[2].value.load<float>(), -4);
    EXPECT_FLOAT_EQ(frame->values[3].value.load<float>(), 996);
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST_F(GraphJitRuntimeFixture, StereoObservationResolvesExplicitChannelSelectorsWithoutImplicitDownmix)
{
    auto graph = configured_module_graph(*revision, "iv.test.graph_jit.state_context.stereo_conversion_module");
    auto const bundles = concrete_bundles(*graph);
    ASSERT_FALSE(bundles.empty());
    std::unordered_map<std::string, NodeInstancePlacement> placements{
        {"stereo", {.root = bundles.front()}},
    };
    ObservationRequirements requests{.revision = 15, .probes = {
        {.id = 1, .source = {.node = {.instance_id = "stereo"}, .member = "out"}, .operation = "level"},
        {.id = 2, .source = {.node = {.instance_id = "stereo"}, .member = "out", .channel = 0}, .operation = "level"},
        {.id = 3, .source = {.node = {.instance_id = "stereo"}, .member = "out", .channel = 1}, .operation = "value"},
    }};
    auto resolved = resolve_observation_requirements(*graph, placements, requests);
    ASSERT_EQ(resolved.probes.size(), 3u);
    EXPECT_FALSE(resolved.probes[0].available);
    EXPECT_TRUE(resolved.probes[1].available);
    EXPECT_EQ(resolved.probes[1].channels, std::vector<std::size_t>{0});
    ASSERT_TRUE(resolved.probes[2].available);
    EXPECT_EQ(resolved.probes[2].channels, std::vector<std::size_t>{1});
    EXPECT_EQ(resolved.probes[2].operation->id, "value");
}
TEST_F(GraphJitRuntimeFixture, BackgroundInputWaveformsUseCoverageRegionsAndSequentialFallback)
{
    struct Case {
        std::string_view module, control, observed_input;
        std::int64_t begin;
        std::vector<GlobalIndexRange> regions;
        std::vector<float> values;
        bool override_connections = false;
        bool public_input = false;
        std::int64_t stride = 1;
    };
    for (auto const& test : {
             Case{"iv.test.replay.coverage_manual_window", "in", "in", 18,
                 {range(18, 20), range(20, 24), range(24, 26)}, {.25f, .25f, 4, 5, 6, 7, .25f, .25f}},
             Case{"iv.test.replay.coverage_manual_window", "coverage", "in", 18,
                 {range(18, 26)}, {.25f, .25f, .25f, .25f, .25f, .25f, .25f, .25f}, true},
             // Both requested samples belong to the same manual Region, with no storage for the samples between them.
             Case{"iv.test.replay.coverage_manual_window", "coverage", "in", 18,
                 {range(18, 26)}, {.25f, .25f}, true, false, 7},
             Case{"iv.test.replay.coverage_manual_window", "coverage", "coverage", 18,
                 {range(18, 20), range(20, 24), range(24, 26)}, {.25f, .25f, 4, 5, 6, 7, .25f, .25f}, false, true},
             Case{"iv.test.automation.transformed_sparse_curve", "gain", "gain", 0,
                 {range(0, 8)}, {0, .5f, 1, 1.5f, .25f, .25f, .25f, .25f}},
         }) {
        SCOPED_TRACE(::testing::Message{} << test.module << " control=" << test.control
            << " observed input=" << test.observed_input << " public=" << test.public_input
            << " stride=" << test.stride);
        auto compiled = compile_with_manual_input(test.module, 741, test.control, .25f, test.override_connections);
        ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
        auto const& graph = *compiled.compiled_graph;
        auto const& plan = graph.background_evaluation_plan;
        NodeBundlePortId selected;
        if (test.public_input) {
            auto const port = std::ranges::find_if(plan.public_coverage_inputs, [&](auto const& input) {
                return graph.configured_graph->node_bundles.resolve_sample_input(input.port).config.name
                    == test.observed_input;
            });
            ASSERT_NE(port, plan.public_coverage_inputs.end());
            ASSERT_TRUE(port->reconstructible);
            selected = port->port;
        } else {
            auto const port = std::ranges::find_if(plan.ports, [&](auto const& port) {
                return port.direction == graph_jit::PortDirection::input && port.name == test.observed_input
                    && port.configured_port.node_bundle_handle < graph.configured_graph->node_bundles.size();
            });
            ASSERT_NE(port, plan.ports.end());
            selected = port->configured_port;
            ASSERT_TRUE(observation_input_window_port(graph, selected));
        }
        auto const operations = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations;
        auto const operation = std::ranges::find(operations, "waveform", &ObservationOperation::id);
        ASSERT_NE(operation, operations.end());
        auto query = std::make_shared<ObservationWindowQuery>();
        query->configured_generation = graph.configured_generation.value_or(graph.project_generation);
        query->kind = ObservationSourceKind::input;
        query->port = selected;
        query->operation = &*operation;
        query->window = {index(test.begin), 8, test.values.size()};

        GraphExecutor executor(resources);
        ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool activated = false;
        while (!(activated = executor.activate_pending(index(test.begin))) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(activated);
        ObservationCollectRequest collection{.windows = {query}};
        executor.handle_observation_collect(collection);
        while (!query->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(query->complete.load(std::memory_order_acquire));
        ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready) << query->result.error;
        ASSERT_EQ(query->result.regions.size(), test.regions.size());
        std::size_t value_index = 0;
        for (auto const expected : test.regions) {
            auto const observed = std::ranges::find_if(query->result.regions,
                [&](auto const& region) { return region.range == expected; });
            ASSERT_NE(observed, query->result.regions.end());
            for (auto const& [position, value] : observed->values) {
                ASSERT_LT(value_index, test.values.size());
                EXPECT_EQ(position, index(test.begin + static_cast<std::int64_t>(value_index) * test.stride));
                ASSERT_TRUE(value.captured);
                EXPECT_FLOAT_EQ(value.load<float>(), test.values[value_index++]);
            }
        }
        EXPECT_EQ(value_index, test.values.size());
    }
}

TEST_F(GraphJitRuntimeFixture, BeatGridObservationReadsRootAndNestedTimelineInputs)
{
    auto compiled = compile("iv.test.replay.project_beat_grid", 751);
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    ObservationCollectRequest collection;
    for (auto const& input : graph.background_evaluation_plan.public_event_coverage_inputs) {
        if (input.type != EventTypeId::beat) continue;
        ASSERT_TRUE(input.reconstructible);
        ASSERT_EQ(input.sources.size(), 1u);
        auto& query = collection.windows.emplace_back(std::make_shared<ObservationWindowQuery>());
        query->configured_generation = graph.configured_generation.value_or(graph.project_generation);
        query->kind = ObservationSourceKind::input;
        query->port = input.port;
        query->window = {.begin = index(0), .length = 44101, .points = 32, .max_depth = 2};
    }
    ASSERT_EQ(collection.windows.size(), 2u);

    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    auto const activation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool activated = false;
    while (!(activated = executor.activate_pending(index(0))) && std::chrono::steady_clock::now() < activation_deadline)
        std::this_thread::yield();
    ASSERT_TRUE(activated);

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    executor.handle_observation_collect(collection);
    using Mark = std::pair<std::int64_t, unsigned>;
    std::vector<std::vector<Mark>> observed;
    for (auto const& query : collection.windows) {
        while (!query->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(query->complete.load(std::memory_order_acquire));
        ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready) << query->result.error;
        ASSERT_TRUE(query->result.unavailable_regions.empty());
        auto& marks = observed.emplace_back();
        for (auto const& event : query->result.events)
            marks.emplace_back(event.time.signed_value(), std::get<BeatEvent>(event.value).depth);
    }
    std::ranges::sort(observed);
    EXPECT_EQ(observed, (std::vector<std::vector<Mark>>{
        {{0, 0}, {7350, 2}, {14700, 2}, {22050, 1}, {29400, 2}, {36750, 2}, {44100, 1}},
        {{0, 0}, {22050, 2}, {44100, 1}},
    }));
}

TEST_F(GraphJitRuntimeFixture, PublicCoverageObservationReadsBeforeInternalDownmixAndAllowsDisconnection)
{
    auto original = configured_module_graph(*revision, "iv.test.replay.stereo_coverage_boundary");
    auto const bundles = original->node_bundles.size();
    NodeBundleHandle boundary = 0;
    while (boundary < bundles && !original->node_bundles.bundle(boundary).is_subgraph()) ++boundary;
    ASSERT_LT(boundary, bundles);
    for (bool connected : {true, false}) {
        SCOPED_TRACE(connected);
        auto configured = std::make_shared<ConfiguredGraph>(*original);
        if (!connected) {
            auto const old = configured->connections.configured_sample_connections();
            std::vector<ConfiguredSampleConnection> connections;
            for (auto const& connection : old) {
                if (connection.target_channels.front().bundle != boundary) connections.push_back(connection);
            }
            configured->connections = GraphBuilderConnections::from_configured_connections(
                connections, configured->connections.configured_event_connections());
        }
        auto compiled = compile_graph(configured, 744);
        ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
        auto const& graph = *compiled.compiled_graph;
        auto const& plan = graph.background_evaluation_plan;
        ASSERT_EQ(plan.public_coverage_inputs.size(), 1u);
        auto const& input = plan.public_coverage_inputs.front();
        EXPECT_TRUE(input.reconstructible);
        EXPECT_EQ(input.projections.empty(), !connected);
        EXPECT_EQ(plan.ports.end(), std::ranges::find(plan.ports, input.port, &graph_jit::BackgroundPortPlan::configured_port));

        std::unordered_map<std::string, NodeInstancePlacement> placements{{"module", {.root = boundary}}};
        auto query = std::make_shared<ObservationWindowQuery>();
        ASSERT_TRUE(resolve_observation_window_source(*configured, placements,
            {.node = {.instance_id = "module"}, .kind = ObservationSourceKind::input, .member = "coverage", .channel = 1},
            "waveform", *query, &graph.port_realization, &graph));
        query->configured_generation = graph.configured_generation.value_or(graph.project_generation);
        query->window = {index(18), 8, 8};
        GraphExecutor executor(resources);
        ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!executor.activate_pending(index(18)) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ObservationCollectRequest collection{.windows = {query}};
        executor.handle_observation_collect(collection);
        while (!query->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(query->complete.load(std::memory_order_acquire));
        ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready) << query->result.error;
        if (!connected) {
            EXPECT_TRUE(query->result.regions.empty());
            continue;
        }
        ASSERT_EQ(query->result.regions.size(), 1u);
        auto const& region = query->result.regions.front();
        EXPECT_EQ(region.range, range(20, 24));
        ASSERT_EQ(region.values.size(), 4u);
        for (std::size_t i = 0; i < region.values.size(); ++i) {
            EXPECT_EQ(region.values[i].first, index(20 + static_cast<std::int64_t>(i)));
            ASSERT_TRUE(region.values[i].second.captured);
            EXPECT_FLOAT_EQ(region.values[i].second.load<float>(), 104 + static_cast<float>(i));
        }
    }
}

TEST_F(GraphJitRuntimeFixture, PublicCoverageObservationDoesNotRequireReplayOfAnUnusedSource)
{
    auto compiled = compile("iv.test.replay.unused_coverage_input", 745);
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& plan = compiled.compiled_graph->background_evaluation_plan;
    ASSERT_EQ(plan.public_coverage_inputs.size(), 1u);
    EXPECT_FALSE(plan.public_coverage_inputs.front().reconstructible);
    EXPECT_TRUE(plan.ports.empty());
    EXPECT_TRUE(plan.runtime.bindings.empty());
}

TEST_F(GraphJitRuntimeFixture, BackgroundWaveformQueriesProjectSparseComputedOutputsBeforeStorageExpires)
{
    auto compiled = compile("iv.test.automation.transformed_sparse_curve", 731);
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto const& plan = graph.background_evaluation_plan;
    for (std::size_t node = 0; node < plan.nodes.size(); ++node) {
        if (!plan.nodes[node].authored_tock_execution) continue;
        auto const& invocation = plan.tick_runtime.nodes[node];
        EXPECT_EQ(invocation.random_access_sample_count, 0u);
        EXPECT_EQ(invocation.random_access_event_count, 0u);
    }
    auto port = std::ranges::find_if(plan.ports, [](auto const& port) {
        return port.kind == PortKind::sample && port.authored_tock_output && port.name == "value";
    });
    ASSERT_NE(port, plan.ports.end());
    auto const& operations = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations;
    auto operation = std::ranges::find(operations, "waveform", &ObservationOperation::id);
    ASSERT_NE(operation, operations.end());
    auto query = std::make_shared<ObservationWindowQuery>();
    query->configured_generation = graph.configured_generation.value_or(graph.project_generation);
    query->port = port->configured_port;
    query->operation = &*operation;
    query->window = {index(0), 8, 8};
    {
        GraphExecutor executor(resources);
        ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool activated = false;
        while (!(activated = executor.activate_pending(index(0))) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(activated);
        auto submit = [&](std::shared_ptr<ObservationWindowQuery> const& request) {
            ObservationCollectRequest collection{.windows = {request}};
            executor.handle_observation_collect(collection);
            while (!request->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            return request->complete.load(std::memory_order_acquire);
        };
        ASSERT_TRUE(submit(query));
        ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready) << query->result.error;
        ASSERT_EQ(query->result.regions.size(), 1u);
        EXPECT_EQ(query->result.regions[0].range, range(0, 4));
        ASSERT_EQ(query->result.regions[0].values.size(), 4u);
        EXPECT_FLOAT_EQ(query->result.regions[0].values[0].second.load<float>(), 0);
        EXPECT_FLOAT_EQ(query->result.regions[0].values[3].second.load<float>(), 1.5f);
        ObservationCollectRequest after_first;
        executor.handle_observation_collect(after_first);

        auto distant = std::make_shared<ObservationWindowQuery>();
        distant->configured_generation = query->configured_generation;
        distant->port = query->port;
        distant->operation = query->operation;
        distant->window = {index(0), std::uint64_t{1} << 40, 512};
        ASSERT_TRUE(submit(distant));
        ASSERT_EQ(distant->result.status, ObservationWindowResult::Status::ready) << distant->result.error;
        ASSERT_EQ(distant->result.regions.size(), 1u);
        ASSERT_EQ(distant->result.regions[0].values.size(), 1u);
        EXPECT_EQ(distant->result.regions[0].values[0].first, index(0));
        ObservationCollectRequest after;
        executor.handle_observation_collect(after);
        EXPECT_EQ(after.execution_data_version, after_first.execution_data_version);
        EXPECT_FALSE(executor.background_execution_failures().any());
    }
    // No transaction view or generation-local storage backs the returned value.
    EXPECT_FLOAT_EQ(query->result.regions[0].values[3].second.load<float>(), 1.5f);
}

TEST_F(GraphJitRuntimeFixture, SparseComputedWaveformFitsBelowTheBufferedMappingRunLimit)
{
    auto compiled = compile("iv.test.computed.huge_playback", 746);
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto const& plan = graph.background_evaluation_plan;
    auto const port = std::ranges::find(plan.ports, std::string{"huge"}, &graph_jit::BackgroundPortPlan::name);
    ASSERT_NE(port, plan.ports.end());
    ASSERT_TRUE(port->authored_tock_output);
    GraphExecutor executor(resources, {}, BufferedStorageLimits{.max_mapping_runs = 64});
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool activated = false;
    while (!(activated = executor.activate_pending(index(0))) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_TRUE(activated);

    // These samples are on separate pages, well outside the initial contiguous
    // playback window. Caching each observed sample would run out of mappings.
    auto const begin = index(1 << 20);
    constexpr std::size_t points = 512, stride = 8193;
    std::shared_ptr<ObservationWindowQuery> query;
    do {
        query = std::make_shared<ObservationWindowQuery>();
        query->configured_generation = graph.configured_generation.value_or(graph.project_generation);
        query->port = port->configured_port;
        query->operation = &observation_details::sample_operations.back();
        query->window = {begin, 1 + (points - 1) * stride, points};
        ObservationCollectRequest collection{.windows = {query}};
        executor.handle_observation_collect(collection);
        while (!query->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(query->complete.load(std::memory_order_acquire));
    } while (query->result.status == ObservationWindowResult::Status::pending_execution
        && std::chrono::steady_clock::now() < deadline);
    ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready)
        << query->result.error;
    ASSERT_EQ(query->result.regions.size(), 1u);
    auto const& region = query->result.regions.front();
    EXPECT_EQ(region.range, (GlobalIndexRange{begin, query->window.length}));
    ASSERT_EQ(region.values.size(), points);
    for (std::size_t i = 0; i < points; ++i) {
        auto const& [position, captured] = region.values[i];
        EXPECT_EQ(position, begin.advanced_by(i * stride));
        ASSERT_TRUE(captured.captured);
        EXPECT_FLOAT_EQ(captured.load<float>(), static_cast<float>(position.bits() & 1023));
    }
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST_F(GraphJitRuntimeFixture, BackgroundWaveformReadsPublishedTickRecordingWithoutReplayingTheProducer)
{
    auto compiled = compile("iv.test.graph_jit.state_context.tick_capture_module", 732);
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto port = std::ranges::find_if(graph.background_evaluation_plan.ports, [](auto const& port) {
        return port.kind == PortKind::sample && port.persisted_tick_output;
    });
    ASSERT_NE(port, graph.background_evaluation_plan.ports.end());
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    executor.tick_block(index(0), 32);
    auto query = std::make_shared<ObservationWindowQuery>();
    query->configured_generation = graph.configured_generation.value_or(graph.project_generation);
    query->port = port->configured_port;
    query->operation = &observation_details::sample_operations.back();
    query->window = {index(4), 4, 4};
    ObservationCollectRequest collection{.windows = {query}};
    executor.handle_observation_collect(collection);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!query->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_TRUE(query->complete.load(std::memory_order_acquire));
    ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready) << query->result.error;
    ASSERT_EQ(query->result.regions.size(), 1u);
    EXPECT_EQ(query->result.regions[0].range, range(4, 8));
    ASSERT_EQ(query->result.regions[0].values.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(query->result.regions[0].values[i].second.captured);
        EXPECT_FLOAT_EQ(query->result.regions[0].values[i].second.load<float>(), static_cast<float>(i) + 4.5f);
    }
    EXPECT_GT(query->result.data_version, 0u);
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST_F(GraphJitRuntimeFixture, ClickDetectorRetainsPlaybackHistoryAcrossMeterChangesAndSeek)
{
    GraphExecutor executor(resources);
    NodeInstances instances;
    GraphConnections connections;
    // Keep bindings alive until ProjectGraph has joined its control worker.
    std::optional<project_graph_node_instances_bridge::scope> instances_scope;
    std::optional<project_graph_graph_connections_bridge::scope> connections_scope;
    std::optional<project_graph_graph_jit_bridge::scope> jit_scope;
    std::optional<project_graph_graph_executor_bridge::scope> executor_scope;
    ProjectGraph project;
    instances_scope.emplace(project, instances);
    connections_scope.emplace(project, connections);
    jit_scope.emplace(project, *jit);
    executor_scope.emplace(project, executor);
    NodeDefinitionsSnapshotChanged changed{.snapshot = definitions};
    in_event_source([&] { project.handle_node_definitions_snapshot_changed(changed); });
    ProjectAckBuilder load;
    in_event_source([&] { project.handle_project_replace_graph({
        .instances = {
            {.instance_id = "source", .definition_id = "iv.test.click_recording.source"},
            {.instance_id = "second-source", .definition_id = "iv.test.click_recording.source"},
            {.instance_id = "third-source", .definition_id = "iv.test.click_recording.source"},
            {.instance_id = "detector", .definition_id = "builtin.click_detector"},
        },
        .connections = {ProjectSampleConnection{
            .connection_id = "inspect-audio",
            .source_type = ChannelTypeId::stereo,
            .outputs = {
                {.instance_id = "source", .port = {.name = "audio"}},
                {.instance_id = "second-source", .port = {.name = "audio"}},
            },
            .target_type = ChannelTypeId::stereo,
            .inputs = {{.instance_id = "detector", .port = {.name = "audio"}}},
        }, ProjectSampleConnection{
            .connection_id = "inspect-third-source",
            .source_type = ChannelTypeId::stereo,
            .outputs = {{.instance_id = "third-source", .port = {.name = "audio"}}},
            .target_type = ChannelTypeId::stereo,
            .inputs = {{.instance_id = "detector", .port = {.name = "audio"}}},
        }},
        // Each source steps by 0.25. It takes both cables and all three
        // sources to cross the threshold on the first sample of the click.
        .manual_controls = {{
            .id = {.node = {.instance_id = "detector"}, .port_name = "threshold"},
            .stored_value = Sample{0.625f},
        }},
    }, load); });
    ASSERT_NO_THROW(load.build());

    auto activate = [&](std::size_t at) {
        auto const target = project.current_generation();
        if (!target || !target->graph_executor_accepted) return false;
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            (void)executor.activate_pending(index(at));
            if (executor.active_generation() == target->generation
                && executor.background_generation() == target->generation) {
                project.acknowledge_execution(target->generation);
                return true;
            }
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    ASSERT_TRUE(project.wait_for_compilation());
    ASSERT_TRUE(activate(0));
    (void)executor.set_playback_running(true);

    auto const graph = executor.active_graph();
    ASSERT_TRUE(graph);
    auto const sample_rate = graph->specialization.sample_rate;
    auto const half_second = sample_rate / 2;
    auto const recording_frames = 5 * half_second;
    auto const block_size = graph->specialization.block_size;
    std::size_t rendered = 0;
    auto render_until = [&](std::size_t end) {
        while (rendered < end) {
            auto const frames = std::min(block_size, end - rendered);
            executor.render_block(frames, sample_rate);
            rendered += frames;
        }
        EXPECT_EQ(executor.playback_snapshot().foreground_playhead, index(rendered));
    };
    auto data_version = [&] {
        ObservationCollectRequest collection;
        executor.handle_observation_collect(collection);
        return collection.execution_data_version;
    };
    ObservationSource const source{
        .node = {.instance_id = "detector"}, .kind = ObservationSourceKind::output,
        .member = "clicks", .port_kind = PortKind::event,
    };
    auto read_recording = [&] {
        ObservationGraphRequest observation{.windows = {{
            source, recorded_event_timestamps_operation, {index(0), recording_frames},
        }}};
        in_event_source([&] { project.handle_observation_request(observation); });
        EXPECT_TRUE(observation.error.empty()) << observation.error;
        auto query = observation.windows.front().query;
        if (!query) return query;
        ObservationCollectRequest collection{.windows = {query}};
        executor.handle_observation_collect(collection);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!query->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return query;
    };

    std::vector<GlobalIndex> expected;
    for (std::size_t i = 0; i < 5; ++i) {
        auto const click = i * half_second + 13;
        auto const previous_version = data_version();
        render_until((click / block_size + 1) * block_size);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (data_version() == previous_version && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_GT(data_version(), previous_version) << "Click at sample " << click << " was never published";

        expected.push_back(index(click));
        auto const query = read_recording();
        ASSERT_TRUE(query);
        ASSERT_TRUE(query->complete.load(std::memory_order_acquire));
        ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready) << query->result.error;
        EXPECT_EQ(query->result.event_indices, expected) << "After recording sample " << click;

        if (i == 1) {
            // Entering a viewport with a live meter can activate a new
            // execution of the same authored project graph.
            ObservationGraphRequest subscribe{.requirements = ObservationRequirements{.revision = 1,
                .probes = {{.id = 17, .source = {.node = {.instance_id = "source"},
                    .member = "audio", .channel = 0}, .operation = "level"}}}};
            in_event_source([&] { project.handle_observation_request(subscribe); });
            ASSERT_TRUE(subscribe.error.empty()) << subscribe.error;
            ASSERT_TRUE(project.wait_for_compilation());
            ASSERT_TRUE(activate(rendered));
            auto const metered = executor.active_graph();
            ASSERT_TRUE(metered);
            EXPECT_NE(metered->project_generation, graph->project_generation);
            EXPECT_EQ(metered->configured_generation, graph->configured_generation);
            ASSERT_EQ(metered->observations.probes.size(), 1u);
            EXPECT_TRUE(metered->observations.probes.front().available);
            auto const after_cutover = read_recording();
            ASSERT_TRUE(after_cutover);
            ASSERT_TRUE(after_cutover->complete.load(std::memory_order_acquire));
            ASSERT_EQ(after_cutover->result.status, ObservationWindowResult::Status::ready)
                << after_cutover->result.error;
            EXPECT_EQ(after_cutover->result.event_indices, expected);
        }
    }

    render_until(recording_frames);
    // A running seek replays a click at a different offset within the
    // callback, so the new write overlaps the original recording.
    auto const before_replay = data_version();
    (void)executor.seek_playback(index(half_second + 5));
    executor.render_block(block_size, sample_rate);
    ASSERT_EQ(executor.playback_snapshot().foreground_playhead, index(half_second + 5 + block_size));
    auto const replay_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (data_version() == before_replay && std::chrono::steady_clock::now() < replay_deadline)
        std::this_thread::yield();
    ASSERT_GT(data_version(), before_replay);
    auto const final = read_recording();
    ASSERT_TRUE(final);
    ASSERT_TRUE(final->complete.load(std::memory_order_acquire));
    ASSERT_EQ(final->result.status, ObservationWindowResult::Status::ready) << final->result.error;
    EXPECT_EQ(final->result.event_indices, expected);
    EXPECT_FALSE(executor.background_execution_failures().any());
}

} // namespace iv::test::graph_jit_tests
