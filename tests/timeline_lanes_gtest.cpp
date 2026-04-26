#include "runtime/lane_graph.h"
#include "runtime/timeline.h"

#include <gtest/gtest.h>

TEST(TimelineLanes, LaneIdsStartValidAndIncrease)
{
    iv::LaneIdAllocator allocator;

    auto const first = allocator.next();
    auto const second = allocator.next();

    EXPECT_TRUE(first);
    EXPECT_TRUE(second);
    EXPECT_NE(first, second);
    EXPECT_LT(first.value, second.value);
}

TEST(TimelineLanes, ConnectionsUseSharedPortKind)
{
    iv::LaneConnection connection {
        .source = iv::LaneEndpointId { .lane = iv::LaneId { 1 } },
        .target = iv::LaneInputId {
            .lane = iv::LaneId { 2 },
            .kind = iv::PortKind::event,
            .ordinal = 3,
        },
    };

    EXPECT_EQ(connection.source.lane.value, 1u);
    EXPECT_EQ(connection.target.lane.value, 2u);
    EXPECT_EQ(connection.target.kind, iv::PortKind::event);
    EXPECT_EQ(connection.target.ordinal, 3u);
}

TEST(TimelineLanes, GraphInputTargetsCanDescribeConcreteEventPorts)
{
    iv::GraphInputLaneTarget target {
        .logical_node_id = "node",
        .concrete_member_ordinal = 4u,
        .port_kind = iv::PortKind::event,
        .port_ordinal = 5u,
        .port_name = "gate",
        .port_type = "trigger",
    };

    ASSERT_TRUE(target.concrete_member_ordinal.has_value());
    EXPECT_EQ(*target.concrete_member_ordinal, 4u);
    EXPECT_EQ(target.port_kind, iv::PortKind::event);
    EXPECT_EQ(target.port_ordinal, 5u);
}

TEST(TimelineLanes, GraphInputLanesReuseIdsForStableTargets)
{
    iv::GraphInputLaneRegistry registry;
    std::vector<iv::GraphInputLaneTarget> targets {
        iv::GraphInputLaneTarget {
            .logical_node_id = "node",
            .port_kind = iv::PortKind::sample,
            .port_ordinal = 1,
            .port_name = "frequency",
            .port_type = "sample",
        },
    };

    auto const first = registry.reconcile(targets);
    auto const second = registry.reconcile(targets);

    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(first[0].id, second[0].id);
}

TEST(TimelineLanes, GraphInputLanesDropMissingTargetsWithoutDeletingConnections)
{
    iv::GraphInputLaneRegistry registry;
    auto const lanes = registry.reconcile({
        iv::GraphInputLaneTarget {
            .logical_node_id = "node",
            .concrete_member_ordinal = 2u,
            .port_kind = iv::PortKind::event,
            .port_ordinal = 0,
            .port_name = "gate",
            .port_type = "trigger",
        },
    });
    ASSERT_EQ(lanes.size(), 1u);

    iv::LaneConnection connection {
        .source = iv::LaneEndpointId { .lane = iv::LaneId { 99 } },
        .target = iv::LaneInputId {
            .lane = lanes[0].id,
            .kind = iv::PortKind::event,
            .ordinal = 0,
        },
    };

    EXPECT_TRUE(registry.target_for(lanes[0].id).has_value());

    auto const after_reload = registry.reconcile({});
    EXPECT_TRUE(after_reload.empty());
    EXPECT_FALSE(registry.target_for(lanes[0].id).has_value());

    auto const dangling = registry.dangling_connections({ connection });
    ASSERT_EQ(dangling.size(), 1u);
    EXPECT_EQ(dangling[0].connection, connection);
    ASSERT_TRUE(dangling[0].missing_target.has_value());
    EXPECT_EQ(dangling[0].missing_target->logical_node_id, "node");
    ASSERT_TRUE(dangling[0].missing_target->concrete_member_ordinal.has_value());
    EXPECT_EQ(*dangling[0].missing_target->concrete_member_ordinal, 2u);
}

TEST(TimelineLanes, TimelineOwnsGraphInputLaneReconciliation)
{
    iv::Timeline timeline;
    auto const lanes = timeline.reconcile_graph_input_lanes({
        iv::GraphInputLaneTarget {
            .logical_node_id = "logical",
            .port_kind = iv::PortKind::sample,
            .port_ordinal = 0,
            .port_name = "gain",
            .port_type = "sample",
        },
    });

    ASSERT_EQ(lanes.size(), 1u);
    auto const target = timeline.graph_input_lane_target(lanes[0].id);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(target->logical_node_id, "logical");

    timeline.reconcile_graph_input_lanes({});

    iv::LaneConnection connection {
        .source = iv::LaneEndpointId { .lane = iv::LaneId { 123 } },
        .target = iv::LaneInputId { .lane = lanes[0].id },
    };
    auto const dangling = timeline.dangling_graph_input_connections({ connection });
    ASSERT_EQ(dangling.size(), 1u);
    ASSERT_TRUE(dangling[0].missing_target.has_value());
    EXPECT_EQ(dangling[0].missing_target->port_name, "gain");
}
