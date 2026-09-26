#include <intravenous/runtime/graph_executor.h>

#include <intravenous/node/layout.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

namespace {

struct TickObservation {
    std::size_t calls = 0;
    std::size_t sample_index = 0;
    std::size_t block_size = 0;
    std::byte* storage = nullptr;
};

TickObservation first_tick;
TickObservation second_tick;
std::size_t first_raw_offset = 0;
std::size_t second_raw_offset = 0;
unsigned migrated_raw_value = 0;

struct BackgroundPropagationObservation {
    std::size_t source_forward_calls[2]{};
    std::size_t sink_forward_calls = 0;
    std::size_t sink_reverse_calls = 0;
    iv::Coverage sink_input_coverage{};
    iv::Coverage sink_input_changed{};
    iv::Coverage sink_output_required{};
    bool throw_from_sink = false;
};

BackgroundPropagationObservation background_observation;

void observe_first(
    std::byte* storage,
    std::size_t sample_index,
    std::size_t block_size)
{
    first_tick = TickObservation{
        .calls = first_tick.calls + 1,
        .sample_index = sample_index,
        .block_size = block_size,
        .storage = storage,
    };
}

void observe_second(
    std::byte* storage,
    std::size_t sample_index,
    std::size_t block_size)
{
    second_tick = TickObservation{
        .calls = second_tick.calls + 1,
        .sample_index = sample_index,
        .block_size = block_size,
        .storage = storage,
    };
}

void write_raw_state(std::byte* storage, std::size_t, std::size_t)
{
    storage[first_raw_offset] = std::byte{0x5a};
}

void observe_raw_state(std::byte* storage, std::size_t, std::size_t)
{
    migrated_raw_value = std::to_integer<unsigned>(storage[second_raw_offset]);
}

void no_op_tick(std::byte*, std::size_t, std::size_t) {}

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
                ? iv::Coverage{{{10, 13}}}
                : iv::Coverage{{{20, 22}}};
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

std::shared_ptr<iv::CompiledGraph const> background_compiled_graph(
    std::uint64_t generation)
{
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = generation;
    graph->specialization.sample_rate = 48000;
    graph->specialization.block_size = 64;
    graph->node_layout = iv::NodeLayoutBuilder(64).build();
    graph->background_evaluation_plan = background_fanin_plan();
    graph->root_operations.tick_block = &no_op_tick;
    graph->background_operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };
    return graph;
}

std::shared_ptr<iv::CompiledGraph const> background_mixed_output_graph(
    std::uint64_t generation)
{
    using namespace iv::graph_jit;
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = generation;
    graph->specialization.sample_rate = 48000;
    graph->specialization.block_size = 64;
    graph->node_layout = iv::NodeLayoutBuilder(64).build();
    graph->background_evaluation_plan.nodes = {
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
    graph->background_evaluation_plan.ports = {
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
    graph->background_evaluation_plan.accumulators = CoverageAccumulatorCounts{
        .output_change_count = 2,
        .output_requirement_count = 2,
    };
    graph->root_operations.tick_block = &no_op_tick;
    graph->background_operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &no_op_background_evaluate,
        .evaluate = &no_op_background_evaluate,
    };
    return graph;
}

std::shared_ptr<iv::CompiledGraph const> compiled_graph(
    std::uint64_t generation,
    iv::CompiledGraphBlockFunction tick,
    iv::NodeLayout layout = iv::NodeLayoutBuilder(64).build())
{
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = generation;
    graph->specialization.sample_rate = 48000;
    graph->specialization.block_size = 64;
    graph->node_layout = std::move(layout);
    graph->root_operations.tick_block = tick;
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
        background_observation = {};
    }
};

