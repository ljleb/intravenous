#include "test_support.h"

namespace iv::test::graph_jit_tests {

namespace {

iv::IndexCoverage index_coverage(
    std::initializer_list<std::pair<std::int64_t, std::int64_t>> ranges)
{
    iv::IndexCoverage result;
    for (auto const [begin, end] : ranges) result.include(range(begin, end));
    return result;
}

constexpr char graph_jit_state_only_module_id[] = "iv.test.graph_jit.state_context.state_only_module";

constexpr char graph_jit_background_only_module_id[] = "iv.test.graph_jit.state_context.background_only_module";

constexpr char graph_jit_stateless_module_id[] = "iv.test.graph_jit.state_context.stateless_module";

constexpr char graph_jit_revising_capture_module_id[] =
    "iv.test.graph_jit.state_context.revising_capture_module";

constexpr char graph_jit_revising_paced_capture_module_id[] =
    "iv.test.graph_jit.state_context.revising_paced_capture_module";

constexpr char graph_jit_tick_capture_module_id[] = "iv.test.graph_jit.state_context.tick_capture_module";

constexpr char graph_jit_persisted_recording_coverage_module_id[] =
    "iv.test.graph_jit.state_context.persisted_recording_coverage_module";

struct SingleSpanProbeMirror {
    std::uint64_t calls = 0;
    std::uint64_t observed_state_extent = 0;
    std::uint64_t observed_background_extent = 0;
};

struct BackgroundExecutionProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t sample_rate = 0;
};

struct alignas(64) MultiRegionCoverageStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t region_count = 0;
    std::uint64_t first_begin = 0;
    std::uint64_t second_begin = 0;
    float first_overlap_value = 0.0f;
    float second_overlap_value = 0.0f;
};

struct alignas(64) MultiRegionSequentialStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t sample_count = 0;
    std::array<float, 6> values{};
};

struct alignas(64) MultiRegionEventCoverageStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t region_count = 0;
    std::uint64_t first_event_count = 0;
    std::uint64_t second_event_count = 0;
    std::uint64_t first_last_time = 0;
    std::uint64_t second_first_time = 0;
};

struct alignas(128) MultiRegionSequentialEventStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t event_count = 0;
    std::array<std::uint64_t, 5> times{};
};

struct alignas(64) SingleRegionSampleStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t begin = 0;
    std::uint64_t extent = 0;
    float first_value = 0.0f;
    float last_value = 0.0f;
};

struct alignas(64) SingleRegionEventStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t begin = 0;
    std::uint64_t extent = 0;
    std::uint64_t event_count = 0;
    std::uint64_t first_time = 0;
    std::uint64_t last_time = 0;
};

struct NativeBatchBackgroundProbeStateMirror {
    std::uint64_t tock_batch_calls = 0;
    std::uint64_t last_lane_count = 0;
    std::uint64_t configured_id = 0;
    std::uint64_t sample_rate = 0;
};

struct alignas(256) FFTValueConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t root_block_size = 0;
    std::uint64_t input_block_size = 0;
    std::uint64_t extended_block_size = 0;
    std::uint64_t value_index = 0;
    float preceding_bin = 0.0f;
    float first_bin = 0.0f;
    float last_bin = 0.0f;
};

struct alignas(512) FractionalIndexConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t root_block_size = 0;
    std::uint64_t input_block_size = 0;
    std::uint64_t value_index = 0;
    std::uint64_t first_whole = 0;
    std::uint64_t first_fraction = 0;
    std::uint64_t last_whole = 0;
    std::uint64_t last_fraction = 0;
};

struct BackgroundOperationHookCapture {
    BackgroundExecutionProbeStateMirror const* state = nullptr;
    std::size_t prepare_calls = 0;
    std::size_t finalize_calls = 0;
    std::uint64_t calls_seen_by_prepare = 0;
    std::uint64_t calls_seen_by_finalize = 0;
};

void prepare_background_operations(void* opaque)
{
    auto& capture = *static_cast<BackgroundOperationHookCapture*>(opaque);
    ++capture.prepare_calls;
    capture.calls_seen_by_prepare = capture.state->calls;
}

void finalize_background_operations(void* opaque)
{
    auto& capture = *static_cast<BackgroundOperationHookCapture*>(opaque);
    ++capture.finalize_calls;
    capture.calls_seen_by_finalize = capture.state->calls;
}

struct BackgroundExecutionCapture {
    std::vector<iv::GlobalIndexRange> regions{};
    iv::IndexCoverage coverage{};
    iv::IndexCoverage changed{};
    std::vector<std::pair<iv::GlobalIndex, float>> samples{};
};

struct GeneratedTickCaptureProbe {
    std::size_t sample_calls = 0;
    std::size_t event_calls = 0;
    iv::GlobalIndex sample_index{};
    std::size_t block_size = 0;
    std::size_t sample_history = 0;
    std::size_t sample_latency = 0;
    float first_sample = 0.0f;
    float last_sample = 0.0f;
    std::size_t event_history = 0;
    std::size_t event_latency = 0;
    std::size_t event_count = 0;
    iv::GlobalIndex first_event_time{};
};

void capture_generated_tick_samples(
    void* opaque,
    iv::ReflectedContinuousOutputPortBinding const* output,
    iv::GlobalIndex sample_index,
    std::size_t block_size) noexcept
{
    auto& probe = *static_cast<GeneratedTickCaptureProbe*>(opaque);
    ++probe.sample_calls;
    probe.sample_index = sample_index;
    probe.block_size = block_size;
    if (!output || block_size == 0) return;
    probe.sample_history = output->history;
    probe.sample_latency = output->latency;
    auto const& storage = output->storage;
    if (storage.channel_layout.channel_type != iv::ChannelTypeId::mono
        || storage.geometry.value_type != iv::ContinuousValueTypeId::sample
        || storage.value_capacity == 0
        || !std::has_single_bit(storage.value_capacity)) {
        return;
    }
    auto read = [&](std::uint64_t index) {
        auto const& channel = storage.channels[0];
        if (!channel.storage || channel.value_capacity == 0
            || !std::has_single_bit(channel.value_capacity)
            || channel.value_stride_bytes < sizeof(iv::Sample)
            || channel.value_delay >= channel.value_capacity) {
            return 0.0f;
        }
        auto const storage_index = index - channel.value_delay;
        auto const slot = static_cast<std::size_t>(storage_index)
            & (channel.value_capacity - 1);
        return static_cast<float>(*reinterpret_cast<iv::Sample const*>(
            channel.storage + slot * channel.value_stride_bytes));
    };
    probe.first_sample = read(output->sequential_begin_index);
    probe.last_sample = read(
        output->sequential_begin_index + block_size - 1);
}

void capture_generated_tick_events(
    void* opaque,
    iv::ReflectedEventOutputPortBinding const* output,
    iv::GlobalIndex,
    std::size_t) noexcept
{
    auto& probe = *static_cast<GeneratedTickCaptureProbe*>(opaque);
    ++probe.event_calls;
    if (!output) return;
    probe.event_history = output->history;
    probe.event_latency = output->latency;
    auto const& storage = output->storage;
    if (!storage.storage || storage.event_capacity == 0
        || !std::has_single_bit(storage.event_capacity)) {
        return;
    }
    std::size_t read = 0;
    auto write = *reinterpret_cast<std::size_t const*>(
        storage.storage + storage.count_offset);
    if (storage.persistent_ring) {
        read = *reinterpret_cast<std::size_t const*>(
            storage.storage + storage.read_index_offset);
        write = *reinterpret_cast<std::size_t const*>(
            storage.storage + storage.write_index_offset);
    }
    if (write < read) return;
    probe.event_count = std::min(write - read, storage.event_capacity);
    if (probe.event_count == 0) return;
    auto const* events = reinterpret_cast<iv::TimedEvent const*>(
        storage.storage + storage.events_offset);
    probe.first_event_time = events[
        read & (storage.event_capacity - 1)].time;
}

void capture_background_regions(
    void* opaque,
    iv::GlobalIndexRange const* regions,
    std::size_t region_count)
{
    auto& capture = *static_cast<BackgroundExecutionCapture*>(opaque);
    capture.regions.clear();
    if (region_count != 0) {
        capture.regions.assign(regions, regions + region_count);
    }
    capture.coverage = {};
    for (auto const region : capture.regions) {
        capture.coverage.include(region);
    }
}

