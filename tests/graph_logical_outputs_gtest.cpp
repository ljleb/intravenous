#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>

#include <gtest/gtest.h>

namespace iv {

TEST(GraphLogicalOutputsTest, EnumeratesLogicalNodeOutputPorts)
{
    GraphBuilder g;
    auto node = _annotate_node_source_info(g.node<Sum<1>>().node_ref(), "node-1");
    (void)node;

    auto const outputs = g.logical_outputs();
    ASSERT_EQ(outputs.sample.size(), 1u);
    auto const &output = outputs.sample.front();
    EXPECT_EQ(output.logical_node_id, "node-1");
    EXPECT_EQ(output.member_ordinal, 0u);
    EXPECT_EQ(output.source.port, 0u);
    EXPECT_FALSE(output.has_existing_downstream_connection);
}

TEST(GraphLogicalOutputsTest, ReportsExistingDownstreamConnection)
{
    GraphBuilder g;
    auto node = _annotate_node_source_info(g.node<Sum<1>>().node_ref(), "node-1");
    auto sink = g.node<Sum<1>>();
    sink(node);

    auto const outputs = g.logical_outputs();
    ASSERT_EQ(outputs.sample.size(), 1u);
    EXPECT_TRUE(outputs.sample.front().has_existing_downstream_connection);
}

TEST(GraphLogicalOutputsTest, GroupsConcreteMembersOfSharedLogicalNode)
{
    GraphBuilder g;
    auto a = _annotate_node_source_info(g.node<Sum<1>>().node_ref(), "shared");
    auto b = _annotate_node_source_info(g.node<Sum<1>>().node_ref(), "shared");
    (void)a;
    (void)b;

    auto const outputs = g.logical_outputs();
    ASSERT_EQ(outputs.sample.size(), 2u);
    EXPECT_EQ(outputs.sample[0].logical_node_id, "shared");
    EXPECT_EQ(outputs.sample[1].logical_node_id, "shared");
    EXPECT_NE(outputs.sample[0].member_ordinal, outputs.sample[1].member_ordinal);
}

} // namespace iv
