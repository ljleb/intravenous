#include <intravenous/runtime/background_coverage_propagation.h>
#include <intravenous/runtime/graph_executor.h>

#include <intravenous/node/layout.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

iv::GlobalIndex index(std::int64_t value)
{
    return iv::GlobalIndex::from_signed(value);
}

iv::GlobalIndexRange range(std::int64_t begin, std::int64_t end)
{
    return iv::GlobalIndexRange::from_begin_and_end(index(begin), index(end));
}

iv::IndexCoverage coverage(
    std::initializer_list<std::pair<std::int64_t, std::int64_t>> ranges)
{
    iv::IndexCoverage result;
    for (auto const [begin, end] : ranges) result.include(range(begin, end));
    return result;
}

struct TickObservation {
    std::size_t calls = 0;
    iv::GlobalIndex sample_index{};
    std::size_t block_size = 0;
    std::byte* storage = nullptr;
    iv::graph_jit::TickInvocationCall const* invocation = nullptr;
};

TickObservation first_tick;
TickObservation second_tick;
std::size_t first_raw_offset = 0;
std::size_t second_raw_offset = 0;
unsigned migrated_raw_value = 0;
iv::IndexCoverage persisted_probe_coverage{};
std::size_t persisted_probe_evaluate_calls = 0;
bool throw_persisted_probe_evaluate = false;
bool omit_last_persisted_probe_sample = false;
std::size_t replay_probe_evaluate_calls = 0;
iv::PersistedDataStore* competing_persisted_store = nullptr;
bool publish_competing_persisted_snapshot = false;

struct BackgroundPropagationObservation {
    std::size_t source_forward_calls[2]{};
    std::size_t sink_forward_calls = 0;
    std::size_t sink_reverse_calls = 0;
    iv::IndexCoverage sink_input_coverage{};
    iv::IndexCoverage sink_input_changed{};
    iv::IndexCoverage sink_output_required{};
    bool throw_from_sink = false;
    bool throw_from_reverse = false;
};

BackgroundPropagationObservation background_observation;

void observe_first(
    std::byte* storage,
    iv::graph_jit::TickInvocationCall const* invocation,
    iv::GlobalIndex sample_index,
    std::size_t block_size)
{
    first_tick = TickObservation{
        .calls = first_tick.calls + 1,
        .sample_index = sample_index,
        .block_size = block_size,
        .storage = storage,
        .invocation = invocation,
    };
}

void observe_second(
    std::byte* storage,
    iv::graph_jit::TickInvocationCall const* invocation,
    iv::GlobalIndex sample_index,
    std::size_t block_size)
{
    second_tick = TickObservation{
        .calls = second_tick.calls + 1,
        .sample_index = sample_index,
        .block_size = block_size,
        .storage = storage,
        .invocation = invocation,
    };
}

void write_raw_state(
    std::byte* storage,
    iv::graph_jit::TickInvocationCall const*,
    iv::GlobalIndex,
    std::size_t)
{
    storage[first_raw_offset] = std::byte{0x5a};
}

void observe_raw_state(
    std::byte* storage,
    iv::graph_jit::TickInvocationCall const*,
    iv::GlobalIndex,
    std::size_t)
{
    migrated_raw_value = std::to_integer<unsigned>(storage[second_raw_offset]);
}

void propagate_background_forward(
    std::byte*,
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    auto nodes = static_cast<
        std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes);
    for (std::size_t node = 0; node < nodes.size(); ++node) {
        auto& frame = nodes[node];
        if (!iv::graph_jit::has_activity(
                frame.activity,
                iv::graph_jit::BackgroundNodeActivity::forward)) {
            continue;
        }
        auto outputs = static_cast<std::span<iv::OutputCoverageChange>>(
            frame.forward.outputs);
        if (node < 2) {
            ++background_observation.source_forward_calls[node];
            auto const coverage = node == 0
                ? coverage({{10, 13}})
                : coverage({{20, 22}});
            outputs[0].publish_coverage(coverage);
            outputs[0].change(coverage);
            continue;
        }

        ++background_observation.sink_forward_calls;
        if (background_observation.throw_from_sink) {
            throw std::runtime_error("background propagation probe failure");
        }
        auto const inputs = static_cast<
            std::span<iv::InputCoverageChange const>>(frame.forward.inputs);
        background_observation.sink_input_coverage = inputs[0].coverage();
        background_observation.sink_input_changed = inputs[0].changed();
        outputs[0].publish_coverage(inputs[0].coverage());
        outputs[0].change(inputs[0].changed());
    }
}

void propagate_background_reverse(
    std::byte*,
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    auto nodes = static_cast<
        std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes);
    auto& sink = nodes[2];
    if (!iv::graph_jit::has_activity(
            sink.activity,
            iv::graph_jit::BackgroundNodeActivity::reverse)) {
        return;
    }
    ++background_observation.sink_reverse_calls;
    if (background_observation.throw_from_reverse) {
        throw std::runtime_error("background reverse propagation probe failure");
    }
    auto const outputs = static_cast<
        std::span<iv::OutputCoverageRequirement const>>(sink.reverse.outputs);
    auto inputs = static_cast<std::span<iv::InputCoverageRequirement>>(
        sink.reverse.inputs);
    background_observation.sink_output_required = outputs[0].required();
    inputs[0].require(outputs[0].required());
}

void no_op_background_evaluate(
    std::byte*,
    iv::graph_jit::BackgroundEvaluationCall*)
{}

void propagate_persisted_probe_forward(
    std::byte*,
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    auto nodes = static_cast<
        std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes);
    for (auto& node : nodes) {
        if (!iv::graph_jit::has_activity(
                node.activity,
                iv::graph_jit::BackgroundNodeActivity::forward)) {
            continue;
        }
        auto outputs = static_cast<std::span<iv::OutputCoverageChange>>(
            node.forward.outputs);
        ASSERT_EQ(outputs.size(), 1u);
        outputs[0].publish_coverage(persisted_probe_coverage);
    }
}