void capture_background_change(
    void* opaque, iv::IndexCoverage const& changed)
{
    static_cast<BackgroundExecutionCapture*>(opaque)->changed = changed;
}

struct PersistedRecordingCoverageStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t sample_region_count = 0;
    std::uint64_t sample_layout = 0;
    std::uint64_t event_region_count = 0;
    std::uint64_t event_count = 0;
    std::uint64_t first_event_time = 0;
    float first_left = 0.0f;
    float first_right = 0.0f;
    float last_left = 0.0f;
    float last_right = 0.0f;
};

struct ModuleBoundaryCoverageStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t sample_region_count = 0;
    std::uint64_t event_region_count = 0;
    std::uint64_t first_event_count = 0;
    std::uint64_t second_event_count = 0;
    float first_overlap_value = 0.0f;
    float second_overlap_value = 0.0f;
};

// Each record a pending queue holds: its header and its payload bytes.
struct CapturedRecord {
    iv::RealtimeProducedRecordHeader header{};
    std::vector<std::byte> payload{};
};

std::vector<CapturedRecord> captured_records(iv::PinnedBlockPrefix const& prefix)
{
    std::vector<CapturedRecord> records;
    std::vector<std::byte> bytes;
    std::size_t remaining_blocks = 0;
    prefix.for_each([&](iv::AsyncQueueBlock const& block) {
        auto const used = block.used_storage();
        if (remaining_blocks == 0) {
            iv::RealtimeProducedRecordHeader header{};
            if (used.size() < sizeof(header)) return;
            std::memcpy(&header, used.data(), sizeof(header));
            remaining_blocks = std::max<std::size_t>(header.record_block_count, 1);
        }
        bytes.insert(bytes.end(), used.begin(), used.end());
        if (--remaining_blocks != 0) return;
        CapturedRecord record;
        std::memcpy(&record.header, bytes.data(), sizeof(record.header));
        auto const payload_end = std::min(
            bytes.size(), sizeof(record.header) + record.header.payload_size);
        record.payload.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(record.header)),
            bytes.begin() + static_cast<std::ptrdiff_t>(payload_end));
        records.push_back(std::move(record));
        bytes.clear();
    });
    return records;
}

} // namespace

TEST_F(GraphJitRuntimeFixture, StateAndBackgroundStateContexts)
{
    auto stateful = compile(graph_jit_stateful_module_id, 100);
    ASSERT_TRUE(stateful.succeeded())
        << (stateful.diagnostics.empty() ? "" : stateful.diagnostics.front().message);
    ASSERT_TRUE(stateful.compiled_graph->root_operations.valid());
    expect_single_node_canonical_regions(
        stateful.compiled_graph->node_layout,
        stateful.compiled_graph->tock_state_layout,
        sizeof(StatefulProbeStateMirror),
        sizeof(StatefulProbeBackgroundStateMirror));
    ASSERT_EQ(stateful.compiled_graph->node_layout.nodes.size(), 1u);

    auto stateful_storage = stateful.compiled_graph->node_layout.create_storage(resources);
    stateful_storage.initialize();
    auto stateful_tock_states =
        stateful.compiled_graph->tock_state_layout.create_storage(resources);
    stateful_tock_states.initialize();
    auto* state = static_cast<StatefulProbeStateMirror*>(stateful_storage.state_ptr(0));
    auto* background = static_cast<StatefulProbeBackgroundStateMirror*>(
        stateful_tock_states.state_ptr(0));
    ASSERT_NE(state, nullptr);
    ASSERT_NE(background, nullptr);
    auto const& stateful_node = stateful.compiled_graph->node_layout.nodes.front();
    auto const& stateful_tock_node =
        stateful.compiled_graph->tock_state_layout.nodes.front();
    EXPECT_EQ(stateful_node.state_alignment, alignof(StatefulProbeStateMirror));
    EXPECT_EQ(
        stateful_tock_node.state_alignment,
        alignof(StatefulProbeBackgroundStateMirror));
    EXPECT_EQ(
        reinterpret_cast<std::uintptr_t>(state) % alignof(StatefulProbeStateMirror),
        0u);
    EXPECT_EQ(
        reinterpret_cast<std::uintptr_t>(background)
            % alignof(StatefulProbeBackgroundStateMirror),
        0u);
    EXPECT_EQ(
        static_cast<std::byte*>(static_cast<void*>(state)),
        stateful_storage.buffer().data()
            + stateful_node.state_offset);

    stateful.compiled_graph->root_operations.tick_block(
        stateful_storage.buffer().data(), &empty_tick_invocation, index(17), 32);
    EXPECT_EQ(state->tick_calls, 1u);
    EXPECT_EQ(state->skip_calls, 0u);
    EXPECT_EQ(state->last_index, 17u);
    EXPECT_EQ(state->last_block_size, 32u);
    EXPECT_EQ(state->sample_rate, 88200u);
    EXPECT_EQ(state->observed_state_extent, sizeof(StatefulProbeStateMirror));
    EXPECT_EQ(state->observed_background_extent, 0u);
    EXPECT_EQ(background->tick_calls, 0u);
    EXPECT_EQ(background->skip_calls, 0u);
    EXPECT_EQ(background->last_index, 0u);
    EXPECT_EQ(background->last_block_size, 0u);
    EXPECT_EQ(background->observed_state_extent, 0u);
    EXPECT_EQ(background->observed_background_extent, 0u);

    stateful.compiled_graph->root_operations.tick_block(
        stateful_storage.buffer().data(), &empty_tick_invocation, index(73), 64);
    EXPECT_EQ(state->tick_calls, 2u);
    EXPECT_EQ(state->skip_calls, 0u);
    EXPECT_EQ(state->last_index, 73u);
    EXPECT_EQ(state->last_block_size, 64u);
    EXPECT_EQ(background->tick_calls, 0u);
    EXPECT_EQ(background->skip_calls, 0u);
    EXPECT_EQ(background->last_index, 0u);
    EXPECT_EQ(background->last_block_size, 0u);

    auto state_only = compile(graph_jit_state_only_module_id, 101);
    ASSERT_TRUE(state_only.succeeded())
        << (state_only.diagnostics.empty() ? "" : state_only.diagnostics.front().message);
    expect_single_node_canonical_regions(
        state_only.compiled_graph->node_layout,
        state_only.compiled_graph->tock_state_layout,
        sizeof(SingleSpanProbeMirror),
        0);
    ASSERT_EQ(state_only.compiled_graph->node_layout.nodes.size(), 1u);
    auto state_only_storage = state_only.compiled_graph->node_layout.create_storage(resources);
    state_only_storage.initialize();
    auto* state_only_value = static_cast<SingleSpanProbeMirror*>(
        state_only_storage.state_ptr(0));
    state_only.compiled_graph->root_operations.tick_block(
        state_only_storage.buffer().data(), &empty_tick_invocation, index(3), 8);
    ASSERT_NE(state_only_value, nullptr);
    EXPECT_EQ(state_only_value->calls, 1u);
    EXPECT_EQ(state_only_value->observed_state_extent, sizeof(SingleSpanProbeMirror));
    EXPECT_EQ(state_only_value->observed_background_extent, 0u);

    auto background_only = compile(graph_jit_background_only_module_id, 102);
    ASSERT_TRUE(background_only.succeeded())
        << (background_only.diagnostics.empty() ? "" : background_only.diagnostics.front().message);
    expect_single_node_canonical_regions(
        background_only.compiled_graph->node_layout,
        background_only.compiled_graph->tock_state_layout,
        0,
        sizeof(SingleSpanProbeMirror));
    ASSERT_EQ(background_only.compiled_graph->node_layout.nodes.size(), 1u);
    auto background_only_storage =
        background_only.compiled_graph->node_layout.create_storage(resources);
    background_only_storage.initialize();
    auto background_only_tock_states =
        background_only.compiled_graph->tock_state_layout.create_storage(resources);
    background_only_tock_states.initialize();
    auto* background_only_value = static_cast<SingleSpanProbeMirror*>(
        background_only_tock_states.state_ptr(0));
    background_only.compiled_graph->root_operations.tick_block(
        background_only_storage.buffer().data(), &empty_tick_invocation, index(5), 16);
    ASSERT_NE(background_only_value, nullptr);
    EXPECT_EQ(background_only_value->calls, 0u);
    EXPECT_EQ(background_only_value->observed_state_extent, 0u);
    EXPECT_EQ(background_only_value->observed_background_extent, 0u);

    auto stateless = compile(graph_jit_stateless_module_id, 103);
    ASSERT_TRUE(stateless.succeeded())
        << (stateless.diagnostics.empty() ? "" : stateless.diagnostics.front().message);
    expect_single_node_canonical_regions(
        stateless.compiled_graph->node_layout,
        stateless.compiled_graph->tock_state_layout,
        0,
        0);
    EXPECT_EQ(stateless.compiled_graph->node_layout.storage_size, 0u);
    EXPECT_NO_THROW(stateless.compiled_graph->root_operations.tick_block(nullptr, &empty_tick_invocation, index(9), 32));

}

