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
        return capability["kind"] == "output" && capability["member"] == "main";
    })) << describe.capabilities->dump();
    ObservationGraphRequest subscribe{.requirements = ObservationRequirements{.revision = 13,
        .probes = {{.id = 23, .source = {.node = {.instance_id = "module-meter"},
            .member = "main"}, .operation = "level"}}}};
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
    query->output = port->configured_port;
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

        auto distant = std::make_shared<ObservationWindowQuery>();
        distant->configured_generation = query->configured_generation;
        distant->output = query->output;
        distant->operation = query->operation;
        distant->window = {index(0), std::uint64_t{1} << 40, 512};
        ASSERT_TRUE(submit(distant));
        ASSERT_EQ(distant->result.status, ObservationWindowResult::Status::ready) << distant->result.error;
        ASSERT_EQ(distant->result.regions.size(), 1u);
        ASSERT_EQ(distant->result.regions[0].values.size(), 1u);
        EXPECT_EQ(distant->result.regions[0].values[0].first, index(0));
        EXPECT_FALSE(executor.background_execution_failures().any());
    }
    // No transaction view or generation-local storage backs the returned value.
    EXPECT_FLOAT_EQ(query->result.regions[0].values[3].second.load<float>(), 1.5f);
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
    query->output = port->configured_port;
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

} // namespace iv::test::graph_jit_tests