void evaluate_persisted_probe(
    std::byte*,
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    auto nodes = static_cast<
        std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes);
    for (auto& node : nodes) {
        if (!iv::graph_jit::has_activity(
                node.activity,
                iv::graph_jit::BackgroundNodeActivity::evaluate)) {
            continue;
        }
        ++persisted_probe_evaluate_calls;
        ASSERT_NE(node.prepare_operations, nullptr);
        ASSERT_NE(node.finalize_operations, nullptr);
        node.prepare_operations(node.operation_frame);

        auto outputs = static_cast<std::span<iv::TockSampleOutputPort>>(
            node.tock.outputs);
        ASSERT_EQ(outputs.size(), 1u);
        for (auto const region : outputs[0].requested_coverage()) {
            for (std::uint64_t offset = 0; offset < region.extent; ++offset) {
                auto const position = region.begin.advanced_by(offset);
                if (omit_last_persisted_probe_sample
                    && offset + 1 == region.extent) {
                    continue;
                }
                outputs[0].write(
                    position, 0,
                    iv::Sample{100.0f
                        + static_cast<float>(position.signed_value())});
            }
        }
        if (throw_persisted_probe_evaluate) {
            throw std::runtime_error("persisted background probe failure");
        }
        node.finalize_operations(node.operation_frame);
        if (publish_competing_persisted_snapshot) {
            auto candidate = competing_persisted_store->begin_candidate(99);
            if (competing_persisted_store->publish(std::move(candidate)) !=
                iv::PersistedDataPublishResult::published) {
                throw std::runtime_error(
                    "competing persisted background probe publish failed");
            }
            publish_competing_persisted_snapshot = false;
        }
    }
}

iv::graph_jit::BackgroundEvaluationPlan probe_tock_plan(
    iv::OutputRetention retention,
    iv::graph_jit::PortStorageKind storage_kind)
{
    using namespace iv::graph_jit;
    BackgroundEvaluationPlan plan;
    plan.nodes = {{
        .bundle = 0,
        .authored_tock_execution = true,
        .outputs = {0},
        .accumulators = NodeAccumulatorRanges{
            .output_change_begin = 0,
            .output_change_count = 1,
            .output_requirement_begin = 0,
            .output_requirement_count = 1,
        },
    }};
    plan.ports = {{
        .node = 0,
        .configured_port = {0, iv::PortKind::sample, 0},
        .kind = iv::PortKind::sample,
        .direction = PortDirection::output,
        .authored_tock_output = true,
        .retention = retention,
        .sample_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .accumulators = PortAccumulatorIndices{
            .output_change = 0,
            .output_requirement = 0,
        },
    }};
    plan.storage.ports = {{
        .kind = iv::PortKind::sample,
        .storage = storage_kind,
        .source_port = iv::NodeBundlePortId{0, iv::PortKind::sample, 0},
        .output_port = 0,
        .sample_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .sample_channels = {0},
    }};
    plan.runtime.bindings = {{
        .node = 0,
        .port = 0,
        .kind = iv::PortKind::sample,
        .direction = PortDirection::output,
        .storage = {0},
    }};
    plan.runtime.port_bindings = {BackgroundBindingSlot{0}};
    plan.runtime.node_operations.resize(1);
    plan.runtime.node_replay_invocations.resize(1);
    plan.accumulators = CoverageAccumulatorCounts{
        .output_change_count = 1,
        .output_requirement_count = 1,
    };
    return plan;
}

iv::CompiledGraph persisted_tock_graph()
{
    iv::CompiledGraph graph;
    graph.project_generation = 1;
    graph.specialization.sample_rate = 48000;
    graph.specialization.block_size = 4;
    graph.background_evaluation_plan = probe_tock_plan(
        iv::OutputRetention::persisted,
        iv::graph_jit::PortStorageKind::persisted_data);
    graph.background_operations = {
        .propagate_forward = &propagate_persisted_probe_forward,
        .propagate_reverse = &no_op_background_evaluate,
        .evaluate = &evaluate_persisted_probe,
    };
    return graph;
}

iv::CompiledGraph ephemeral_tock_graph()
{
    auto graph = persisted_tock_graph();
    graph.background_evaluation_plan = probe_tock_plan(
        iv::OutputRetention::ephemeral,
        iv::graph_jit::PortStorageKind::background);
    return graph;
}

void propagate_replay_probe_forward(
    std::byte*, iv::graph_jit::BackgroundEvaluationCall* batch)
{
    for (auto& node :
         static_cast<std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes)) {
        if (iv::graph_jit::has_activity(
                node.activity,
                iv::graph_jit::BackgroundNodeActivity::forward)) {
            node.replay_forward(node.replay_context, node.forward);
        }
    }
}

void propagate_replay_probe_reverse(
    std::byte*, iv::graph_jit::BackgroundEvaluationCall* batch)
{
    for (auto& node :
         static_cast<std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes)) {
        if (iv::graph_jit::has_activity(
                node.activity,
                iv::graph_jit::BackgroundNodeActivity::reverse)) {
            node.replay_reverse(node.replay_context, node.reverse);
        }
    }
}

void evaluate_replay_probe(
    std::byte*, iv::graph_jit::BackgroundEvaluationCall* batch)
{
    for (auto& node :
         static_cast<std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes)) {
        if (!iv::graph_jit::has_activity(
                node.activity,
                iv::graph_jit::BackgroundNodeActivity::evaluate)) {
            continue;
        }
        ++replay_probe_evaluate_calls;
        node.prepare_operations(node.operation_frame);
        auto outputs = static_cast<
            std::span<iv::ReflectedSampleOutputPortBinding const>>(
            node.replay.sample_output_bindings);
        for (auto const region :
             static_cast<std::span<iv::GlobalIndexRange const>>(
                 node.replay_regions)) {
            for (iv::GlobalIndex::extent_type offset = 0;
                 offset < region.extent;
                 ++offset) {
                auto const position = region.begin.advanced_by(offset);
                for (std::size_t output = 0; output < outputs.size(); ++output) {
                    auto const& storage = outputs[output].storage;
                    auto* values = reinterpret_cast<iv::Sample*>(
                        storage.channels[0].storage);
                    auto const frame = static_cast<std::size_t>(position.bits()) &
                        (storage.frame_capacity - 1);
                    values[frame * storage.channels[0].frame_stride] =
                        iv::Sample{
                            100.0f * static_cast<float>(output)
                            + static_cast<float>(position.signed_value())};
                }
            }
        }
        node.finalize_operations(node.operation_frame);
    }
}