TEST_F(GraphJitRuntimeFixture, GeneratedBackgroundRootsUseSuppliedBatchBindings)
{
    auto compiled = compile(graph_jit_background_execution_module_id, 104);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->background_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->background_evaluation_plan.nodes.size(), 1u);

    auto tock_states =
        compiled.compiled_graph->tock_state_layout.create_storage(resources);
    tock_states.initialize();
    auto* state = static_cast<BackgroundExecutionProbeStateMirror*>(
        tock_states.state_ptr(0));
    ASSERT_NE(state, nullptr);
    BackgroundOperationHookCapture operation_hooks{.state = state};

    BackgroundExecutionCapture capture;
    iv::IndexCoverage const empty;
    std::array output_changes{iv::OutputCoverageChange{
        .data = &capture,
        .previous_coverage_value = &empty,
        .publish_regions_value = &capture_background_regions,
        .publish_changed_value = &capture_background_change,
    }};
    std::array node_frames{iv::graph_jit::BackgroundNodeCall{
        .activity = iv::graph_jit::BackgroundNodeActivity::forward,
        .operation_frame = &operation_hooks,
        .prepare_operations = &prepare_background_operations,
        .finalize_operations = &finalize_background_operations,
        .forward = iv::ReflectedNodeForwardCoverageContext{
            .outputs = output_changes,
            .local_state_changed = true,
            .sample_rate = 88200,
        },
    }};
    iv::graph_jit::BackgroundEvaluationCall batch{.nodes = node_frames};

    compiled.compiled_graph->background_operations.propagate_forward(
        &batch);
    EXPECT_EQ(capture.coverage, index_coverage({{10, 13}}));
    EXPECT_EQ(capture.changed, capture.coverage);
    EXPECT_EQ(operation_hooks.prepare_calls, 0u);
    EXPECT_EQ(operation_hooks.finalize_calls, 0u);

    auto const requested = index_coverage({{10, 13}});
    auto const sample_geometry = *iv::continuous_value_storage_geometry(
        iv::continuous_value_type_id_v<iv::Sample>, std::nullopt);
    std::array<iv::Sample, 3> output_values{};
    std::array output_regions{iv::ContinuousRegionStorageView{
        .storage = reinterpret_cast<std::byte*>(output_values.data()),
        .range = iv::GlobalIndexRange{
            .begin = iv::GlobalIndex::from_bits(10),
            .extent = output_values.size(),
        },
        .geometry = sample_geometry,
        .channel_layout = iv::mono_planar_channel_layout,
    }};
    std::array outputs{iv::TockContinuousOutputPort{
        .regions = output_regions.data(),
        .region_count = output_regions.size(),
        .requested_coverage_value = &requested,
        .geometry = sample_geometry,
    }};
    node_frames[0].activity = iv::graph_jit::BackgroundNodeActivity::evaluate;
    node_frames[0].tock = iv::ReflectedNodeTockCoverageContext{
        .outputs = outputs,
        .background_state_storage = {
            static_cast<std::byte*>(tock_states.state_ptr(0)),
            sizeof(BackgroundExecutionProbeStateMirror),
        },
        .sample_rate = 88200,
    };
    compiled.compiled_graph->background_operations.evaluate(
        &batch);

    EXPECT_EQ(operation_hooks.prepare_calls, 1u);
    EXPECT_EQ(operation_hooks.finalize_calls, 1u);
    EXPECT_EQ(operation_hooks.calls_seen_by_prepare, 0u);
    EXPECT_EQ(operation_hooks.calls_seen_by_finalize, 1u);
    EXPECT_EQ(state->calls, 1u);
    EXPECT_EQ(state->sample_rate, 88200u);
    EXPECT_FLOAT_EQ(output_values[0].value, 10.0f);
    EXPECT_FLOAT_EQ(output_values[1].value, 11.0f);
    EXPECT_FLOAT_EQ(output_values[2].value, 12.0f);

    auto const& plan = compiled.compiled_graph->background_evaluation_plan;
    iv::BackgroundCoverageState coverage(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(
        plan, compiled.compiled_graph->specialization.sample_rate);
    auto prepared = workspace.prepare(
        compiled.compiled_graph->background_operations,
        coverage,
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {0},
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 0,
                    .required = index_coverage({{10, 13}}),
                },
            },
        });
    auto const& propagated = prepared.result();
    ASSERT_EQ(propagated.output_changes.size(), 1u);
    EXPECT_EQ(propagated.output_changes[0].port, 0u);
    EXPECT_EQ(propagated.output_changes[0].coverage, requested);
    EXPECT_EQ(propagated.output_changes[0].changed, requested);
    ASSERT_EQ(propagated.output_requirements.size(), 1u);
    EXPECT_EQ(propagated.output_requirements[0].port, 0u);
    EXPECT_EQ(propagated.output_requirements[0].required, requested);
    workspace.discard(std::move(prepared));
}