TEST_F(GraphExecutorFixture, StagesActivatesAndDispatchesOnlyActiveGeneration)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_first);
    auto second = compiled_graph(2, &observe_second);

    EXPECT_THROW(executor.tick_block(0, 64), std::logic_error);
    EXPECT_EQ(
        executor.stage(first), iv::GraphExecutorStageResult::staged);
    EXPECT_FALSE(executor.active_generation());
    EXPECT_EQ(executor.pending_generation(), 1u);
    EXPECT_TRUE(executor.activate_pending());
    EXPECT_EQ(executor.active_generation(), 1u);
    EXPECT_FALSE(executor.pending_generation());

    executor.tick_block(17, 32);
    EXPECT_EQ(first_tick.calls, 1u);
    EXPECT_EQ(first_tick.sample_index, 17u);
    EXPECT_EQ(first_tick.block_size, 32u);

    EXPECT_EQ(
        executor.stage(second), iv::GraphExecutorStageResult::staged);
    executor.tick_block(49, 16);
    EXPECT_EQ(first_tick.calls, 2u);
    EXPECT_EQ(second_tick.calls, 0u);

    EXPECT_TRUE(executor.activate_pending());
    EXPECT_EQ(executor.active_generation(), 2u);
    EXPECT_EQ(executor.active_graph(), second);
    executor.tick_block(65, 64);
    EXPECT_EQ(second_tick.calls, 1u);
    EXPECT_EQ(second_tick.sample_index, 65u);
    EXPECT_EQ(second_tick.block_size, 64u);
    EXPECT_FALSE(executor.activate_pending());
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
    EXPECT_TRUE(executor.activate_pending());
    EXPECT_EQ(executor.active_generation(), 2u);
    EXPECT_EQ(
        executor.stage(first), iv::GraphExecutorStageResult::ignored_stale);
}

TEST_F(GraphExecutorFixture, MigratesPersistentNodeStorageBeforeActivation)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(
        1, &write_raw_state, persistent_raw_layout(first_raw_offset));
    auto second = compiled_graph(
        2, &observe_raw_state, persistent_raw_layout(second_raw_offset));

    ASSERT_EQ(
        executor.stage(first), iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending());
    executor.tick_block(0, 64);

    ASSERT_EQ(
        executor.stage(second), iv::GraphExecutorStageResult::staged);
    EXPECT_EQ(executor.active_generation(), 1u);
    ASSERT_TRUE(executor.activate_pending());
    executor.tick_block(64, 64);
    EXPECT_EQ(migrated_raw_value, 0x5au);
}

TEST_F(GraphExecutorFixture, RejectsInvalidRequestsAndBlockSizes)
{
    iv::GraphExecutor executor;
    EXPECT_THROW(executor.stage(nullptr), std::invalid_argument);

    ASSERT_EQ(
        executor.stage(compiled_graph(1, &observe_first)),
        iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending());
    EXPECT_THROW(executor.tick_block(0, 0), std::invalid_argument);
    EXPECT_THROW(executor.tick_block(0, 65), std::invalid_argument);
}

TEST_F(
    GraphExecutorFixture,
    BackgroundPropagationAccumulatesFaninAndReverseDemandOncePerNode)
{
    iv::GraphExecutor executor;
    ASSERT_EQ(
        executor.stage(background_compiled_graph(1)),
        iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending());

    auto const result = executor.propagate_coverage(
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {0, 1},
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = iv::Coverage{{{11, 21}}},
                },
            },
        });

    EXPECT_EQ(background_observation.source_forward_calls[0], 1u);
    EXPECT_EQ(background_observation.source_forward_calls[1], 1u);
    EXPECT_EQ(background_observation.sink_forward_calls, 1u);
    EXPECT_EQ(
        background_observation.sink_input_coverage,
        (iv::Coverage{{{10, 13}, {20, 22}}}));
    EXPECT_EQ(
        background_observation.sink_input_changed,
        background_observation.sink_input_coverage);
    EXPECT_EQ(background_observation.sink_reverse_calls, 1u);
    EXPECT_EQ(
        background_observation.sink_output_required,
        (iv::Coverage{{{11, 13}, {20, 21}}}));

    ASSERT_EQ(result.output_changes.size(), 3u);
    EXPECT_EQ(
        result.output_changes[2].coverage,
        (iv::Coverage{{{10, 13}, {20, 22}}}));
    ASSERT_EQ(result.input_requirements.size(), 1u);
    EXPECT_EQ(result.input_requirements[0].port, 2u);
    EXPECT_EQ(
        result.input_requirements[0].required,
        (iv::Coverage{{{11, 13}, {20, 21}}}));
    ASSERT_EQ(result.output_requirements.size(), 3u);
    EXPECT_EQ(result.output_requirements[0].port, 0u);
    EXPECT_EQ(
        result.output_requirements[0].required,
        (iv::Coverage{{{11, 13}}}));
    EXPECT_EQ(result.output_requirements[1].port, 1u);
    EXPECT_EQ(
        result.output_requirements[1].required,
        (iv::Coverage{{{20, 21}}}));
    EXPECT_EQ(result.output_requirements[2].port, 3u);
    EXPECT_EQ(
        result.output_requirements[2].required,
        (iv::Coverage{{{11, 13}, {20, 21}}}));

    background_observation = {};
    auto const demand_only = executor.propagate_coverage(
        iv::CoveragePropagationRequest{
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = iv::Coverage{{{10, 22}}},
                },
            },
        });
    EXPECT_EQ(background_observation.sink_forward_calls, 0u);
    EXPECT_EQ(background_observation.sink_reverse_calls, 1u);
    ASSERT_EQ(demand_only.output_requirements.size(), 3u);

    background_observation = {};
    auto const propagation_result = executor.propagate_coverage(
        iv::CoveragePropagationRequest{
            .input_demands = {
                iv::InputCoverageRequest{
                    .port = 2,
                    .required = iv::Coverage{{{10, 22}}},
                },
            },
        });
    EXPECT_EQ(background_observation.sink_reverse_calls, 0u);
    ASSERT_EQ(propagation_result.input_requirements.size(), 1u);
    EXPECT_EQ(propagation_result.input_requirements[0].port, 2u);
    ASSERT_EQ(propagation_result.output_requirements.size(), 2u);
    EXPECT_EQ(propagation_result.output_requirements[0].port, 0u);
    EXPECT_EQ(propagation_result.output_requirements[1].port, 1u);
}