iv::CompiledGraph replay_probe_graph()
{
    using namespace iv::graph_jit;
    iv::CompiledGraph graph;
    graph.project_generation = 1;
    graph.specialization.sample_rate = 48000;
    graph.specialization.block_size = 4;
    auto& plan = graph.background_evaluation_plan;
    plan.nodes = {{
        .bundle = 0,
        .replays_tick = true,
        .uses_replay_forward_coverage = true,
        .uses_replay_reverse_coverage = true,
        .uses_imported_tick_block_for_replay = true,
        .replay_maximum_block_size = 4,
        .outputs = {0, 1},
        .accumulators = NodeAccumulatorRanges{
            .output_change_begin = 0,
            .output_change_count = 2,
            .output_requirement_begin = 0,
            .output_requirement_count = 2,
        },
    }};
    plan.ports = {
        {
            .node = 0,
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .replayed_tick_output = true,
            .retention = iv::OutputRetention::ephemeral,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .accumulators = PortAccumulatorIndices{
                .output_change = 0,
                .output_requirement = 0,
            },
        },
        {
            .node = 0,
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .replayed_tick_output = true,
            .retention = iv::OutputRetention::ephemeral,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .accumulators = PortAccumulatorIndices{
                .output_change = 1,
                .output_requirement = 1,
            },
        },
    };
    plan.storage.ports = {
        {
            .kind = iv::PortKind::sample,
            .storage = PortStorageKind::background,
            .output_port = 0,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .sample_channels = {0},
        },
        {
            .kind = iv::PortKind::sample,
            .storage = PortStorageKind::background,
            .output_port = 1,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .sample_channels = {0},
        },
    };
    plan.runtime.bindings = {
        {.node = 0,
         .port = 0,
         .kind = iv::PortKind::sample,
         .direction = PortDirection::output,
         .storage = {0}},
        {.node = 0,
         .port = 1,
         .kind = iv::PortKind::sample,
         .direction = PortDirection::output,
         .storage = {1}},
    };
    plan.runtime.port_bindings = {0, 1};
    plan.runtime.node_operations.resize(1);
    plan.runtime.replay_invocations = {{
        .node = 0,
        .maximum_block_size = 4,
        .output_bindings = {0, 1},
    }};
    plan.runtime.node_replay_invocations = {0};
    plan.accumulators = {
        .output_change_count = 2,
        .output_requirement_count = 2,
    };
    graph.background_operations = {
        .propagate_forward = &propagate_replay_probe_forward,
        .propagate_reverse = &propagate_replay_probe_reverse,
        .evaluate = &evaluate_replay_probe,
    };
    return graph;
}