TEST_F(GraphJitRuntimeFixture,
     GeneratedRegionAndCoverageCallbacksPreserveAuthoredRegions)
{
    auto compiled = compile(graph_jit_multi_region_module_id, 304);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto& graph = *compiled.compiled_graph;
    auto const& plan = graph.background_evaluation_plan;

    auto const find_port = [&](std::string_view name,
                               iv::graph_jit::PortDirection direction)
        -> std::optional<iv::graph_jit::BackgroundPortIndex> {
        for (iv::graph_jit::BackgroundPortIndex port = 0;
             port < plan.ports.size(); ++port) {
            if (plan.ports[port].name == name
                && plan.ports[port].direction == direction) {
                return port;
            }
        }
        return std::nullopt;
    };

    auto const source_output = find_port(
        "multi-region-source-output",
        iv::graph_jit::PortDirection::output);
    auto const coverage_consumer_output = find_port(
        "multi-region-consumer-output",
        iv::graph_jit::PortDirection::output);
    auto const sequential_consumer_input = find_port(
        "multi-region-sequential-input",
        iv::graph_jit::PortDirection::input);
    auto const event_source_output = find_port(
        "multi-region-event-source-output",
        iv::graph_jit::PortDirection::output);
    auto const event_coverage_consumer_output = find_port(
        "multi-region-event-consumer-output",
        iv::graph_jit::PortDirection::output);
    auto const sequential_event_consumer_input = find_port(
        "multi-region-sequential-event-input",
        iv::graph_jit::PortDirection::input);
    auto const single_sample_source_output = find_port(
        "single-region-sample-source-output",
        iv::graph_jit::PortDirection::output);
    auto const single_sample_consumer_output = find_port(
        "single-region-sample-consumer-output",
        iv::graph_jit::PortDirection::output);
    auto const single_event_source_output = find_port(
        "single-region-event-source-output",
        iv::graph_jit::PortDirection::output);
    auto const single_event_consumer_output = find_port(
        "single-region-event-consumer-output",
        iv::graph_jit::PortDirection::output);
    ASSERT_TRUE(source_output.has_value());
    ASSERT_TRUE(coverage_consumer_output.has_value());
    ASSERT_TRUE(sequential_consumer_input.has_value());
    ASSERT_TRUE(event_source_output.has_value());
    ASSERT_TRUE(event_coverage_consumer_output.has_value());
    ASSERT_TRUE(sequential_event_consumer_input.has_value());
    ASSERT_TRUE(single_sample_source_output.has_value());
    ASSERT_TRUE(single_sample_consumer_output.has_value());
    ASSERT_TRUE(single_event_source_output.has_value());
    ASSERT_TRUE(single_event_consumer_output.has_value());

    auto tock_states = graph.tock_state_layout.create_storage(resources);
    tock_states.initialize();
    iv::BackgroundCoverageState coverage(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation(
        plan, graph.specialization.sample_rate);
    iv::PersistedDataStore data;
    auto data_reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();

    auto const requested = index_coverage({{10, 16}});
    iv::BackgroundEvaluationTransaction transaction{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        materializations,
        {
            .semantic_version = 1,
            .roots = {
                .locally_changed_nodes = {
                    plan.ports[*source_output].node,
                    plan.ports[*event_source_output].node,
                    plan.ports[*single_sample_source_output].node,
                    plan.ports[*single_event_source_output].node,
                },
                .input_demands = {
                    {
                        .port = *sequential_consumer_input,
                        .required = requested,
                    },
                    {
                        .port = *sequential_event_consumer_input,
                        .required = requested,
                    },
                },
                .output_demands = {
                    {
                        .port = *coverage_consumer_output,
                        .required = requested,
                    },
                    {
                        .port = *event_coverage_consumer_output,
                        .required = requested,
                    },
                    {
                        .port = *single_sample_consumer_output,
                        .required = index_coverage({{20, 24}}),
                    },
                    {
                        .port = *single_event_consumer_output,
                        .required = index_coverage({{30, 34}}),
                    },
                },
            },
        }};
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);
    ASSERT_TRUE(result->promoted_tick_materialization.has_value());

    auto const coverage_node = plan.ports[*coverage_consumer_output].node;
    ASSERT_LT(coverage_node, plan.nodes.size());
    ASSERT_TRUE(plan.nodes[coverage_node].tock_state_node.has_value());
    auto const* coverage_state =
        static_cast<MultiRegionCoverageStateMirror const*>(
            tock_states.state_ptr(
                *plan.nodes[coverage_node].tock_state_node));
    ASSERT_NE(coverage_state, nullptr);
    EXPECT_EQ(coverage_state->calls, 1u);
    EXPECT_EQ(coverage_state->region_count, 2u);
    EXPECT_EQ(coverage_state->first_begin, index(10).bits());
    EXPECT_EQ(coverage_state->second_begin, index(12).bits());
    EXPECT_FLOAT_EQ(coverage_state->first_overlap_value, 1.0f);
    EXPECT_FLOAT_EQ(coverage_state->second_overlap_value, 2.0f);

    auto const event_coverage_node =
        plan.ports[*event_coverage_consumer_output].node;
    ASSERT_LT(event_coverage_node, plan.nodes.size());
    ASSERT_TRUE(plan.nodes[event_coverage_node].tock_state_node.has_value());
    auto const* event_coverage_state =
        static_cast<MultiRegionEventCoverageStateMirror const*>(
            tock_states.state_ptr(
                *plan.nodes[event_coverage_node].tock_state_node));
    ASSERT_NE(event_coverage_state, nullptr);
    EXPECT_EQ(event_coverage_state->calls, 1u);
    EXPECT_EQ(event_coverage_state->region_count, 2u);
    EXPECT_EQ(event_coverage_state->first_event_count, 2u);
    EXPECT_EQ(event_coverage_state->second_event_count, 2u);
    EXPECT_EQ(event_coverage_state->first_last_time, index(13).bits());
    EXPECT_EQ(event_coverage_state->second_first_time, index(12).bits());

    auto const single_sample_node =
        plan.ports[*single_sample_consumer_output].node;
    ASSERT_LT(single_sample_node, plan.nodes.size());
    ASSERT_TRUE(plan.nodes[single_sample_node].tock_state_node.has_value());
    auto const* single_sample_state =
        static_cast<SingleRegionSampleStateMirror const*>(
            tock_states.state_ptr(
                *plan.nodes[single_sample_node].tock_state_node));
    ASSERT_NE(single_sample_state, nullptr);
    EXPECT_EQ(single_sample_state->calls, 1u);
    EXPECT_EQ(single_sample_state->begin, index(20).bits());
    EXPECT_EQ(single_sample_state->extent, 4u);
    EXPECT_FLOAT_EQ(single_sample_state->first_value, 4.0f);
    EXPECT_FLOAT_EQ(single_sample_state->last_value, 7.0f);

    auto const single_event_node =
        plan.ports[*single_event_consumer_output].node;
    ASSERT_LT(single_event_node, plan.nodes.size());
    ASSERT_TRUE(plan.nodes[single_event_node].tock_state_node.has_value());
    auto const* single_event_state =
        static_cast<SingleRegionEventStateMirror const*>(
            tock_states.state_ptr(
                *plan.nodes[single_event_node].tock_state_node));
    ASSERT_NE(single_event_state, nullptr);
    EXPECT_EQ(single_event_state->calls, 1u);
    EXPECT_EQ(single_event_state->begin, index(30).bits());
    EXPECT_EQ(single_event_state->extent, 4u);
    EXPECT_EQ(single_event_state->event_count, 2u);
    EXPECT_EQ(single_event_state->first_time, index(30).bits());
    EXPECT_EQ(single_event_state->last_time, index(33).bits());

    auto node_storage = graph.node_layout.create_storage(resources);
    node_storage.initialize();
    auto const layout_node_for_port = [&](iv::graph_jit::BackgroundPortIndex port)
        -> std::optional<std::size_t> {
        auto const background_node = plan.ports.at(port).node;
        auto const bundle = plan.nodes.at(background_node).bundle;
        auto const configured = graph.configured_graph->node_bundles
            .materialize_concrete_description(bundle);
        auto const layout_node = std::ranges::find(
            graph.node_layout.nodes,
            configured.node_storage.get(),
            &iv::NodeLayout::NodeRecord::node);
        if (layout_node == graph.node_layout.nodes.end()) return std::nullopt;
        return static_cast<std::size_t>(
            std::distance(graph.node_layout.nodes.begin(), layout_node));
    };
    auto const sequential_layout_node =
        layout_node_for_port(*sequential_consumer_input);
    auto const sequential_event_layout_node =
        layout_node_for_port(*sequential_event_consumer_input);
    ASSERT_TRUE(sequential_layout_node.has_value());
    ASSERT_TRUE(sequential_event_layout_node.has_value());
    auto* sequential_state = static_cast<MultiRegionSequentialStateMirror*>(
        node_storage.state_ptr(*sequential_layout_node));
    auto* sequential_event_state =
        static_cast<MultiRegionSequentialEventStateMirror*>(
            node_storage.state_ptr(*sequential_event_layout_node));
    ASSERT_NE(sequential_state, nullptr);
    ASSERT_NE(sequential_event_state, nullptr);

    iv::TickInvocationWorkspace tick_workspace(
        plan,
        graph.project_generation,
        data,
        graph.specialization.block_size);
    iv::TickInvocationFrame frame{
        data_reader,
        materialization_reader,
        tick_workspace,
        index(10),
        6};
    ASSERT_NE(graph.root_operations.tick_block, nullptr);
    graph.root_operations.tick_block(
        node_storage.buffer().data(), &frame.call(), index(10), 6);

    EXPECT_EQ(sequential_state->calls, 1u);
    EXPECT_EQ(sequential_state->sample_count, 6u);
    EXPECT_EQ(
        sequential_state->values,
        (std::array<float, 6>{1.0f, 1.0f, 3.0f, 3.0f, 2.0f, 2.0f}));
    EXPECT_EQ(sequential_event_state->calls, 1u);
    EXPECT_EQ(sequential_event_state->event_count, 4u);
    EXPECT_EQ(
        sequential_event_state->times,
        (std::array<std::uint64_t, 5>{
            index(10).bits(),
            index(12).bits(),
            index(13).bits(),
            index(15).bits(),
            0,
        }));

    // The narrow demand intersects only the first published Region directly.
    // Replacing that complete Region also intersects the second Region, so the
    // producer must receive both pre-allocated output Regions. Consumers remain
    // absent from this transaction because neither consumer output is demanded.
    iv::BackgroundEvaluationTransaction narrow_transaction{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        materializations,
        {
            .semantic_version = 2,
            .roots = {
                .locally_changed_nodes = {
                    plan.ports[*source_output].node,
                    plan.ports[*event_source_output].node,
                },
                .output_demands = {
                    {
                        .port = *source_output,
                        .required = index_coverage({{10, 11}}),
                    },
                    {
                        .port = *event_source_output,
                        .required = index_coverage({{10, 11}}),
                    },
                },
            },
        }};
    auto narrow_result = narrow_transaction.execute();
    ASSERT_TRUE(narrow_result.has_value()) << narrow_result.error();
    EXPECT_EQ(
        narrow_result->status, iv::BackgroundEvaluationStatus::committed);
    EXPECT_EQ(coverage_state->calls, 1u);
    EXPECT_EQ(event_coverage_state->calls, 1u);

    // Reverse propagation must widen the narrow output demand before the
    // Coverage transforms require their inputs. In particular, the event
    // transform cannot receive one partial input Region and two complete
    // pre-allocated output Regions.
    iv::BackgroundEvaluationTransaction narrow_transform_transaction{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        materializations,
        {
            .semantic_version = 3,
            .roots = {
                .locally_changed_nodes = {
                    coverage_node,
                    event_coverage_node,
                },
                .output_demands = {
                    {
                        .port = *coverage_consumer_output,
                        .required = index_coverage({{10, 11}}),
                    },
                    {
                        .port = *event_coverage_consumer_output,
                        .required = index_coverage({{10, 11}}),
                    },
                },
            },
        }};
    auto narrow_transform_result = narrow_transform_transaction.execute();
    ASSERT_TRUE(narrow_transform_result.has_value())
        << narrow_transform_result.error();
    EXPECT_EQ(
        narrow_transform_result->status,
        iv::BackgroundEvaluationStatus::committed);
    EXPECT_EQ(coverage_state->calls, 2u);
    EXPECT_EQ(coverage_state->region_count, 2u);
    EXPECT_EQ(event_coverage_state->calls, 2u);
    EXPECT_EQ(event_coverage_state->region_count, 2u);
}

