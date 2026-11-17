#include "test_support.h"

#include <chrono>
#include <thread>

namespace iv::test::graph_jit_tests {
namespace {
NodeBundlePortId first_output(ConfiguredGraph const& graph)
{
    for (NodeBundleHandle bundle = 0; bundle < graph.node_bundles.size(); ++bundle)
        if (graph.node_bundles.bundle(bundle).is_concrete()
            && graph.node_bundles.bundle(bundle).sample_output_count())
            return {bundle, PortKind::sample, 0};
    throw std::logic_error("gain test has no concrete output");
}
void add_gain(ConfiguredGraph& graph, NodeBundlePortId output, Sample factor,
    bool dynamic = false, bool preparation = false)
{
    graph.manual_controls.push_back({.id = {.node = {.instance_id = "gain-test"},
        .port_name = graph.node_bundles.resolve_sample_output(output).config.name,
        .kind = ManualControlKind::output_gain},
        .settled_value = factor, .dynamic = dynamic, .output = output,
        .requires_preparation = preparation});
}
bool activate(GraphExecutor& executor, GlobalIndex at)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do { if (executor.activate_pending(at)) return true; std::this_thread::yield(); }
    while (std::chrono::steady_clock::now() < deadline);
    return false;
}
std::shared_ptr<ObservationWindowQuery> query_window(GraphExecutor& executor,
    CompiledGraph const& graph, NodeBundlePortId port, std::int64_t begin = 4)
{
    auto query = std::make_shared<ObservationWindowQuery>();
    query->configured_generation = graph.configured_generation.value_or(graph.project_generation);
    query->output = port;
    query->operation = &observation_details::sample_operations.back();
    query->window = {index(begin), 4, 4};
    ObservationCollectRequest collection{.windows = {query}};
    executor.handle_observation_collect(collection);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!query->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return query;
}
}

TEST_F(GraphJitRuntimeFixture, SettledUnityOutputGainKeepsTheGainFreeLayoutAndSpecialization)
{
    auto plain = configured_module_graph(*revision,
        "iv.test.graph_jit.state_context.transient_sample_module");
    auto unity = std::make_shared<ConfiguredGraph>(*plain);
    add_gain(*unity, first_output(*unity), 1);
    auto before = compile_graph(plain, 810);
    ASSERT_TRUE(before.succeeded()) << before.diagnostics.front().message;
    auto after = compile_graph(unity, 811);
    ASSERT_TRUE(after.succeeded()) << after.diagnostics.front().message;
    EXPECT_TRUE(after.compiled_graph->output_gain_views.empty());
    EXPECT_TRUE(after.compiled_graph->manual_controls.empty());
    EXPECT_EQ(after.compiled_graph->node_layout.storage_size, before.compiled_graph->node_layout.storage_size);
    EXPECT_EQ(after.compiled_graph->node_layout.nodes.size(), before.compiled_graph->node_layout.nodes.size());
    EXPECT_EQ(after.compiled_graph->background_evaluation_plan.ports.size(),
        before.compiled_graph->background_evaluation_plan.ports.size());
    ASSERT_EQ(after.timings.size(), 1u);
    // Reuse of the original kernel also checks that unity did not introduce
    // a multiply, callback wrapper or control load into its specialization key.
    EXPECT_EQ(after.timings[0].region_specializations_compiled, 0u);
    EXPECT_GT(after.timings[0].region_specializations_reused, 0u);
}