TEST_F(GraphExecutorFixture, FailedBackgroundPropagationDoesNotCommitCoverage)
{
    iv::GraphExecutor executor;
    ASSERT_EQ(
        executor.stage(background_compiled_graph(1)),
        iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending());

    background_observation.throw_from_sink = true;
    EXPECT_THROW(
        static_cast<void>(executor.propagate_coverage(
            iv::CoveragePropagationRequest{
                .locally_changed_nodes = {0, 1},
            })),
        std::runtime_error);

    background_observation = {};
    auto const result = executor.propagate_coverage(
        iv::CoveragePropagationRequest{
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = iv::Coverage{{{10, 22}}},
                },
            },
        });
    EXPECT_TRUE(result.output_requirements.empty());
    EXPECT_EQ(background_observation.sink_reverse_calls, 0u);
}

TEST_F(GraphExecutorFixture, BackgroundInputRootRepresentsConnectionSetChange)
{
    iv::GraphExecutor executor;
    ASSERT_EQ(
        executor.stage(background_compiled_graph(1)),
        iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending());

    auto const coverage = iv::Coverage{{{30, 34}}};
    auto const result = executor.propagate_coverage(
        iv::CoveragePropagationRequest{
            .input_changes = {
                iv::InputCoverageChangeRequest{
                    .port = 2,
                    .coverage = coverage,
                    .changed = coverage,
                },
            },
        });

    EXPECT_EQ(background_observation.source_forward_calls[0], 0u);
    EXPECT_EQ(background_observation.source_forward_calls[1], 0u);
    EXPECT_EQ(background_observation.sink_forward_calls, 1u);
    EXPECT_EQ(background_observation.sink_input_coverage, coverage);
    ASSERT_EQ(result.output_changes.size(), 1u);
    EXPECT_EQ(result.output_changes[0].port, 3u);
    EXPECT_EQ(result.output_changes[0].coverage, coverage);
    EXPECT_EQ(result.output_changes[0].changed, coverage);
}

TEST_F(GraphExecutorFixture, BackgroundCallbackBindingsExcludeNonTockOutputs)
{
    iv::GraphExecutor executor;
    ASSERT_EQ(
        executor.stage(background_mixed_output_graph(1)),
        iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending());

    auto const result = executor.propagate_coverage(
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {0},
        });

    ASSERT_EQ(result.output_changes.size(), 1u);
    EXPECT_EQ(result.output_changes[0].port, 1u);
    EXPECT_EQ(
        result.output_changes[0].coverage,
        (iv::Coverage{{{10, 13}}}));
}

} // namespace