TEST_F(GraphJitRuntimeFixture, NativeBackgroundCallbacksReceiveReadyFrontierBatch)
{
    auto compiled = compile(graph_jit_native_batch_background_module_id, 105);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->background_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->background_evaluation_plan.nodes.size(), 2u);
    ASSERT_EQ(compiled.compiled_graph->tock_state_layout.nodes.size(), 2u);
    ASSERT_EQ(
        compiled.compiled_graph->background_evaluation_plan
            .forward_ready_groups.size(),
        1u);
    EXPECT_EQ(
        compiled.compiled_graph->background_evaluation_plan
            .forward_ready_groups.front().size(),
        2u);
    ASSERT_EQ(
        compiled.compiled_graph->background_evaluation_plan
            .evaluation_ready_groups.size(),
        1u);
    EXPECT_EQ(
        compiled.compiled_graph->background_evaluation_plan
            .evaluation_ready_groups.front().size(),
        2u);

    auto tock_states =
        compiled.compiled_graph->tock_state_layout.create_storage(resources);
    tock_states.initialize();
    auto* first_state = static_cast<NativeBatchBackgroundProbeStateMirror*>(
        tock_states.state_ptr(0));
    auto* second_state = static_cast<NativeBatchBackgroundProbeStateMirror*>(
        tock_states.state_ptr(1));
    ASSERT_NE(first_state, nullptr);
    ASSERT_NE(second_state, nullptr);

    BackgroundExecutionCapture first_capture;
    BackgroundExecutionCapture second_capture;
    iv::IndexCoverage const empty;
    std::array first_changes{iv::OutputCoverageChange{
        .data = &first_capture,
        .previous_coverage_value = &empty,
        .publish_regions_value = &capture_background_regions,
        .publish_changed_value = &capture_background_change,
    }};
    std::array second_changes{iv::OutputCoverageChange{
        .data = &second_capture,
        .previous_coverage_value = &empty,
        .publish_regions_value = &capture_background_regions,
        .publish_changed_value = &capture_background_change,
    }};
    std::array node_frames{
        iv::graph_jit::BackgroundNodeCall{
            .activity = iv::graph_jit::BackgroundNodeActivity::forward,
            .forward = iv::ReflectedNodeForwardCoverageContext{
                .outputs = first_changes,
                .sample_rate = 88200,
            },
        },
        iv::graph_jit::BackgroundNodeCall{
            .activity = iv::graph_jit::BackgroundNodeActivity::forward,
            .forward = iv::ReflectedNodeForwardCoverageContext{
                .outputs = second_changes,
                .sample_rate = 88200,
            },
        },
    };
    iv::graph_jit::BackgroundEvaluationCall batch{.nodes = node_frames};

    compiled.compiled_graph->background_operations.propagate_forward(&batch);
    EXPECT_EQ(first_capture.coverage, index_coverage({{11, 13}}));
    EXPECT_EQ(first_capture.changed, first_capture.coverage);
    EXPECT_EQ(second_capture.coverage, index_coverage({{22, 24}}));
    EXPECT_EQ(second_capture.changed, second_capture.coverage);

    auto const no_op = +[](void*) {};
    node_frames[0].activity = iv::graph_jit::BackgroundNodeActivity::evaluate;
    node_frames[0].operation_frame = nullptr;
    node_frames[0].prepare_operations = no_op;
    node_frames[0].finalize_operations = no_op;
    node_frames[0].tock = iv::ReflectedNodeTockCoverageContext{
        .background_state_storage = {
            static_cast<std::byte*>(tock_states.state_ptr(0)),
            sizeof(NativeBatchBackgroundProbeStateMirror),
        },
        .sample_rate = 88200,
    };
    node_frames[1].activity = iv::graph_jit::BackgroundNodeActivity::evaluate;
    node_frames[1].operation_frame = nullptr;
    node_frames[1].prepare_operations = no_op;
    node_frames[1].finalize_operations = no_op;
    node_frames[1].tock = iv::ReflectedNodeTockCoverageContext{
        .background_state_storage = {
            static_cast<std::byte*>(tock_states.state_ptr(1)),
            sizeof(NativeBatchBackgroundProbeStateMirror),
        },
        .sample_rate = 88200,
    };

    compiled.compiled_graph->background_operations.evaluate(&batch);
    EXPECT_EQ(first_state->tock_batch_calls, 1u);
    EXPECT_EQ(first_state->last_lane_count, 2u);
    EXPECT_EQ(first_state->configured_id, 11u);
    EXPECT_EQ(first_state->sample_rate, 88200u);
    EXPECT_EQ(second_state->tock_batch_calls, 1u);
    EXPECT_EQ(second_state->last_lane_count, 2u);
    EXPECT_EQ(second_state->configured_id, 22u);
    EXPECT_EQ(second_state->sample_rate, 88200u);
}

// Reverse propagation needs a Random Access input. This connected module also
// exercises the background-only sample delivery used by that input.
TEST_F(GraphJitRuntimeFixture, NativeReversePropagationReceivesReadyFrontierBatch)
{
    auto compiled = compile(graph_jit_native_batch_reverse_module_id, 106);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->background_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->background_evaluation_plan.nodes.size(), 4u);
    ASSERT_EQ(
        compiled.compiled_graph->background_evaluation_plan
            .reverse_ready_groups.size(),
        2u);
    EXPECT_EQ(
        compiled.compiled_graph->background_evaluation_plan
            .reverse_ready_groups.front().size(),
        2u);

    iv::IndexCoverage const empty;
    iv::IndexCoverage first_requirement;
    iv::IndexCoverage second_requirement;
    auto const capture_requirement = +[](
        void* data,
        iv::IndexCoverage const& required) {
        *static_cast<iv::IndexCoverage*>(data) = required;
    };
    std::array first_inputs{iv::InputCoverageRequirement{
        .data = &first_requirement,
        .coverage_value = &empty,
        .publish_required_value = capture_requirement,
    }};
    std::array second_inputs{iv::InputCoverageRequirement{
        .data = &second_requirement,
        .coverage_value = &empty,
        .publish_required_value = capture_requirement,
    }};
    std::array node_frames{
        iv::graph_jit::BackgroundNodeCall{},
        iv::graph_jit::BackgroundNodeCall{},
        iv::graph_jit::BackgroundNodeCall{
            .activity = iv::graph_jit::BackgroundNodeActivity::reverse,
            .reverse = iv::ReflectedNodeReverseCoverageContext{
                .inputs = first_inputs,
                .sample_rate = 88200,
            },
        },
        iv::graph_jit::BackgroundNodeCall{
            .activity = iv::graph_jit::BackgroundNodeActivity::reverse,
            .reverse = iv::ReflectedNodeReverseCoverageContext{
                .inputs = second_inputs,
                .sample_rate = 88200,
            },
        },
    };
    iv::graph_jit::BackgroundEvaluationCall batch{.nodes = node_frames};

    compiled.compiled_graph->background_operations.propagate_reverse(&batch);
    EXPECT_EQ(first_requirement, index_coverage({{11, 13}}));
    EXPECT_EQ(second_requirement, index_coverage({{22, 24}}));
}