TEST_F(GraphJitRuntimeFixture, OutputGainScalesConsumersAndObservationsAfterSpecialization)
{
    auto graph = std::make_shared<ConfiguredGraph>(*configured_module_graph(*revision,
        "iv.test.graph_jit.state_context.transient_sample_module"));
    auto output = first_output(*graph);
    add_gain(*graph, output, -.5f);
    auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
    auto compiled = jit->compile({.project_generation = 812, .graph = graph, .definitions = definitions,
        .observations = {.revision = 11, .probes = {{.id = 1, .operation = &operation,
            .kind = ObservationSourceKind::output, .bundle = output.node_bundle_handle,
            .port = output.port_index, .available = true}}}});
    ASSERT_TRUE(compiled.succeeded()) << compiled.diagnostics.front().message;
    ASSERT_EQ(compiled.compiled_graph->output_gain_views.size(), 1u);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(activate(executor, index(3)));
    executor.tick_block(index(3), 8);
    auto frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->count, 1u);
    EXPECT_DOUBLE_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().peak, 5);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    compiled.compiled_graph->root_operations.tick_block(storage.buffer().data(), &empty_tick_invocation, index(3), 8);
    auto state = find_unique_state<SampleConsumerProbeStateMirror>(compiled.compiled_graph->node_layout, storage);
    ASSERT_NE(state, nullptr);
    EXPECT_FLOAT_EQ(state->first, -1.5f);
    EXPECT_FLOAT_EQ(state->last, -5);
}

