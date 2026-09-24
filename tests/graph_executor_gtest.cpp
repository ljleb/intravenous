#include <intravenous/runtime/graph_executor.h>

#include <intravenous/node/layout.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
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

std::shared_ptr<iv::CompiledGraph const> compiled_graph(
    std::uint64_t generation,
    iv::CompiledGraphBlockFunction tick,
    iv::NodeLayout layout = iv::NodeLayoutBuilder(64).build())
{
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = generation;
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

} // namespace