TEST_F(GraphJitRuntimeFixture, GeneratedRootInvokesPersistedTickCaptures)
{
    auto compiled = compile(graph_jit_tick_capture_module_id, 220);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& background =
        compiled.compiled_graph->background_evaluation_plan;
    ASSERT_EQ(background.tick_runtime.sample_captures.size(), 1u);
    ASSERT_EQ(background.tick_runtime.event_captures.size(), 1u);
    ASSERT_EQ(background.tick_runtime.nodes.size(), 1u);
    EXPECT_EQ(background.tick_runtime.nodes[0].sample_capture_begin, 0u);
    EXPECT_EQ(background.tick_runtime.nodes[0].sample_capture_count, 1u);
    EXPECT_EQ(background.tick_runtime.nodes[0].event_capture_begin, 0u);
    EXPECT_EQ(background.tick_runtime.nodes[0].event_capture_count, 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    GeneratedTickCaptureProbe probe{};
    std::array sample_captures{
        iv::graph_jit::TickSampleCaptureOperation{
            .context = &probe,
            .capture = &capture_generated_tick_samples,
        },
    };
    std::array event_captures{
        iv::graph_jit::TickEventCaptureOperation{
            .context = &probe,
            .capture = &capture_generated_tick_events,
        },
    };
    iv::graph_jit::TickInvocationCall invocation{
        .sample_captures = sample_captures,
        .event_captures = event_captures,
    };

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &invocation, index(64), 64);

    EXPECT_EQ(probe.sample_calls, 1u);
    EXPECT_EQ(probe.event_calls, 1u);
    EXPECT_EQ(probe.sample_index, index(64));
    EXPECT_EQ(probe.block_size, 64u);
    EXPECT_EQ(probe.sample_history, 2u);
    EXPECT_EQ(probe.sample_latency, 1u);
    EXPECT_FLOAT_EQ(probe.first_sample, 64.5f);
    EXPECT_FLOAT_EQ(probe.last_sample, 127.5f);
    EXPECT_EQ(probe.event_history, 2u);
    EXPECT_EQ(probe.event_latency, 1u);
    EXPECT_EQ(probe.event_count, 1u);
    EXPECT_EQ(probe.first_event_time, index(67));

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &invocation, index(1000), 4);
    EXPECT_EQ(probe.sample_calls, 2u);
    EXPECT_EQ(probe.event_calls, 2u);
    EXPECT_EQ(probe.sample_index, index(1000));
    EXPECT_EQ(probe.block_size, 4u);
    EXPECT_FLOAT_EQ(probe.first_sample, 1000.5f);
    EXPECT_FLOAT_EQ(probe.last_sample, 1003.5f);
    EXPECT_EQ(probe.event_count, 1u);
    EXPECT_EQ(probe.first_event_time, index(1003));
}

// A Tick/persisted output of latency 1 records, for each block, the positions
// that block finalizes (coverage_and_background_evaluation.md, dispositions):
// for a block at B of N positions, [B - 1, B + N - 1). The sample record holds
// each value after its revision, and the event record holds the event the
// block added in its latency window beside the one the previous block
// authored at the same position.
TEST_F(GraphJitRuntimeFixture, PersistedLatentTickOutputsRecordWhatEachBlockFinalizes)
{
    auto compiled = compile(graph_jit_revising_capture_module_id, 454);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto& graph = *compiled.compiled_graph;
    auto const& plan = graph.background_evaluation_plan;

    iv::PersistedDataStore data;
    auto data_reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    iv::TickInvocationWorkspace workspace(
        plan,
        graph.project_generation,
        data,
        graph.specialization.block_size,
        graph.root_operations.combine_sequential_input,
        graph.root_operations.initialize_sequential_input);
    auto const requirements = workspace.producer_requirements();
    ASSERT_EQ(requirements.size(), 2u);
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    std::atomic<bool> reservation_failed{false};
    std::vector<std::unique_ptr<iv::ProducerReserve>> reserves;
    std::vector<std::unique_ptr<iv::PendingQueue>> pending;
    std::optional<std::size_t> sample_endpoint;
    std::optional<std::size_t> event_endpoint;
    for (std::size_t endpoint = 0; endpoint < requirements.size(); ++endpoint) {
        reserves.push_back(std::make_unique<iv::ProducerReserve>(
            iv::realtime_produced_block_storage_size));
        pending.push_back(std::make_unique<iv::PendingQueue>(
            *reserves.back(), work_signal));
        auto const blocks = requirements[endpoint].maximum_blocks_per_callback;
        // Two callbacks, none drained in between.
        ASSERT_EQ(manager.maintain(*reserves.back(),
            {blocks, blocks, 2 * blocks}), 2 * blocks);
        workspace.bind_producer_endpoint(
            endpoint, *reserves.back(), *pending.back(), reservation_failed);
        (requirements[endpoint].kind == iv::PortKind::sample
            ? sample_endpoint
            : event_endpoint) = endpoint;
    }
    ASSERT_TRUE(sample_endpoint && event_endpoint);

    auto storage = graph.node_layout.create_storage(resources);
    storage.initialize();
    for (std::int64_t begin = 0; begin < 128; begin += 64) {
        iv::TickInvocationFrame frame{
            data_reader, materialization_reader, workspace, index(begin), 64};
        graph.root_operations.tick_block(
            storage.buffer().data(), &frame.call(), index(begin), 64);
    }
    EXPECT_FALSE(reservation_failed.load(std::memory_order_relaxed));

    auto const samples = captured_records(pending[*sample_endpoint]->pin());
    ASSERT_EQ(samples.size(), 2u);
    for (std::size_t block = 0; block < samples.size(); ++block) {
        SCOPED_TRACE("sample record of block " + std::to_string(block));
        auto const begin = static_cast<std::int64_t>(block * 64) - 1;
        auto const& record = samples[block];
        EXPECT_EQ(record.header.begin, index(begin));
        ASSERT_EQ(record.header.range_extent, 64u);
        ASSERT_EQ(record.payload.size(), 64 * sizeof(iv::Sample));
        std::array<iv::Sample, 64> values{};
        std::memcpy(values.data(), record.payload.data(), record.payload.size());
        EXPECT_FLOAT_EQ(values.front(), static_cast<float>(1000 + begin));
        EXPECT_FLOAT_EQ(values.back(), static_cast<float>(1000 + begin + 63));
    }

    auto const events = captured_records(pending[*event_endpoint]->pin());
    ASSERT_EQ(events.size(), 2u);
    // The first block finalizes [-1, 63): its event at -1. The second block
    // finalizes [63, 127): the first block's event at 63 and its own.
    std::array<std::size_t, 2> const counts{1, 2};
    std::array<std::int64_t, 2> const times{-1, 63};
    for (std::size_t block = 0; block < events.size(); ++block) {
        SCOPED_TRACE("event record of block " + std::to_string(block));
        auto const& record = events[block];
        EXPECT_EQ(
            record.header.begin,
            index(static_cast<std::int64_t>(block * 64) - 1));
        EXPECT_EQ(record.header.range_extent, 64u);
        ASSERT_EQ(record.header.event_count, counts[block]);
        ASSERT_EQ(
            record.payload.size(), counts[block] * sizeof(iv::TimedEvent));
        for (std::size_t event = 0; event < counts[block]; ++event) {
            iv::TimedEvent timed{};
            std::memcpy(
                &timed,
                record.payload.data() + event * sizeof(iv::TimedEvent),
                sizeof(timed));
            EXPECT_EQ(timed.time, index(times[block]));
        }
    }
}

