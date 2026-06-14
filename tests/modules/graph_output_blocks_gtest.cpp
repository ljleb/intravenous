#include <intravenous/runtime/graph_output_blocks.h>

#include <gtest/gtest.h>

#include <vector>

namespace iv {

TEST(GraphOutputBlocksTest, SampleBlockRoundTrips)
{
    GraphOutputBlocks blocks;
    std::vector<Sample> const block{ Sample{ 1.0f }, Sample{ 2.0f }, Sample{ 3.0f } };
    blocks.publish_sample_block(LaneId{ 7 }, block);

    auto const read = blocks.sample_block(LaneId{ 7 });
    ASSERT_EQ(read.size(), 3u);
    EXPECT_EQ(read[0].value, 1.0f);
    EXPECT_EQ(read[1].value, 2.0f);
    EXPECT_EQ(read[2].value, 3.0f);
}

TEST(GraphOutputBlocksTest, EventBlockRoundTrips)
{
    GraphOutputBlocks blocks;
    std::vector<TimedEvent> const events{
        TimedEvent{ .time = 4, .value = TriggerEvent{} },
        TimedEvent{ .time = 9, .value = TriggerEvent{} },
    };
    blocks.publish_event_block(LaneId{ 3 }, events);

    auto const read = blocks.event_block(LaneId{ 3 });
    ASSERT_EQ(read.size(), 2u);
    EXPECT_EQ(read[0].time, 4u);
    EXPECT_EQ(read[1].time, 9u);
}

TEST(GraphOutputBlocksTest, UnknownLaneReturnsEmpty)
{
    GraphOutputBlocks blocks;
    EXPECT_TRUE(blocks.sample_block(LaneId{ 99 }).empty());
    EXPECT_TRUE(blocks.event_block(LaneId{ 99 }).empty());
}

TEST(GraphOutputBlocksTest, RepublishReplacesPriorBlock)
{
    GraphOutputBlocks blocks;
    std::vector<Sample> const first{ Sample{ 1.0f }, Sample{ 2.0f } };
    std::vector<Sample> const second{ Sample{ 5.0f } };
    blocks.publish_sample_block(LaneId{ 1 }, first);
    blocks.publish_sample_block(LaneId{ 1 }, second);

    auto const read = blocks.sample_block(LaneId{ 1 });
    ASSERT_EQ(read.size(), 1u);
    EXPECT_EQ(read[0].value, 5.0f);
}

} // namespace iv