iv::graph_jit::BackgroundEvaluationPlan background_fanin_plan()
{
    using namespace iv::graph_jit;
    BackgroundEvaluationPlan plan;
    plan.nodes = {
        BackgroundNodePlan{
            .bundle = 0,
            .authored_tock_execution = true,
            .outputs = {0},
            .accumulators = NodeAccumulatorRanges{
                .output_change_begin = 0,
                .output_change_count = 1,
                .output_requirement_begin = 0,
                .output_requirement_count = 1,
            },
        },
        BackgroundNodePlan{
            .bundle = 1,
            .authored_tock_execution = true,
            .outputs = {1},
            .accumulators = NodeAccumulatorRanges{
                .output_change_begin = 1,
                .output_change_count = 1,
                .output_requirement_begin = 1,
                .output_requirement_count = 1,
            },
        },
        BackgroundNodePlan{
            .bundle = 2,
            .authored_tock_execution = true,
            .inputs = {2},
            .outputs = {3},
            .accumulators = NodeAccumulatorRanges{
                .input_change_begin = 0,
                .input_change_count = 1,
                .output_change_begin = 2,
                .output_change_count = 1,
                .output_requirement_begin = 2,
                .output_requirement_count = 1,
                .input_requirement_begin = 0,
                .input_requirement_count = 1,
            },
        },
    };
    plan.ports = {
        BackgroundPortPlan{
            .node = 0,
            .configured_port = {0, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .authored_tock_output = true,
            .retention = iv::OutputRetention::ephemeral,
            .outgoing_connections = {0},
            .accumulators = PortAccumulatorIndices{
                .output_change = 0,
                .output_requirement = 0,
            },
        },
        BackgroundPortPlan{
            .node = 1,
            .configured_port = {1, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .authored_tock_output = true,
            .retention = iv::OutputRetention::ephemeral,
            .outgoing_connections = {1},
            .accumulators = PortAccumulatorIndices{
                .output_change = 1,
                .output_requirement = 1,
            },
        },
        BackgroundPortPlan{
            .node = 2,
            .configured_port = {2, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::input,
            .random_access_input = true,
            .incoming_connections = {0, 1},
            .accumulators = PortAccumulatorIndices{
                .input_change = 0,
                .input_requirement = 0,
            },
        },
        BackgroundPortPlan{
            .node = 2,
            .configured_port = {2, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .authored_tock_output = true,
            .retention = iv::OutputRetention::ephemeral,
            .accumulators = PortAccumulatorIndices{
                .output_change = 2,
                .output_requirement = 2,
            },
        },
    };
    plan.connections = {
        BackgroundConnectionPlan{
            .kind = iv::PortKind::sample,
            .source_coverage_ports = {0},
            .target_coverage_ports = {2},
        },
        BackgroundConnectionPlan{
            .kind = iv::PortKind::sample,
            .source_coverage_ports = {1},
            .target_coverage_ports = {2},
        },
    };
    plan.accumulators = CoverageAccumulatorCounts{
        .input_change_count = 1,
        .output_change_count = 3,
        .output_requirement_count = 3,
        .input_requirement_count = 1,
    };
    return plan;
}

iv::graph_jit::BackgroundEvaluationPlan background_mixed_output_plan()
{
    using namespace iv::graph_jit;
    BackgroundEvaluationPlan plan;
    plan.nodes = {
        BackgroundNodePlan{
            .bundle = 0,
            .authored_tock_execution = true,
            .outputs = {0, 1},
            .accumulators = NodeAccumulatorRanges{
                .output_change_begin = 0,
                .output_change_count = 2,
                .output_requirement_begin = 0,
                .output_requirement_count = 2,
            },
        },
    };
    plan.ports = {
        BackgroundPortPlan{
            .node = 0,
            .configured_port = {0, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .persisted_tick_output = true,
            .retention = iv::OutputRetention::persisted,
            .accumulators = PortAccumulatorIndices{
                .output_change = 0,
                .output_requirement = 0,
            },
        },
        BackgroundPortPlan{
            .node = 0,
            .configured_port = {0, iv::PortKind::sample, 1},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .authored_tock_output = true,
            .retention = iv::OutputRetention::ephemeral,
            .accumulators = PortAccumulatorIndices{
                .output_change = 1,
                .output_requirement = 1,
            },
        },
    };
    plan.accumulators = CoverageAccumulatorCounts{
        .output_change_count = 2,
        .output_requirement_count = 2,
    };
    return plan;
}

std::shared_ptr<iv::CompiledGraph const> compiled_graph(
    std::uint64_t generation,
    iv::CompiledGraphBlockFunction tick,
    iv::NodeLayout layout = iv::NodeLayoutBuilder(64).build(),
    iv::CompiledGraphBackgroundFunction background = nullptr)
{
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = generation;
    graph->specialization.sample_rate = 48000;
    graph->specialization.block_size = 64;
    graph->node_layout = std::move(layout);
    graph->root_operations.tick_block = tick;
    if (background) {
        graph->background_operations = {
            .propagate_forward = &no_op_background_evaluate,
            .propagate_reverse = &no_op_background_evaluate,
            .evaluate = background,
        };
    }
    return graph;
}

iv::NodeLayout persistent_raw_layout(std::size_t& offset)
{
    iv::NodeLayoutBuilder builder(64);
    auto const region = builder.declare_raw_region(
        1, alignof(std::byte), "graph-executor-test-state");
    auto layout = std::move(builder).build();
    offset = layout.regions[region.index].storage_offset;
    return layout;
}

class GraphExecutorFixture : public testing::Test {
protected:
    void SetUp() override
    {
        first_tick = {};
        second_tick = {};
        first_raw_offset = 0;
        second_raw_offset = 0;
        migrated_raw_value = 0;
        persisted_probe_coverage = coverage({{0, 8}});
        persisted_probe_evaluate_calls = 0;
        throw_persisted_probe_evaluate = false;
        omit_last_persisted_probe_sample = false;
        replay_probe_evaluate_calls = 0;
        competing_persisted_store = nullptr;
        publish_competing_persisted_snapshot = false;
    }
};

class BackgroundCoveragePropagationFixture : public testing::Test {
protected:
    void SetUp() override { background_observation = {}; }
};

TEST_F(GraphExecutorFixture, StagesActivatesAndDispatchesOnlyActiveGeneration)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_first);
    auto second = compiled_graph(2, &observe_second);

    EXPECT_THROW(executor.tick_block(index(0), 64), std::logic_error);
    EXPECT_EQ(
        executor.stage(first), iv::GraphExecutorStageResult::staged);
    EXPECT_FALSE(executor.active_generation());
    EXPECT_EQ(executor.pending_generation(), 1u);
    EXPECT_TRUE(executor.activate_pending(index(0)));
    EXPECT_EQ(executor.active_generation(), 1u);
    EXPECT_FALSE(executor.pending_generation());

    executor.tick_block(index(17), 32);
    EXPECT_EQ(first_tick.calls, 1u);
    EXPECT_EQ(first_tick.sample_index, index(17));
    EXPECT_EQ(first_tick.block_size, 32u);
    ASSERT_NE(first_tick.invocation, nullptr);
    EXPECT_TRUE(first_tick.invocation->sequential_sample_inputs.empty());
    EXPECT_TRUE(first_tick.invocation->sequential_event_inputs.empty());
    EXPECT_TRUE(first_tick.invocation->random_access_sample_inputs.empty());
    EXPECT_TRUE(first_tick.invocation->random_access_event_inputs.empty());

    EXPECT_EQ(
        executor.stage(second), iv::GraphExecutorStageResult::staged);
    executor.tick_block(index(49), 16);
    EXPECT_EQ(first_tick.calls, 2u);
    EXPECT_EQ(second_tick.calls, 0u);

    EXPECT_TRUE(executor.activate_pending(index(65)));
    EXPECT_EQ(executor.active_generation(), 2u);
    EXPECT_EQ(executor.active_graph(), second);
    executor.tick_block(index(65), 64);
    EXPECT_EQ(second_tick.calls, 1u);
    EXPECT_EQ(second_tick.sample_index, index(65));
    EXPECT_EQ(second_tick.block_size, 64u);
    EXPECT_FALSE(executor.activate_pending(index(129)));
}

TEST_F(GraphExecutorFixture, IgnoresStaleAndSupersedesOlderPendingGeneration)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_first);
    auto second = compiled_graph(2, &observe_second);

    EXPECT_EQ(
        executor.stage(first), iv::GraphExecutorStageResult::staged);
    EXPECT_EQ(
        executor.stage(first), iv::GraphExecutorStageResult::ignored_stale);
    EXPECT_EQ(
        executor.stage(second), iv::GraphExecutorStageResult::staged);
    EXPECT_EQ(executor.pending_generation(), 2u);
    EXPECT_TRUE(executor.activate_pending(index(0)));
    EXPECT_EQ(executor.active_generation(), 2u);
    EXPECT_EQ(
        executor.stage(first), iv::GraphExecutorStageResult::ignored_stale);
}

TEST_F(
    GraphExecutorFixture,
    PreparesRealtimeMigrationWithoutSnapshottingMutableStorage)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(
        1, &write_raw_state, persistent_raw_layout(first_raw_offset));
    auto second = compiled_graph(
        2, &observe_raw_state, persistent_raw_layout(second_raw_offset));

    ASSERT_EQ(
        executor.stage(first), iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending(index(0)));

    ASSERT_EQ(
        executor.stage(second), iv::GraphExecutorStageResult::staged);
    EXPECT_EQ(executor.active_generation(), 1u);
    // Migration preparation must not snapshot mutable realtime bytes. The
    // value written after staging is the value copied at activation.
    executor.tick_block(index(0), 64);
    ASSERT_TRUE(executor.activate_pending(index(64)));
    executor.tick_block(index(64), 64);
    EXPECT_EQ(migrated_raw_value, 0x5au);
}