// A latent persisted sample output at a non-project pace records, for each
// block, the values it finalizes: the block's values on the output's lattice,
// one value (its latency) earlier, each after its revision.
TEST_F(GraphJitRuntimeFixture, PersistedLatentPacedSampleOutputRecordsWhatEachBlockFinalizes)
{
    auto compiled = compile(graph_jit_revising_paced_capture_module_id, 462);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto& graph = *compiled.compiled_graph;
    auto const& plan = graph.background_evaluation_plan;
    auto const port = std::ranges::find_if(
        plan.ports, [](auto const& candidate) {
            return candidate.persisted_tick_output;
        });
    ASSERT_NE(port, plan.ports.end());
    auto const exponent = port->absolute_pace_exponent;

    iv::PersistedDataStore data;
    auto data_reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    iv::TickInvocationWorkspace workspace(
        plan,
        graph.project_generation,
        data,
        graph.specialization.block_size,
        graph.root_operations.combine_sequential_input,
        graph.root_operations.initialize_sequential_input);
    auto const requirements = workspace.producer_requirements();
    ASSERT_EQ(requirements.size(), 1u);
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    std::atomic<bool> reservation_failed{false};
    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::PendingQueue pending{reserve, work_signal};
    auto const blocks = requirements[0].maximum_blocks_per_callback;
    ASSERT_EQ(manager.maintain(reserve, {blocks, blocks, 2 * blocks}),
        2 * blocks);
    workspace.bind_producer_endpoint(0, reserve, pending, reservation_failed);

    auto storage = graph.node_layout.create_storage(resources);
    storage.initialize();
    for (std::int64_t begin = 0; begin < 128; begin += 64) {
        iv::TickInvocationFrame frame{
            data_reader, materialization_reader, workspace, index(begin), 64};
        graph.root_operations.tick_block(
            storage.buffer().data(), &frame.call(), index(begin), 64);
    }
    EXPECT_FALSE(reservation_failed.load(std::memory_order_relaxed));

    auto const records = captured_records(pending.pin());
    ASSERT_EQ(records.size(), 2u);
    for (std::size_t block = 0; block < records.size(); ++block) {
        SCOPED_TRACE("record of block " + std::to_string(block));
        auto const projection = iv::FractionalGlobalIndexRange::from_global_range(
            iv::GlobalIndexRange{
                .begin = index(static_cast<std::int64_t>(block * 64)),
                .extent = 64,
            }).try_project_onto_lattice(exponent);
        ASSERT_TRUE(projection.has_value());
        auto const first = iv::GlobalIndex::from_bits(projection->begin_index_bits)
            .retreated_by(1);
        auto const& record = records[block];
        EXPECT_EQ(record.header.begin, first);
        ASSERT_EQ(record.header.range_extent, projection->count);
        ASSERT_EQ(
            record.payload.size(), projection->count * sizeof(iv::Sample));
        std::vector<iv::Sample> values(projection->count);
        std::memcpy(values.data(), record.payload.data(), record.payload.size());
        auto const first_index = first.signed_value();
        EXPECT_FLOAT_EQ(values.front(), static_cast<float>(1000 + first_index));
        EXPECT_FLOAT_EQ(
            values.back(),
            static_cast<float>(
                1000 + first_index
                + static_cast<std::int64_t>(projection->count) - 1));
    }
}

TEST_F(GraphJitRuntimeFixture,
     PersistedTickRecordingsFeedPlanarSampleAndEventCoverageInputs)
{
    auto compiled = compile(
        graph_jit_persisted_recording_coverage_module_id, 307);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto& graph = *compiled.compiled_graph;
    auto const& plan = graph.background_evaluation_plan;

    auto const find_port = [&](std::string_view name,
                               iv::graph_jit::PortDirection direction)
        -> std::optional<iv::graph_jit::BackgroundPortIndex> {
        for (iv::graph_jit::BackgroundPortIndex port = 0;
             port < plan.ports.size(); ++port) {
            if (plan.ports[port].name == name
                && plan.ports[port].direction == direction) {
                return port;
            }
        }
        return std::nullopt;
    };
    auto const observed_samples = find_port(
        "observed-sample-coverage",
        iv::graph_jit::PortDirection::output);
    auto const observed_events = find_port(
        "observed-event-coverage",
        iv::graph_jit::PortDirection::output);
    auto const boundary_sample_source = find_port(
        "multi-region-source-output",
        iv::graph_jit::PortDirection::output);
    auto const boundary_event_source = find_port(
        "multi-region-event-source-output",
        iv::graph_jit::PortDirection::output);
    auto const boundary_samples = find_port(
        "module-boundary-sample-output",
        iv::graph_jit::PortDirection::output);
    auto const boundary_events = find_port(
        "module-boundary-event-output",
        iv::graph_jit::PortDirection::output);
    ASSERT_TRUE(observed_samples.has_value());
    ASSERT_TRUE(observed_events.has_value());
    ASSERT_TRUE(boundary_sample_source.has_value());
    ASSERT_TRUE(boundary_event_source.has_value());
    ASSERT_TRUE(boundary_samples.has_value());
    ASSERT_TRUE(boundary_events.has_value());

    iv::PersistedDataStore data;
    auto data_reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    iv::TickInvocationWorkspace workspace(
        plan,
        graph.project_generation,
        data,
        graph.specialization.block_size,
        graph.root_operations.combine_sequential_input,
        graph.root_operations.initialize_sequential_input);
    auto const requirements = workspace.producer_requirements();
    ASSERT_EQ(requirements.size(), 2u);
    EXPECT_EQ(requirements[0].kind, iv::PortKind::sample);
    EXPECT_EQ(requirements[1].kind, iv::PortKind::event);

    iv::ProducerReserve sample_reserve{
        iv::realtime_produced_block_storage_size};
    iv::ProducerReserve event_reserve{
        iv::realtime_produced_block_storage_size};
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue sample_pending{sample_reserve, work_signal};
    iv::PendingQueue event_pending{event_reserve, work_signal};
    std::atomic<bool> reservation_failed{false};
    auto const sample_blocks = requirements[0].maximum_blocks_per_callback;
    auto const event_blocks = requirements[1].maximum_blocks_per_callback;
    ASSERT_EQ(manager.maintain(sample_reserve,
        {sample_blocks, sample_blocks, 2 * sample_blocks}),
        2 * sample_blocks);
    ASSERT_EQ(manager.maintain(event_reserve,
        {event_blocks, event_blocks, 2 * event_blocks}),
        2 * event_blocks);
    workspace.bind_producer_endpoint(
        0, sample_reserve, sample_pending, reservation_failed);
    workspace.bind_producer_endpoint(
        1, event_reserve, event_pending, reservation_failed);

    auto node_storage = graph.node_layout.create_storage(resources);
    node_storage.initialize();
    {
        iv::TickInvocationFrame frame{
            data_reader,
            materialization_reader,
            workspace,
            index(64),
            8};
        graph.root_operations.tick_block(
            node_storage.buffer().data(), &frame.call(), index(64), 8);
    }
    EXPECT_FALSE(reservation_failed.load(std::memory_order_relaxed));

    std::array routes{
        iv::BackgroundProducedInputRoute{
            .output = requirements[0].output,
            .port = requirements[0].port,
            .kind = requirements[0].kind,
        },
        iv::BackgroundProducedInputRoute{
            .output = requirements[1].output,
            .port = requirements[1].port,
            .kind = requirements[1].kind,
        },
    };
    std::array selections{
        sample_pending.pin(),
        event_pending.pin(),
    };
    auto tock_states = graph.tock_state_layout.create_storage(resources);
    tock_states.initialize();
    iv::BackgroundCoverageState coverage(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation(
        plan, graph.specialization.sample_rate);
    auto const demanded = index_coverage({{64, 72}});
    iv::BackgroundEvaluationTransaction transaction{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        materializations,
        {
            .semantic_version = 1,
            .roots = {
                .locally_changed_nodes = {
                    plan.ports[*boundary_sample_source].node,
                    plan.ports[*boundary_event_source].node,
                },
                .output_demands = {
                    {.port = *observed_samples, .required = demanded},
                    {.port = *observed_events, .required = demanded},
                    {
                        .port = *boundary_samples,
                        .required = index_coverage({{10, 16}}),
                    },
                    {
                        .port = *boundary_events,
                        .required = index_coverage({{10, 16}}),
                    },
                },
            },
        },
        routes,
        selections,
    };
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);

    auto const consumer_node = plan.ports[*observed_samples].node;
    ASSERT_EQ(consumer_node, plan.ports[*observed_events].node);
    ASSERT_TRUE(plan.nodes[consumer_node].tock_state_node.has_value());
    auto const* state =
        static_cast<PersistedRecordingCoverageStateMirror const*>(
            tock_states.state_ptr(
                *plan.nodes[consumer_node].tock_state_node));
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->calls, 1u);
    EXPECT_EQ(state->sample_region_count, 1u);
    EXPECT_EQ(state->sample_layout,
        static_cast<std::uint64_t>(iv::SampleStreamLayout::planar));
    EXPECT_FLOAT_EQ(state->first_left, 64.0f);
    EXPECT_FLOAT_EQ(state->first_right, 1064.0f);
    EXPECT_FLOAT_EQ(state->last_left, 71.0f);
    EXPECT_FLOAT_EQ(state->last_right, 1071.0f);
    EXPECT_EQ(state->event_region_count, 1u);
    EXPECT_EQ(state->event_count, 1u);
    EXPECT_EQ(state->first_event_time, index(66).bits());

    auto const boundary_node = plan.ports[*boundary_samples].node;
    ASSERT_EQ(boundary_node, plan.ports[*boundary_events].node);
    ASSERT_TRUE(plan.nodes[boundary_node].tock_state_node.has_value());
    auto const* boundary_state =
        static_cast<ModuleBoundaryCoverageStateMirror const*>(
            tock_states.state_ptr(
                *plan.nodes[boundary_node].tock_state_node));
    ASSERT_NE(boundary_state, nullptr);
    EXPECT_EQ(boundary_state->calls, 1u);
    EXPECT_EQ(boundary_state->sample_region_count, 2u);
    EXPECT_FLOAT_EQ(boundary_state->first_overlap_value, 1.0f);
    EXPECT_FLOAT_EQ(boundary_state->second_overlap_value, 2.0f);
    EXPECT_EQ(boundary_state->event_region_count, 2u);
    EXPECT_EQ(boundary_state->first_event_count, 2u);
    EXPECT_EQ(boundary_state->second_event_count, 2u);

    auto published = data_reader.pin();
    auto const stored_samples = published->sample_region_views(
        requirements[0].output,
        iv::ChannelLayout{
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        },
        *iv::continuous_value_storage_geometry(
            iv::continuous_value_type_id_v<iv::Sample>, std::nullopt));
    ASSERT_EQ(stored_samples.size(), 1u);
    EXPECT_EQ(stored_samples.front().range, range(64, 72));
}

