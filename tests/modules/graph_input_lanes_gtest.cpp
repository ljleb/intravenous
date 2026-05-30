#include "runtime/graph_input_lanes.h"

#include "graph/build_types.h"
#include "linker_event.h"
#include "runtime/graph_input_lanes_events.h"
#include "runtime/iv_module_instances.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <vector>

namespace {
struct GraphInputLanesWitness {
    std::vector<iv::TimelineLaneBatchUpdate> timeline_batches {};
};

GraphInputLanesWitness *g_graph_input_lanes_witness = nullptr;

iv::IvModuleInstanceBuilder make_instance_builder_with_ports()
{
    iv::IvModuleInstanceBuilder instance_builder {};
    auto &instance = instance_builder.instance;
    instance.instance_id = "instance:1";
    instance.definition_id = "definition:1";
    instance.module_root = std::filesystem::path("/tmp/module");
    instance.module_id = "iv.test.module";

    iv::IntrospectionLogicalNode node {};
    node.id = "node-1";
    node.kind = "TestNode";
    node.sample_inputs.push_back(iv::IntrospectionPortInfo {
        .name = "frequency",
        .type = "sample",
        .connectivity = iv::LogicalPortConnectivity::disconnected,
        .ordinal = 0,
        .default_value = 440.0f,
    });
    node.event_inputs.push_back(iv::IntrospectionPortInfo {
        .name = "trigger",
        .type = "event",
        .connectivity = iv::LogicalPortConnectivity::disconnected,
        .ordinal = 1,
    });

    instance.introspection.logical_nodes.push_back(std::move(node));
    return instance_builder;
}

IV_SUBSCRIBE_LINKER_EVENT(
    iv::GraphInputLanesTimelineBatchRequestedEvent,
    iv_runtime_graph_input_lanes_timeline_batch_requested_event,
    +[](iv::TimelineLaneBatchUpdate const &batch,
        iv::GraphInputLanesAckBuilder &builder) {
        if (g_graph_input_lanes_witness != nullptr) {
            g_graph_input_lanes_witness->timeline_batches.push_back(batch);
        }
        builder.succeed();
    });

class GraphInputLanesTest : public ::testing::Test {
protected:
    GraphInputLanesWitness witness {};

    void SetUp() override
    {
        g_graph_input_lanes_witness = &witness;
    }

    void TearDown() override
    {
        g_graph_input_lanes_witness = nullptr;
    }
};
} // namespace

TEST_F(GraphInputLanesTest, InstanceChangesPublishTimelineBatch)
{
    iv::GraphInputLanes lanes;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {make_instance_builder_with_ports()},
    });

    ASSERT_EQ(witness.timeline_batches.size(), 1u);
    EXPECT_GE(witness.timeline_batches.front().upserts.size(), 1u);
    EXPECT_TRUE(witness.timeline_batches.front().removals.empty());
}

TEST_F(GraphInputLanesTest, DeletingLastInstancePublishesTimelineRemovals)
{
    iv::GraphInputLanes lanes;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {make_instance_builder_with_ports()},
    });
    auto const created_count = witness.timeline_batches.front().upserts.size();
    witness.timeline_batches.clear();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .deleted_instance_ids = {"instance:1"},
    });

    ASSERT_EQ(witness.timeline_batches.size(), 1u);
    EXPECT_EQ(witness.timeline_batches.front().removals.size(), created_count);
}

TEST_F(GraphInputLanesTest, UpdatedInstancePublishesTimelineBatch)
{
    iv::GraphInputLanes lanes;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .updated = {make_instance_builder_with_ports()},
    });

    ASSERT_EQ(witness.timeline_batches.size(), 1u);
    EXPECT_GE(witness.timeline_batches.front().upserts.size(), 1u);
}

TEST_F(GraphInputLanesTest, SampleInputMutationsPublishTimelineBatch)
{
    iv::GraphInputLanes lanes;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {make_instance_builder_with_ports()},
    });
    witness.timeline_batches.clear();

    lanes.set_sample_input_value(iv::ProjectSetSampleInputValueRequest{
        .node_id = "node-1",
        .member_ordinal = std::nullopt,
        .input_ordinal = 0,
        .value = iv::Sample{0.5f},
    });

    ASSERT_EQ(witness.timeline_batches.size(), 1u);
    EXPECT_EQ(witness.timeline_batches.front().upserts.size(), 1u);
}