TEST_F(GraphExecutorFixture, GenerationCutoverWakesTheBackgroundActor)
{
    iv::GraphExecutor executor;
    ASSERT_EQ(
        executor.stage(compiled_graph(1, &observe_first)),
        iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    ASSERT_EQ(
        executor.stage(compiled_graph(2, &observe_second)),
        iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending(index(0)));

    auto const deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{2};
    while (executor.background_generation() != 2u
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_EQ(executor.background_generation(), 2u);
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST_F(GraphExecutorFixture, RejectsInvalidGraphsAndBlockSizes)
{
    iv::GraphExecutor executor;
    EXPECT_THROW(executor.stage(nullptr), std::invalid_argument);

    ASSERT_EQ(
        executor.stage(compiled_graph(1, &observe_first)),
        iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    EXPECT_FALSE(executor.realtime_production_failures().any());
    EXPECT_FALSE(executor.realtime_capacity_maintenance_failures().any());
    EXPECT_THROW(executor.tick_block(index(0), 0), std::invalid_argument);
    EXPECT_THROW(executor.tick_block(index(0), 65), std::invalid_argument);
}

TEST_F(
    GraphExecutorFixture,
    BackgroundTransactionPublishesPersistedDataOnlyWhenItChanges)
{
    auto graph = persisted_tock_graph();
    iv::BackgroundCoverageState coverage(
        graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();

    iv::BackgroundEvaluationTransaction first{
        graph,
        nullptr,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 7,
            .roots = {
                .locally_changed_nodes = {0},
                .output_demands = {{
                    .port = 0,
                    .required = coverage({{0, 8}}),
                }},
            },
        }};
    auto first_result = first.execute();
    ASSERT_TRUE(first_result.has_value()) << first_result.error();
    ASSERT_TRUE(first_result->published_data.has_value());
    EXPECT_EQ(
        *first_result->published_data,
        (iv::PersistedDataVersion{.semantic = 7, .revision = 1}));
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
    // execute() releases its base pin even while the transaction object lives.
    EXPECT_EQ(data.retired_snapshot_count(), 1u);
    EXPECT_EQ(data.reclaim_retired(), 1u);

    auto const output = data.resolve_output(iv::PersistedOutputId{
        iv::GenerationLocalPersistedOutputId{
            .generation = 1,
            .port = 0,
            .kind = iv::PortKind::sample,
        }});
    {
        auto pin = reader.pin();
        auto const* region = pin->find_sample_region(output, index(0));
        ASSERT_NE(region, nullptr);
        EXPECT_EQ(region->range, range(0, 8));
        ASSERT_EQ(region->values.size(), 8u);
        EXPECT_FLOAT_EQ(region->values[0].value, 100.0f);
        EXPECT_FLOAT_EQ(region->values[3].value, 103.0f);
        EXPECT_FLOAT_EQ(region->values[4].value, 104.0f);
        EXPECT_FLOAT_EQ(region->values[7].value, 107.0f);
    }

    iv::BackgroundEvaluationTransaction unchanged{
        graph,
        nullptr,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 7,
            .roots = {
                .output_demands = {{
                    .port = 0,
                    .required = coverage({{0, 8}}),
                }},
            },
        }};
    auto unchanged_result = unchanged.execute();
    ASSERT_TRUE(unchanged_result.has_value()) << unchanged_result.error();
    EXPECT_FALSE(unchanged_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
    {
        auto pin = reader.pin();
        EXPECT_EQ(
            pin->version(),
            (iv::PersistedDataVersion{.semantic = 7, .revision = 1}));
    }

    persisted_probe_coverage = coverage({{0, 4}});
    iv::BackgroundEvaluationTransaction shrink{
        graph,
        nullptr,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 8,
            .roots = {.locally_changed_nodes = {0}},
        }};
    auto shrink_result = shrink.execute();
    ASSERT_TRUE(shrink_result.has_value()) << shrink_result.error();
    ASSERT_TRUE(shrink_result->published_data.has_value());
    EXPECT_EQ(
        *shrink_result->published_data,
        (iv::PersistedDataVersion{.semantic = 8, .revision = 2}));
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
    {
        auto pin = reader.pin();
        EXPECT_NE(pin->find_sample_region(output, index(0)), nullptr);
        EXPECT_EQ(pin->find_sample_region(output, index(4)), nullptr);
    }
}

TEST_F(
    GraphExecutorFixture,
    FailedPersistedBackgroundEvaluationPromotesNeitherDataNorCoverage)
{
    auto graph = persisted_tock_graph();
    iv::BackgroundCoverageState coverage(
        graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    auto const request = iv::BackgroundTransactionInputs{
        .semantic_version = 7,
        .roots = {
            .locally_changed_nodes = {0},
            .output_demands = {{
                .port = 0,
                .required = coverage({{0, 8}}),
            }},
        },
    };

    throw_persisted_probe_evaluate = true;
    iv::BackgroundEvaluationTransaction failed{
        graph, nullptr, coverage, propagation, data, request};
    auto failed_result = failed.execute();
    EXPECT_FALSE(failed_result.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
    {
        auto pin = reader.pin();
        EXPECT_EQ(pin->version(), (iv::PersistedDataVersion{}));
        EXPECT_EQ(pin->sample_region_count(), 0u);
    }

    throw_persisted_probe_evaluate = false;
    iv::BackgroundEvaluationTransaction demand_only{
        graph,
        nullptr,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 7,
            .roots = {
                .output_demands = {{
                    .port = 0,
                    .required = coverage({{0, 8}}),
                }},
            },
        }};
    auto demand_only_result = demand_only.execute();
    ASSERT_TRUE(demand_only_result.has_value()) << demand_only_result.error();
    EXPECT_FALSE(demand_only_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);

    iv::BackgroundEvaluationTransaction retry{
        graph, nullptr, coverage, propagation, data, request};
    auto retry_result = retry.execute();
    ASSERT_TRUE(retry_result.has_value()) << retry_result.error();
    ASSERT_TRUE(retry_result->published_data.has_value());
    EXPECT_EQ(retry_result->published_data->revision, 1u);
    EXPECT_EQ(persisted_probe_evaluate_calls, 2u);
}

TEST_F(
    GraphExecutorFixture,
    IncompletePersistedBackgroundEvaluationPromotesNeitherDataNorCoverage)
{
    auto graph = persisted_tock_graph();
    iv::BackgroundCoverageState coverage(
        graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    auto const request = iv::BackgroundTransactionInputs{
        .semantic_version = 7,
        .roots = {
            .locally_changed_nodes = {0},
            .output_demands = {{
                .port = 0,
                .required = coverage({{0, 8}}),
            }},
        },
    };

    omit_last_persisted_probe_sample = true;
    iv::BackgroundEvaluationTransaction incomplete{
        graph, nullptr, coverage, propagation, data, request};
    auto incomplete_result = incomplete.execute();
    ASSERT_FALSE(incomplete_result.has_value());
    EXPECT_NE(
        incomplete_result.error().find("did not initialize every selected"),
        std::string::npos);
    {
        auto pin = reader.pin();
        EXPECT_EQ(pin->version(), (iv::PersistedDataVersion{}));
        EXPECT_EQ(pin->sample_region_count(), 0u);
    }

    omit_last_persisted_probe_sample = false;
    iv::BackgroundEvaluationTransaction retry{
        graph, nullptr, coverage, propagation, data, request};
    auto retry_result = retry.execute();
    ASSERT_TRUE(retry_result.has_value()) << retry_result.error();
    ASSERT_TRUE(retry_result->published_data.has_value());
    EXPECT_EQ(retry_result->published_data->revision, 1u);
}

TEST_F(
    GraphExecutorFixture,
    BackgroundTransactionRunsReplayOverUnionButCompletesOnlyDemandedOutputs)
{
    auto graph = replay_probe_graph();
    iv::BackgroundCoverageState coverage(
        graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;

    iv::BackgroundEvaluationTransaction transaction{
        graph,
        nullptr,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 1,
            .roots = {
                .output_changes = {
                    {
                        .port = 0,
                        .coverage = coverage({{0, 8}}),
                    },
                    {
                        .port = 1,
                        .coverage = coverage({{0, 8}}),
                    },
                },
                .output_demands = {
                    {
                        .port = 0,
                        .required = coverage({{0, 4}}),
                    },
                    {
                        .port = 1,
                        .required = coverage({{4, 8}}),
                    },
                },
            },
        }};
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);
    EXPECT_FALSE(result->published_data.has_value());
    EXPECT_EQ(replay_probe_evaluate_calls, 1u);
}

TEST_F(
    GraphExecutorFixture,
    StalePersistedBackgroundEvaluationPromotesNeitherDataNorCoverage)
{
    auto graph = persisted_tock_graph();
    iv::BackgroundCoverageState coverage(
        graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    auto const request = iv::BackgroundTransactionInputs{
        .semantic_version = 7,
        .roots = {
            .locally_changed_nodes = {0},
            .output_demands = {{
                .port = 0,
                .required = coverage({{0, 8}}),
            }},
        },
    };

    competing_persisted_store = &data;
    publish_competing_persisted_snapshot = true;
    iv::BackgroundEvaluationTransaction stale{
        graph, nullptr, coverage, propagation, data, request};
    auto stale_result = stale.execute();
    ASSERT_TRUE(stale_result.has_value()) << stale_result.error();
    EXPECT_EQ(stale_result->status, iv::BackgroundEvaluationStatus::stale_base);
    EXPECT_FALSE(stale_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
    {
        auto pin = reader.pin();
        EXPECT_EQ(
            pin->version(),
            (iv::PersistedDataVersion{.semantic = 99, .revision = 1}));
        EXPECT_EQ(pin->sample_region_count(), 0u);
    }

    // execute() has returned, so the rejected transaction must no longer pin
    // the retired base even while the transaction object itself remains alive.
    EXPECT_EQ(data.retired_snapshot_count(), 1u);
    EXPECT_EQ(data.reclaim_retired(), 1u);

    iv::BackgroundEvaluationTransaction demand_only{
        graph,
        nullptr,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 99,
            .roots = {
                .output_demands = {{
                    .port = 0,
                    .required = coverage({{0, 8}}),
                }},
            },
        }};
    auto demand_only_result = demand_only.execute();
    ASSERT_TRUE(demand_only_result.has_value()) << demand_only_result.error();
    EXPECT_FALSE(demand_only_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
}

TEST_F(
    GraphExecutorFixture,
    StaleDataFreeBackgroundEvaluationPromotesNoCoverage)
{
    auto graph = ephemeral_tock_graph();
    iv::BackgroundCoverageState coverage(
        graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();

    competing_persisted_store = &data;
    publish_competing_persisted_snapshot = true;
    iv::BackgroundEvaluationTransaction stale{
        graph,
        nullptr,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 7,
            .roots = {
                .locally_changed_nodes = {0},
                .output_demands = {{
                    .port = 0,
                    .required = coverage({{0, 8}}),
                }},
            },
        }};
    auto stale_result = stale.execute();
    ASSERT_TRUE(stale_result.has_value()) << stale_result.error();
    EXPECT_EQ(stale_result->status, iv::BackgroundEvaluationStatus::stale_base);
    EXPECT_FALSE(stale_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
    {
        auto pin = reader.pin();
        EXPECT_EQ(
            pin->version(),
            (iv::PersistedDataVersion{.semantic = 99, .revision = 1}));
        EXPECT_EQ(pin->sample_region_count(), 0u);
    }

    EXPECT_EQ(data.retired_snapshot_count(), 1u);
    EXPECT_EQ(data.reclaim_retired(), 1u);

    iv::BackgroundEvaluationTransaction demand_only{
        graph,
        nullptr,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 99,
            .roots = {
                .output_demands = {{
                    .port = 0,
                    .required = coverage({{0, 8}}),
                }},
            },
        }};
    auto demand_only_result = demand_only.execute();
    ASSERT_TRUE(demand_only_result.has_value())
        << demand_only_result.error();
    EXPECT_EQ(
        demand_only_result->status, iv::BackgroundEvaluationStatus::committed);
    EXPECT_FALSE(demand_only_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
}

TEST_F(
    GraphExecutorFixture,
    RetainedDataFreeBackgroundEvaluationPreservesPersistedBaseValidation)
{
    auto persisted_graph = persisted_tock_graph();
    iv::BackgroundCoverageState persisted_coverage(
        persisted_graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace persisted_propagation{
        persisted_graph.background_evaluation_plan,
        persisted_graph.specialization.sample_rate};
    iv::PersistedDataStore data;

    iv::BackgroundEvaluationTransaction seed{
        persisted_graph,
        nullptr,
        persisted_coverage,
        persisted_propagation,
        data,
        {
            .semantic_version = 7,
            .roots = {
                .locally_changed_nodes = {0},
                .output_demands = {{
                    .port = 0,
                    .required = coverage({{0, 8}}),
                }},
            },
        }};
    auto seed_result = seed.execute();
    ASSERT_TRUE(seed_result.has_value()) << seed_result.error();
    ASSERT_TRUE(seed_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);

    auto ephemeral_graph = ephemeral_tock_graph();
    iv::BackgroundCoverageState ephemeral_coverage(
        ephemeral_graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace ephemeral_propagation{
        ephemeral_graph.background_evaluation_plan,
        ephemeral_graph.specialization.sample_rate};
    auto const demand = iv::CoveragePropagationRequest{
        .locally_changed_nodes = {0},
        .output_demands = {{
            .port = 0,
            .required = coverage({{0, 8}}),
        }},
    };

    iv::BackgroundEvaluationTransaction backwards_semantic{
        ephemeral_graph,
        nullptr,
        ephemeral_coverage,
        ephemeral_propagation,
        data,
        {
            .semantic_version = 6,
            .roots = demand,
        }};
    auto backwards_result = backwards_semantic.execute();
    ASSERT_FALSE(backwards_result.has_value());
    EXPECT_NE(
        backwards_result.error().find("semantic version cannot move backwards"),
        std::string::npos);

    ephemeral_graph.specialization.block_size = 8;
    iv::BackgroundEvaluationTransaction different_block_size{
        ephemeral_graph,
        nullptr,
        ephemeral_coverage,
        ephemeral_propagation,
        data,
        {
            .semantic_version = 7,
            .roots = demand,
        }};
    auto different_block_size_result = different_block_size.execute();
    ASSERT_TRUE(different_block_size_result.has_value())
        << different_block_size_result.error();

    // Neither rejected data-free transaction reaches evaluation or coverage
    // commit.
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    PreparedCoverageExposesEvaluationActivityWithoutCommitting)
{
    auto const plan = background_fanin_plan();
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };

    auto prepared = workspace.prepare(
        operations,
        nullptr,
        coverage,
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {0, 1},
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = coverage({{11, 21}}),
                },
            },
        });

    EXPECT_EQ(prepared.result().output_changes.size(), 3u);
    EXPECT_EQ(prepared.result().input_requirements.size(), 1u);
    EXPECT_EQ(prepared.result().output_requirements.size(), 3u);
    ASSERT_EQ(prepared.node_activity().size(), 3u);
    for (auto const activity : prepared.node_activity()) {
        EXPECT_TRUE(iv::graph_jit::has_activity(
            activity, iv::graph_jit::BackgroundNodeActivity::forward));
        EXPECT_TRUE(iv::graph_jit::has_activity(
            activity, iv::graph_jit::BackgroundNodeActivity::reverse));
        EXPECT_TRUE(iv::graph_jit::has_activity(
            activity, iv::graph_jit::BackgroundNodeActivity::evaluate));
    }

    workspace.discard(std::move(prepared));

    background_observation = {};
    auto retry = workspace.prepare(
        operations,
        nullptr,
        coverage,
        iv::CoveragePropagationRequest{
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = coverage({{10, 22}}),
                },
            },
        });
    EXPECT_TRUE(retry.result().output_requirements.empty());
    for (auto const activity : retry.node_activity()) {
        EXPECT_EQ(activity, iv::graph_jit::BackgroundNodeActivity::none);
    }
    workspace.discard(std::move(retry));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    BackgroundPropagationAccumulatesFaninAndReverseDemandOncePerNode)
{
    auto const plan = background_fanin_plan();
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };

    auto prepared = workspace.prepare(
        operations,
        nullptr,
        coverage,
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {0, 1},
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = coverage({{11, 21}}),
                },
            },
        });
    auto const result = workspace.commit(coverage, std::move(prepared));

    EXPECT_EQ(background_observation.source_forward_calls[0], 1u);
    EXPECT_EQ(background_observation.source_forward_calls[1], 1u);
    EXPECT_EQ(background_observation.sink_forward_calls, 1u);
    EXPECT_EQ(
        background_observation.sink_input_coverage,
        coverage({{10, 13}, {20, 22}}));
    EXPECT_EQ(
        background_observation.sink_input_changed,
        background_observation.sink_input_coverage);
    EXPECT_EQ(background_observation.sink_reverse_calls, 1u);
    EXPECT_EQ(
        background_observation.sink_output_required,
        coverage({{11, 13}, {20, 21}}));

    ASSERT_EQ(result.output_changes.size(), 3u);
    EXPECT_EQ(
        result.output_changes[2].coverage,
        coverage({{10, 13}, {20, 22}}));
    ASSERT_EQ(result.input_requirements.size(), 1u);
    EXPECT_EQ(result.input_requirements[0].port, 2u);
    EXPECT_EQ(
        result.input_requirements[0].required,
        coverage({{11, 13}, {20, 21}}));
    ASSERT_EQ(result.output_requirements.size(), 3u);
    EXPECT_EQ(result.output_requirements[0].port, 0u);
    EXPECT_EQ(
        result.output_requirements[0].required,
        coverage({{11, 13}}));
    EXPECT_EQ(result.output_requirements[1].port, 1u);
    EXPECT_EQ(
        result.output_requirements[1].required,
        coverage({{20, 21}}));
    EXPECT_EQ(result.output_requirements[2].port, 3u);
    EXPECT_EQ(
        result.output_requirements[2].required,
        coverage({{11, 13}, {20, 21}}));

    background_observation = {};
    prepared = workspace.prepare(
        operations,
        nullptr,
        coverage,
        iv::CoveragePropagationRequest{
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = coverage({{10, 22}}),
                },
            },
        });
    auto const demand_only = workspace.commit(
        coverage, std::move(prepared));
    EXPECT_EQ(background_observation.sink_forward_calls, 0u);
    EXPECT_EQ(background_observation.sink_reverse_calls, 1u);
    ASSERT_EQ(demand_only.output_requirements.size(), 3u);

    background_observation = {};
    prepared = workspace.prepare(
        operations,
        nullptr,
        coverage,
        iv::CoveragePropagationRequest{
            .input_demands = {
                iv::InputCoverageRequest{
                    .port = 2,
                    .required = coverage({{10, 22}}),
                },
            },
        });
    auto const propagation_result = workspace.commit(
        coverage, std::move(prepared));
    EXPECT_EQ(background_observation.sink_reverse_calls, 0u);
    ASSERT_EQ(propagation_result.input_requirements.size(), 1u);
    EXPECT_EQ(propagation_result.input_requirements[0].port, 2u);
    ASSERT_EQ(propagation_result.output_requirements.size(), 2u);
    EXPECT_EQ(propagation_result.output_requirements[0].port, 0u);
    EXPECT_EQ(propagation_result.output_requirements[1].port, 1u);
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    FailedForwardPreparationLeavesCommittedCoverageUnchanged)
{
    auto const plan = background_fanin_plan();
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };

    background_observation.throw_from_sink = true;
    EXPECT_THROW(
        static_cast<void>(workspace.prepare(
            operations,
            nullptr,
            coverage,
            iv::CoveragePropagationRequest{
                .locally_changed_nodes = {0, 1},
            })),
        std::runtime_error);

    background_observation = {};
    auto prepared = workspace.prepare(
        operations,
        nullptr,
        coverage,
        iv::CoveragePropagationRequest{
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = coverage({{10, 22}}),
                },
            },
        });
    auto const& result = prepared.result();
    EXPECT_TRUE(result.output_requirements.empty());
    EXPECT_EQ(background_observation.sink_reverse_calls, 0u);
    workspace.discard(std::move(prepared));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    FailedReversePreparationLeavesCommittedCoverageUnchanged)
{
    auto const plan = background_fanin_plan();
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };

    background_observation.throw_from_reverse = true;
    EXPECT_THROW(
        static_cast<void>(workspace.prepare(
            operations,
            nullptr,
            coverage,
            iv::CoveragePropagationRequest{
                .locally_changed_nodes = {0, 1},
                .output_demands = {
                    iv::OutputCoverageRequest{
                        .port = 3,
                        .required = coverage({{10, 22}}),
                    },
                },
            })),
        std::runtime_error);

    background_observation = {};
    auto prepared = workspace.prepare(
        operations,
        nullptr,
        coverage,
        iv::CoveragePropagationRequest{
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = coverage({{10, 22}}),
                },
            },
        });
    auto const& result = prepared.result();
    EXPECT_TRUE(result.output_requirements.empty());
    EXPECT_EQ(background_observation.sink_reverse_calls, 0u);
    workspace.discard(std::move(prepared));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    BackgroundInputRootRepresentsConnectionSetChange)
{
    auto const plan = background_fanin_plan();
    iv::BackgroundCoverageState committed(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };

    auto const selected_coverage = coverage({{30, 34}});
    auto prepared = workspace.prepare(
        operations,
        nullptr,
        committed,
        iv::CoveragePropagationRequest{
            .input_changes = {
                iv::InputCoverageChangeRequest{
                    .port = 2,
                    .coverage = selected_coverage,
                    .changed = selected_coverage,
                },
            },
        });
    auto const& result = prepared.result();

    EXPECT_EQ(background_observation.source_forward_calls[0], 0u);
    EXPECT_EQ(background_observation.source_forward_calls[1], 0u);
    EXPECT_EQ(background_observation.sink_forward_calls, 1u);
    EXPECT_EQ(background_observation.sink_input_coverage, selected_coverage);
    ASSERT_EQ(result.output_changes.size(), 1u);
    EXPECT_EQ(result.output_changes[0].port, 3u);
    EXPECT_EQ(result.output_changes[0].coverage, selected_coverage);
    EXPECT_EQ(result.output_changes[0].changed, selected_coverage);
    workspace.discard(std::move(prepared));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    BackgroundCallbackBindingsExcludeNonTockOutputs)
{
    auto const plan = background_mixed_output_plan();
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &no_op_background_evaluate,
        .evaluate = &no_op_background_evaluate,
    };

    auto prepared = workspace.prepare(
        operations,
        nullptr,
        coverage,
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {0},
        });
    auto const& result = prepared.result();

    ASSERT_EQ(result.output_changes.size(), 1u);
    EXPECT_EQ(result.output_changes[0].port, 1u);
    EXPECT_EQ(
        result.output_changes[0].coverage,
        coverage({{10, 13}}));
    workspace.discard(std::move(prepared));
}

} // namespace