TEST_F(GraphJitRuntimeFixture, TypeOwnedFFTBlockStorageUsesProjectedPortExtents)
{
    auto compiled = compile(graph_jit_fft_value_module_id, 301);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    FFTValueConsumerStateMirror* state = nullptr;
    for (std::size_t node = 0;
         node < compiled.compiled_graph->node_layout.nodes.size(); ++node) {
        if (compiled.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(FFTValueConsumerStateMirror)) {
            ASSERT_EQ(state, nullptr);
            state = static_cast<FFTValueConsumerStateMirror*>(
                storage.state_ptr(node));
        }
    }
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(state->calls, 1u);
    EXPECT_EQ(state->root_block_size, 64u);
    EXPECT_EQ(state->input_block_size, 16u);
    EXPECT_EQ(state->extended_block_size, 17u);
    EXPECT_EQ(state->value_index, 0u);
    EXPECT_FLOAT_EQ(state->first_bin, 0.0f);
    EXPECT_FLOAT_EQ(state->last_bin, 56.0f);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 32);
    EXPECT_EQ(state->calls, 2u);
    EXPECT_EQ(state->root_block_size, 32u);
    EXPECT_EQ(state->input_block_size, 8u);
    EXPECT_EQ(state->extended_block_size, 9u);
    EXPECT_EQ(state->value_index, 16u);
    EXPECT_FLOAT_EQ(state->preceding_bin, 56.0f);
    EXPECT_FLOAT_EQ(state->first_bin, 60.0f);
    EXPECT_FLOAT_EQ(state->last_bin, 88.0f);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(97), 1);
    EXPECT_EQ(state->calls, 3u);
    EXPECT_EQ(state->root_block_size, 1u);
    EXPECT_EQ(state->input_block_size, 0u);
    EXPECT_EQ(state->extended_block_size, 1u);
    EXPECT_EQ(state->value_index, 25u);
    EXPECT_FLOAT_EQ(state->preceding_bin, 88.0f);

    // A transport seam changes the node-visible global lattice immediately,
    // while both the producer's sample history and the consumer's slow FFT
    // history remain contiguous in invocation order. This test producer anchors
    // each spectrum at the preceding four-sample Sequential window.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(1000), 4);
    EXPECT_EQ(state->calls, 4u);
    EXPECT_EQ(state->root_block_size, 4u);
    EXPECT_EQ(state->input_block_size, 1u);
    EXPECT_EQ(state->extended_block_size, 2u);
    EXPECT_EQ(state->value_index, 250u);
    EXPECT_FLOAT_EQ(state->preceding_bin, 88.0f);
    EXPECT_FLOAT_EQ(state->first_bin, 93.0f);
    EXPECT_FLOAT_EQ(state->last_bin, 93.0f);
}

TEST_F(GraphJitRuntimeFixture, TypeOwnedFractionalIndexStorageSupportsFastPace)
{
    auto compiled = compile(graph_jit_fractional_index_value_module_id, 302);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    FractionalIndexConsumerStateMirror* state = nullptr;
    for (std::size_t node = 0;
         node < compiled.compiled_graph->node_layout.nodes.size(); ++node) {
        if (compiled.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(FractionalIndexConsumerStateMirror)) {
            ASSERT_EQ(state, nullptr);
            state = static_cast<FractionalIndexConsumerStateMirror*>(
                storage.state_ptr(node));
        }
    }
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 3);
    EXPECT_EQ(state->calls, 1u);
    EXPECT_EQ(state->root_block_size, 3u);
    EXPECT_EQ(state->input_block_size, 6u);
    EXPECT_EQ(state->value_index, 0u);
    EXPECT_EQ(state->first_whole, 0u);
    EXPECT_EQ(state->first_fraction, 0x8000000000000000ull);
    EXPECT_EQ(state->last_whole, 5u);
    EXPECT_EQ(state->last_fraction, 0x8000000000000005ull);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(3), 2);
    EXPECT_EQ(state->calls, 2u);
    EXPECT_EQ(state->root_block_size, 2u);
    EXPECT_EQ(state->input_block_size, 4u);
    EXPECT_EQ(state->value_index, 6u);
    EXPECT_EQ(state->first_whole, 6u);
    EXPECT_EQ(state->first_fraction, 0x8000000000000006ull);
    EXPECT_EQ(state->last_whole, 9u);
    EXPECT_EQ(state->last_fraction, 0x8000000000000009ull);
}

TEST_F(GraphJitRuntimeFixture, PersistedTickCaptureAcceptsDynamicContinuousValues)
{
    auto compiled = compile(graph_jit_persisted_fft_value_module_id, 303);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& plan = compiled.compiled_graph->background_evaluation_plan;
    auto const found = std::ranges::find_if(
        plan.ports, [](auto const& port) {
            return port.persisted_tick_output;
        });
    ASSERT_NE(found, plan.ports.end());
    EXPECT_EQ(
        found->continuous_geometry.value_type,
        iv::ContinuousValueTypeId::fft_block);
    EXPECT_EQ(found->continuous_geometry.elements_per_value, 3u);
    ASSERT_EQ(plan.tick_runtime.sample_captures.size(), 1u);
}

TEST_F(GraphJitRuntimeFixture, PersistedTickCaptureAcceptsResolvedNonProjectPace)
{
    auto compiled = compile(
        graph_jit_persisted_non_project_rate_sample_module_id, 304);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& plan = compiled.compiled_graph->background_evaluation_plan;
    auto const found = std::ranges::find_if(
        plan.ports, [](auto const& port) {
            return port.persisted_tick_output;
        });
    ASSERT_NE(found, plan.ports.end());
    EXPECT_EQ(
        found->absolute_pace_exponent,
        iv::PortPace{2}.exponent());
    ASSERT_EQ(plan.tick_runtime.sample_captures.size(), 1u);
}

} // namespace iv::test::graph_jit_tests