TEST_F(GraphJitRuntimeFixture, RealtimeOutputGainUsesTheExistingSampleBySampleControlRamp)
{
    auto graph = std::make_shared<ConfiguredGraph>(*configured_module_graph(*revision,
        "iv.test.graph_jit.state_context.transient_sample_module"));
    add_gain(*graph, first_output(*graph), .5f, true);
    auto compiled = compile_graph(graph, 813);
    ASSERT_TRUE(compiled.succeeded()) << compiled.diagnostics.front().message;
    ASSERT_EQ(compiled.compiled_graph->manual_controls.size(), 1u);
    auto const& binding = compiled.compiled_graph->manual_controls.front();
    EXPECT_EQ(binding.configured.id.kind, ManualControlKind::output_gain);
    EXPECT_TRUE(binding.configured.dynamic);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto control = reinterpret_cast<ManualControlRuntimeState*>(storage.buffer().data() + binding.state_storage_offset);
    std::atomic_ref<std::uint32_t>(control->target_bits).store(std::bit_cast<std::uint32_t>(1.f));
    compiled.compiled_graph->root_operations.tick_block(storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    auto state = find_unique_state<SampleConsumerProbeStateMirror>(compiled.compiled_graph->node_layout, storage);
    ASSERT_NE(state, nullptr);
    EXPECT_FLOAT_EQ(state->first, 4 * .625f);
    EXPECT_FLOAT_EQ(state->last, 7);
    EXPECT_FLOAT_EQ(state->sum, 4*.625f + 5*.75f + 6*.875f + 7);
}

TEST_F(GraphJitRuntimeFixture, FFTOutputGainScalesEveryTransportedValueAtItsOwnPace)
{
    struct alignas(256) State {
        std::uint64_t calls, root_block_size, input_block_size, extended_block_size, value_index;
        float preceding_bin, first_bin, last_bin;
    };
    auto graph = std::make_shared<ConfiguredGraph>(*configured_module_graph(*revision, graph_jit_fft_value_module_id));
    std::optional<NodeBundlePortId> output;
    for (NodeBundleHandle bundle = 0; bundle < graph->node_bundles.size(); ++bundle) {
        auto const& node = graph->node_bundles.bundle(bundle);
        if (!node.is_concrete()) continue;
        for (std::size_t port = 0; port < node.sample_output_count(); ++port)
            if (continuous_value_type_domain(graph->node_bundles.resolve_sample_output({bundle, PortKind::sample, port}).config)
                .sole_type() == ContinuousValueTypeId::fft_block) output = NodeBundlePortId{bundle, PortKind::sample, port};
    }
    ASSERT_TRUE(output);
    add_gain(*graph, *output, 2);
    auto compiled = compile_graph(graph, 818);
    ASSERT_TRUE(compiled.succeeded()) << compiled.diagnostics.front().message;
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto state = find_unique_state<State>(compiled.compiled_graph->node_layout, storage);
    ASSERT_NE(state, nullptr);
    compiled.compiled_graph->root_operations.tick_block(storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(state->input_block_size, 16u);
    EXPECT_FLOAT_EQ(state->last_bin, 112);
    compiled.compiled_graph->root_operations.tick_block(storage.buffer().data(), &empty_tick_invocation, index(64), 32);
    EXPECT_FLOAT_EQ(state->preceding_bin, 112);
    EXPECT_FLOAT_EQ(state->first_bin, 120);
    EXPECT_FLOAT_EQ(state->last_bin, 176);
}

TEST_F(GraphJitRuntimeFixture, BufferedOutputGainRescalesOldRecordingWithoutMutatingItsBacking)
{
    // Generation-local recordings of anonymous nodes intentionally start
    // fresh. This edit retains the source, as a named project node does.
    auto raw = with_node_identities(configured_module_graph(*revision,
        "iv.test.graph_jit.state_context.tick_capture_module"));
    auto output = first_output(*raw);
    auto before = compile_graph(raw, 814);
    ASSERT_TRUE(before.succeeded()) << before.diagnostics.front().message;
    auto const& before_ports = before.compiled_graph->background_evaluation_plan.ports;
    auto const before_recording = std::ranges::find_if(before_ports, [&](auto const& port) {
        return port.persisted_tick_output && port.configured_port == output;
    });
    ASSERT_NE(before_recording, before_ports.end());
    ASSERT_TRUE(before_recording->stable_identity);
    ASSERT_EQ(before_recording->output_latency, 1u);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(before.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(activate(executor, index(0)));
    executor.tick_block(index(0), 32);
    auto original = query_window(executor, *before.compiled_graph, output);
    ASSERT_TRUE(original->complete.load());
    ASSERT_EQ(original->result.status, ObservationWindowResult::Status::ready) << original->result.error;
    ASSERT_EQ(original->result.regions.size(), 1u);
    ASSERT_EQ(original->result.regions[0].values.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(original->result.regions[0].values[i].second.load<float>(), static_cast<float>(i) + 4.5f);

    auto changed = std::make_shared<ConfiguredGraph>(*raw);
    add_gain(*changed, output, 2, false, true);
    auto after = jit->handle_project_graph_transaction({.successor = {
        .project_generation = 815, .graph = changed, .definitions = definitions},
        .predecessor = before.compiled_graph});
    ASSERT_TRUE(after.succeeded()) << after.diagnostics.front().message;
    auto const& after_ports = after.compiled_graph->background_evaluation_plan.ports;
    auto const after_recording = std::ranges::find_if(after_ports, [&](auto const& port) {
        return port.persisted_tick_output && port.configured_port == output;
    });
    ASSERT_NE(after_recording, after_ports.end());
    EXPECT_EQ(after_recording->stable_identity, before_recording->stable_identity);
    auto const& plan = after.compiled_graph->background_evaluation_plan;
    ASSERT_EQ(after.compiled_graph->background_output_gain_views.size(), 1u);
    auto const view = after.compiled_graph->background_output_gain_views[0].second;
    auto const gain_input = std::ranges::find_if(after_ports, [&](auto const& port) {
        return port.direction == graph_jit::PortDirection::input
            && port.configured_port == NodeBundlePortId{view.node_bundle_handle, PortKind::sample, 0};
    });
    ASSERT_NE(gain_input, after_ports.end());
    ASSERT_EQ(gain_input->incoming_connections.size(), 1u);
    EXPECT_EQ(plan.connections[gain_input->incoming_connections[0]].sample_read_latencies,
        (std::vector<std::size_t>{0}));
    ASSERT_EQ(executor.stage(after.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(activate(executor, index(32)));
    auto scaled = query_window(executor, *after.compiled_graph, output);
    ASSERT_TRUE(scaled->complete.load());
    ASSERT_EQ(scaled->result.status, ObservationWindowResult::Status::ready) << scaled->result.error;
    ASSERT_EQ(scaled->result.regions.size(), 1u);
    ASSERT_EQ(scaled->result.regions[0].values.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(scaled->result.regions[0].values[i].second.load<float>(), 2*(static_cast<float>(i) + 4.5f));

    auto absent = query_window(executor, *after.compiled_graph, output, 64);
    ASSERT_TRUE(absent->complete.load());
    ASSERT_EQ(absent->result.status, ObservationWindowResult::Status::ready) << absent->result.error;
    EXPECT_TRUE(absent->result.regions.empty()); // Gain never invents recorded positions.

    auto restored = jit->handle_project_graph_transaction({.successor = {
        .project_generation = 816, .graph = raw, .definitions = definitions},
        .predecessor = after.compiled_graph});
    ASSERT_TRUE(restored.succeeded()) << restored.diagnostics.front().message;
    ASSERT_EQ(executor.stage(restored.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(activate(executor, index(32)));
    auto unchanged = query_window(executor, *restored.compiled_graph, output);
    ASSERT_TRUE(unchanged->complete.load());
    ASSERT_EQ(unchanged->result.status, ObservationWindowResult::Status::ready) << unchanged->result.error;
    ASSERT_EQ(unchanged->result.regions.size(), 1u);
    for (std::size_t i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(unchanged->result.regions[0].values[i].second.load<float>(), static_cast<float>(i) + 4.5f);
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST_F(GraphJitRuntimeFixture, TockOutputGainPreservesLogicalCoverageAndScalesPreparedWindows)
{
    auto graph = std::make_shared<ConfiguredGraph>(*configured_module_graph(*revision,
        "iv.test.automation.transformed_sparse_curve"));
    std::optional<NodeBundlePortId> output;
    graph->node_bundles.for_each_configured_bundle([&, bundle = NodeBundleHandle{}](ConfiguredNodeBundleView const& view) mutable {
        if (view.operations && !view.operations->runtime.tick_block && view.ports)
            for (std::size_t port = 0; port < view.ports->sample_output_count(); ++port)
                if (view.ports->sample_output(port).name == "value") output = NodeBundlePortId{bundle, PortKind::sample, port};
        ++bundle;
    });
    ASSERT_TRUE(output);
    add_gain(*graph, *output, 2, true, true);
    auto compiled = compile_graph(graph, 817);
    ASSERT_TRUE(compiled.succeeded()) << compiled.diagnostics.front().message;
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(activate(executor, index(0)));
    auto query = query_window(executor, *compiled.compiled_graph, *output);
    ASSERT_TRUE(query->complete.load());
    ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready) << query->result.error;
    // [4,8) is genuinely absent: gain must not invent values or Regions.
    EXPECT_TRUE(query->result.regions.empty());
    auto covered = query_window(executor, *compiled.compiled_graph, *output, 0);
    ASSERT_TRUE(covered->complete.load());
    ASSERT_EQ(covered->result.status, ObservationWindowResult::Status::ready) << covered->result.error;
    ASSERT_EQ(covered->result.regions.size(), 1u);
    EXPECT_EQ(covered->result.regions[0].range, range(0, 4));
    ASSERT_EQ(covered->result.regions[0].values.size(), 4u);
    EXPECT_FLOAT_EQ(covered->result.regions[0].values[3].second.load<float>(), 3.f);
    auto const& ports = compiled.compiled_graph->background_evaluation_plan.ports;
    auto scaled_port = compiled.compiled_graph->output_gain_views.front().second;
    auto found = std::ranges::find_if(ports, [&](auto const& port) {
        return port.direction == graph_jit::PortDirection::output && port.configured_port == scaled_port;
    });
    ASSERT_NE(found, ports.end());
    EXPECT_TRUE(found->authored_tock_output);
    EXPECT_FALSE(executor.background_execution_failures().any());
}
} // namespace iv::test::graph_jit_tests
