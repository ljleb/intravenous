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

struct IndexedPropagationObservation {
    std::size_t source_forward_calls[2]{};
    std::size_t sink_forward_calls = 0;
    std::size_t sink_reverse_calls = 0;
    iv::IndexedCoverage sink_input_coverage{};
    iv::IndexedCoverage sink_input_changed{};
    iv::IndexedCoverage sink_output_required{};
    bool throw_from_sink = false;
};

IndexedPropagationObservation indexed_observation;

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

void propagate_indexed_forward(
    std::byte*,
    iv::graph_jit::IndexedBatchFrame* batch)
{
    auto nodes = static_cast<
        std::span<iv::graph_jit::IndexedNodeBatchFrame>>(batch->nodes);
    for (std::size_t node = 0; node < nodes.size(); ++node) {
        auto& frame = nodes[node];
        if (!iv::graph_jit::has_activity(
                frame.activity,
                iv::graph_jit::IndexedNodeBatchActivity::forward)) {
            continue;
        }
        auto outputs = static_cast<std::span<iv::IndexedOutputChange>>(
            frame.forward.outputs);
        if (node < 2) {
            ++indexed_observation.source_forward_calls[node];
            auto const coverage = node == 0
                ? iv::IndexedCoverage{{{10, 13}}}
                : iv::IndexedCoverage{{{20, 22}}};
            outputs[0].publish_coverage(coverage);
            outputs[0].change(coverage);
            continue;
        }

        ++indexed_observation.sink_forward_calls;
        if (indexed_observation.throw_from_sink) {
            throw std::runtime_error("indexed propagation probe failure");
        }
        auto const inputs = static_cast<
            std::span<iv::IndexedInputChange const>>(frame.forward.inputs);
        indexed_observation.sink_input_coverage = inputs[0].coverage();
        indexed_observation.sink_input_changed = inputs[0].changed();
        outputs[0].publish_coverage(inputs[0].coverage());
        outputs[0].change(inputs[0].changed());
    }
}

void propagate_indexed_reverse(
    std::byte*,
    iv::graph_jit::IndexedBatchFrame* batch)
{
    auto nodes = static_cast<
        std::span<iv::graph_jit::IndexedNodeBatchFrame>>(batch->nodes);
    auto& sink = nodes[2];
    if (!iv::graph_jit::has_activity(
            sink.activity,
            iv::graph_jit::IndexedNodeBatchActivity::reverse)) {
        return;
    }
    ++indexed_observation.sink_reverse_calls;
    auto const outputs = static_cast<
        std::span<iv::IndexedOutputRequirement const>>(sink.reverse.outputs);
    auto inputs = static_cast<std::span<iv::IndexedInputRequirement>>(
        sink.reverse.inputs);
    indexed_observation.sink_output_required = outputs[0].required();
    inputs[0].require(outputs[0].required());
}

void no_op_indexed_evaluate(
    std::byte*,
    iv::graph_jit::IndexedBatchFrame*)
{}

