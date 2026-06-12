#include <intravenous/graph/builder.h>
#include <intravenous/node/block_executor.h>

#include <gtest/gtest.h>

namespace {
struct CountingNode {
    int *ticks = nullptr;

    void tick_block(iv::TickBlockContext<CountingNode> const&) const
    {
        if (ticks) {
            *ticks += 1;
        }
    }
};
}

TEST(BlockNodeExecutor, TicksGraphOncePerCall)
{
    int ticks = 0;
    iv::GraphBuilder builder;
    (void)builder.node<CountingNode>(&ticks);
    builder.outputs();

    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(builder.build_root_node().graph),
        8);

    executor.tick_block(0);
    executor.tick_block(8);

    EXPECT_EQ(executor.block_size(), 8u);
    EXPECT_EQ(ticks, 2);
}

TEST(BlockNodeExecutor, ReloadReplacesTheRootGraph)
{
    int ticks_a = 0;
    iv::GraphBuilder builder_a;
    (void)builder_a.node<CountingNode>(&ticks_a);
    builder_a.outputs();

    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(builder_a.build_root_node().graph),
        8);

    executor.tick_block(0);
    EXPECT_EQ(ticks_a, 1);

    int ticks_b = 0;
    iv::GraphBuilder builder_b;
    (void)builder_b.node<CountingNode>(&ticks_b);
    builder_b.outputs();

    executor.reload(iv::TypeErasedNode(builder_b.build_root_node().graph));
    executor.tick_block(8);

    EXPECT_EQ(ticks_a, 1);
    EXPECT_EQ(ticks_b, 1);
}