iv::graph_jit::IndexedPlan indexed_fanin_plan()
{
    using namespace iv::graph_jit;
    IndexedPlan plan;
    plan.nodes = {
        IndexedNodePlan{
            .bundle = 0,
            .authored_tock_execution = true,
            .outputs = {0},
            .accumulators = IndexedNodeAccumulatorPlan{
                .output_change_begin = 0,
                .output_change_count = 1,
                .output_requirement_begin = 0,
                .output_requirement_count = 1,
            },
        },
        IndexedNodePlan{
            .bundle = 1,
            .authored_tock_execution = true,
            .outputs = {1},
            .accumulators = IndexedNodeAccumulatorPlan{
                .output_change_begin = 1,
                .output_change_count = 1,
                .output_requirement_begin = 1,
                .output_requirement_count = 1,
            },
        },
        IndexedNodePlan{
            .bundle = 2,
            .authored_tock_execution = true,
            .inputs = {2},
            .outputs = {3},
            .accumulators = IndexedNodeAccumulatorPlan{
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
    plan.endpoints = {
        IndexedEndpointPlan{
            .node = 0,
            .configured_port = {0, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = IndexedEndpointDirection::output,
            .authored_tock_output = true,
            .retention = iv::OutputRetention::ephemeral,
            .outgoing_connections = {0},
            .accumulators = IndexedAccumulatorSlotPlan{
                .output_change = 0,
                .output_requirement = 0,
            },
        },
        IndexedEndpointPlan{
            .node = 1,
            .configured_port = {1, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = IndexedEndpointDirection::output,
            .authored_tock_output = true,
            .retention = iv::OutputRetention::ephemeral,
            .outgoing_connections = {1},
            .accumulators = IndexedAccumulatorSlotPlan{
                .output_change = 1,
                .output_requirement = 1,
            },
        },
        IndexedEndpointPlan{
            .node = 2,
            .configured_port = {2, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = IndexedEndpointDirection::input,
            .random_access_input = true,
            .incoming_connections = {0, 1},
            .accumulators = IndexedAccumulatorSlotPlan{
                .input_change = 0,
                .input_requirement = 0,
            },
        },
        IndexedEndpointPlan{
            .node = 2,
            .configured_port = {2, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = IndexedEndpointDirection::output,
            .authored_tock_output = true,
            .retention = iv::OutputRetention::ephemeral,
            .accumulators = IndexedAccumulatorSlotPlan{
                .output_change = 2,
                .output_requirement = 2,
            },
        },
    };
    plan.connections = {
        IndexedConnectionPlan{
            .kind = iv::PortKind::sample,
            .source_endpoints = {0},
            .target_endpoints = {2},
        },
        IndexedConnectionPlan{
            .kind = iv::PortKind::sample,
            .source_endpoints = {1},
            .target_endpoints = {2},
        },
    };
    plan.accumulators = IndexedAccumulatorPlan{
        .input_change_count = 1,
        .output_change_count = 3,
        .output_requirement_count = 3,
        .input_requirement_count = 1,
    };
    return plan;
}

std::shared_ptr<iv::CompiledGraph const> indexed_compiled_graph(
    std::uint64_t generation)
{
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = generation;
    graph->specialization.sample_rate = 48000;
    graph->specialization.block_size = 64;
    graph->node_layout = iv::NodeLayoutBuilder(64).build();
    graph->indexed_plan = indexed_fanin_plan();
    graph->root_operations.tick_block = &no_op_tick;
    graph->indexed_operations = iv::CompiledGraphIndexedOperations{
        .propagate_forward = &propagate_indexed_forward,
        .propagate_reverse = &propagate_indexed_reverse,
        .evaluate = &no_op_indexed_evaluate,
    };
    return graph;
}

std::shared_ptr<iv::CompiledGraph const> indexed_mixed_output_graph(
    std::uint64_t generation)
{
    using namespace iv::graph_jit;
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = generation;
    graph->specialization.sample_rate = 48000;
    graph->specialization.block_size = 64;
    graph->node_layout = iv::NodeLayoutBuilder(64).build();
    graph->indexed_plan.nodes = {
        IndexedNodePlan{
            .bundle = 0,
            .authored_tock_execution = true,
            .outputs = {0, 1},
            .accumulators = IndexedNodeAccumulatorPlan{
                .output_change_begin = 0,
                .output_change_count = 2,
                .output_requirement_begin = 0,
                .output_requirement_count = 2,
            },
        },
    };
    graph->indexed_plan.endpoints = {
        IndexedEndpointPlan{
            .node = 0,
            .configured_port = {0, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = IndexedEndpointDirection::output,
            .persisted_tick_output = true,
            .retention = iv::OutputRetention::persisted,
            .accumulators = IndexedAccumulatorSlotPlan{
                .output_change = 0,
                .output_requirement = 0,
            },
        },
        IndexedEndpointPlan{
            .node = 0,
            .configured_port = {0, iv::PortKind::sample, 1},
            .kind = iv::PortKind::sample,
            .direction = IndexedEndpointDirection::output,
            .authored_tock_output = true,
            .retention = iv::OutputRetention::ephemeral,
            .accumulators = IndexedAccumulatorSlotPlan{
                .output_change = 1,
                .output_requirement = 1,
            },
        },
    };
    graph->indexed_plan.accumulators = IndexedAccumulatorPlan{
        .output_change_count = 2,
        .output_requirement_count = 2,
    };
    graph->root_operations.tick_block = &no_op_tick;
    graph->indexed_operations = iv::CompiledGraphIndexedOperations{
        .propagate_forward = &propagate_indexed_forward,
        .propagate_reverse = &no_op_indexed_evaluate,
        .evaluate = &no_op_indexed_evaluate,
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
        indexed_observation = {};
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

TEST_F(GraphExecutorFixture, RejectsInvalidGraphsAndBlockSizes)
{
    iv::GraphExecutor executor;
    EXPECT_THROW(executor.stage(nullptr), std::invalid_argument);

    auto invalid = std::make_shared<iv::CompiledGraph>();
    invalid->project_generation = 1;
    invalid->specialization.block_size = 64;
    invalid->node_layout = iv::NodeLayoutBuilder(64).build();
    EXPECT_THROW(executor.stage(invalid), std::invalid_argument);

    ASSERT_EQ(
        executor.stage(compiled_graph(1, &observe_first)),
        iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending());
    EXPECT_THROW(executor.tick_block(0, 0), std::invalid_argument);
    EXPECT_THROW(executor.tick_block(0, 65), std::invalid_argument);
}

TEST_F(
    GraphExecutorFixture,
    IndexedPropagationAccumulatesFaninAndReverseDemandOncePerNode)
{
    iv::GraphExecutor executor;
    ASSERT_EQ(
        executor.stage(indexed_compiled_graph(1)),
        iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending());

    auto const result = executor.propagate_indexed(
        iv::GraphExecutorIndexedPropagationRequest{
            .locally_changed_nodes = {0, 1},
            .output_demands = {
                iv::GraphExecutorIndexedOutputDemandRoot{
                    .endpoint = 3,
                    .required = iv::IndexedCoverage{{{11, 21}}},
                },
            },
        });

    EXPECT_EQ(indexed_observation.source_forward_calls[0], 1u);
    EXPECT_EQ(indexed_observation.source_forward_calls[1], 1u);
    EXPECT_EQ(indexed_observation.sink_forward_calls, 1u);
    EXPECT_EQ(
        indexed_observation.sink_input_coverage,
        (iv::IndexedCoverage{{{10, 13}, {20, 22}}}));
    EXPECT_EQ(
        indexed_observation.sink_input_changed,
        indexed_observation.sink_input_coverage);
    EXPECT_EQ(indexed_observation.sink_reverse_calls, 1u);
    EXPECT_EQ(
        indexed_observation.sink_output_required,
        (iv::IndexedCoverage{{{11, 13}, {20, 21}}}));

    ASSERT_EQ(result.output_changes.size(), 3u);
    EXPECT_EQ(
        result.output_changes[2].coverage,
        (iv::IndexedCoverage{{{10, 13}, {20, 22}}}));
    ASSERT_EQ(result.input_requirements.size(), 1u);
    EXPECT_EQ(result.input_requirements[0].endpoint, 2u);
    EXPECT_EQ(
        result.input_requirements[0].required,
        (iv::IndexedCoverage{{{11, 13}, {20, 21}}}));
    ASSERT_EQ(result.output_requirements.size(), 3u);
    EXPECT_EQ(result.output_requirements[0].endpoint, 0u);
    EXPECT_EQ(
        result.output_requirements[0].required,
        (iv::IndexedCoverage{{{11, 13}}}));
    EXPECT_EQ(result.output_requirements[1].endpoint, 1u);
    EXPECT_EQ(
        result.output_requirements[1].required,
        (iv::IndexedCoverage{{{20, 21}}}));
    EXPECT_EQ(result.output_requirements[2].endpoint, 3u);
    EXPECT_EQ(
        result.output_requirements[2].required,
        (iv::IndexedCoverage{{{11, 13}, {20, 21}}}));

    indexed_observation = {};
    auto const demand_only = executor.propagate_indexed(
        iv::GraphExecutorIndexedPropagationRequest{
            .output_demands = {
                iv::GraphExecutorIndexedOutputDemandRoot{
                    .endpoint = 3,
                    .required = iv::IndexedCoverage{{{10, 22}}},
                },
            },
        });
    EXPECT_EQ(indexed_observation.sink_forward_calls, 0u);
    EXPECT_EQ(indexed_observation.sink_reverse_calls, 1u);
    ASSERT_EQ(demand_only.output_requirements.size(), 3u);

    indexed_observation = {};
    auto const prepared_input = executor.propagate_indexed(
        iv::GraphExecutorIndexedPropagationRequest{
            .input_demands = {
                iv::GraphExecutorIndexedInputDemandRoot{
                    .endpoint = 2,
                    .required = iv::IndexedCoverage{{{10, 22}}},
                },
            },
        });
    EXPECT_EQ(indexed_observation.sink_reverse_calls, 0u);
    ASSERT_EQ(prepared_input.input_requirements.size(), 1u);
    EXPECT_EQ(prepared_input.input_requirements[0].endpoint, 2u);
    ASSERT_EQ(prepared_input.output_requirements.size(), 2u);
    EXPECT_EQ(prepared_input.output_requirements[0].endpoint, 0u);
    EXPECT_EQ(prepared_input.output_requirements[1].endpoint, 1u);
}

TEST_F(GraphExecutorFixture, FailedIndexedPropagationDoesNotCommitCoverage)
{
    iv::GraphExecutor executor;
    ASSERT_EQ(
        executor.stage(indexed_compiled_graph(1)),
        iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending());

    indexed_observation.throw_from_sink = true;
    EXPECT_THROW(
        static_cast<void>(executor.propagate_indexed(
            iv::GraphExecutorIndexedPropagationRequest{
                .locally_changed_nodes = {0, 1},
            })),
        std::runtime_error);

    indexed_observation = {};
    auto const result = executor.propagate_indexed(
        iv::GraphExecutorIndexedPropagationRequest{
            .output_demands = {
                iv::GraphExecutorIndexedOutputDemandRoot{
                    .endpoint = 3,
                    .required = iv::IndexedCoverage{{{10, 22}}},
                },
            },
        });
    EXPECT_TRUE(result.output_requirements.empty());
    EXPECT_EQ(indexed_observation.sink_reverse_calls, 0u);
}

TEST_F(GraphExecutorFixture, IndexedInputRootRepresentsConnectionSetChange)
{
    iv::GraphExecutor executor;
    ASSERT_EQ(
        executor.stage(indexed_compiled_graph(1)),
        iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending());

    auto const coverage = iv::IndexedCoverage{{{30, 34}}};
    auto const result = executor.propagate_indexed(
        iv::GraphExecutorIndexedPropagationRequest{
            .input_changes = {
                iv::GraphExecutorIndexedInputChangeRoot{
                    .endpoint = 2,
                    .coverage = coverage,
                    .changed = coverage,
                },
            },
        });

    EXPECT_EQ(indexed_observation.source_forward_calls[0], 0u);
    EXPECT_EQ(indexed_observation.source_forward_calls[1], 0u);
    EXPECT_EQ(indexed_observation.sink_forward_calls, 1u);
    EXPECT_EQ(indexed_observation.sink_input_coverage, coverage);
    ASSERT_EQ(result.output_changes.size(), 1u);
    EXPECT_EQ(result.output_changes[0].endpoint, 3u);
    EXPECT_EQ(result.output_changes[0].coverage, coverage);
    EXPECT_EQ(result.output_changes[0].changed, coverage);
}

TEST_F(GraphExecutorFixture, IndexedCallbackBindingsExcludeNonTockOutputs)
{
    iv::GraphExecutor executor;
    ASSERT_EQ(
        executor.stage(indexed_mixed_output_graph(1)),
        iv::GraphExecutorStageResult::staged);
    ASSERT_TRUE(executor.activate_pending());

    auto const result = executor.propagate_indexed(
        iv::GraphExecutorIndexedPropagationRequest{
            .locally_changed_nodes = {0},
        });

    ASSERT_EQ(result.output_changes.size(), 1u);
    EXPECT_EQ(result.output_changes[0].endpoint, 1u);
    EXPECT_EQ(
        result.output_changes[0].coverage,
        (iv::IndexedCoverage{{{10, 13}}}));
}

} // namespace
