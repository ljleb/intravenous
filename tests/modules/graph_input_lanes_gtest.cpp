#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/build_types.h>
#include <intravenous/linker_event.h>
#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/task_ids.h>
#include <intravenous/runtime/task_runner_events.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace {
struct TestEventSink {
    auto event_inputs() const
    {
        return std::array<iv::EventInputConfig, 1>{{
            {.name = "trigger", .type = iv::EventTypeId::trigger}
        }};
    }

    auto outputs() const
    {
        return std::array<iv::OutputConfig, 1>{};
    }

    void tick(auto const &ctx) const
    {
        ctx.outputs[0].push(iv::Sample{0.0f});
    }
};

struct GraphInputLanesWitness {
    std::vector<iv::TimelineLaneBatchUpdate> timeline_batches {};
    std::vector<std::vector<std::string>> rebuild_requests {};
};

GraphInputLanesWitness *g_graph_input_lanes_witness = nullptr;

iv::IvModuleInstance make_instance_with_ports()
{
    iv::IvModuleInstance instance {};
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
        .ordinal = 0,
    });

    instance.introspection.logical_nodes.push_back(std::move(node));
    return instance;
}

iv::IvModuleInstance make_instance_with_member_ports()
{
    auto instance = make_instance_with_ports();
    iv::IntrospectionLogicalNode::Member member {};
    member.ordinal = 0;
    member.backing_node_id = "node-1";
    member.kind = "TestNode";
    member.sample_inputs.push_back(iv::IntrospectionPortInfo {
        .name = "frequency",
        .type = "sample",
        .connectivity = iv::LogicalPortConnectivity::disconnected,
        .ordinal = 0,
        .default_value = 440.0f,
    });
    member.event_inputs.push_back(iv::IntrospectionPortInfo {
        .name = "trigger",
        .type = "event",
        .connectivity = iv::LogicalPortConnectivity::disconnected,
        .ordinal = 0,
    });
    instance.introspection.logical_nodes.front().members.push_back(std::move(member));
    return instance;
}

iv::IvModuleInstance make_instance_with_output_ports()
{
    iv::IvModuleInstance instance {};
    instance.instance_id = "instance:1";
    instance.definition_id = "definition:1";
    instance.module_root = std::filesystem::path("/tmp/module");
    instance.module_id = "iv.test.module";

    iv::IntrospectionLogicalNode node {};
    node.id = "node-1";
    node.kind = "TestNode";
    node.sample_outputs.push_back(iv::IntrospectionPortInfo {
        .name = "out",
        .type = "sample",
        .connectivity = iv::LogicalPortConnectivity::disconnected,
        .ordinal = 0,
    });
    instance.introspection.logical_nodes.push_back(std::move(node));
    return instance;
}

iv::IvModuleInstance make_instance_with_member_output_ports()
{
    auto instance = make_instance_with_output_ports();
    iv::IntrospectionLogicalNode::Member member {};
    member.ordinal = 0;
    member.backing_node_id = "node-1";
    member.kind = "TestNode";
    member.sample_outputs.push_back(iv::IntrospectionPortInfo {
        .name = "out",
        .type = "sample",
        .connectivity = iv::LogicalPortConnectivity::disconnected,
        .ordinal = 0,
    });
    instance.introspection.logical_nodes.front().members.push_back(std::move(member));
    return instance;
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

IV_SUBSCRIBE_LINKER_EVENT(
    iv::GraphInputLanesRebuildRequestedEvent,
    iv_runtime_graph_input_lanes_rebuild_requested_event,
    +[](iv::GraphInputLanesRebuildRequested const &request) {
        if (g_graph_input_lanes_witness != nullptr) {
            g_graph_input_lanes_witness->rebuild_requests.push_back(request.instance_ids);
        }
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
    auto instance = make_instance_with_ports();
    iv::GraphBuilder builder;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});

    ASSERT_GE(witness.timeline_batches.size(), 1u);
    size_t upsert_count = 0;
    size_t removal_count = 0;
    for (auto const &batch : witness.timeline_batches) {
        upsert_count += batch.upserts.size();
        removal_count += batch.removals.size();
    }
    EXPECT_GE(upsert_count, 1u);
    EXPECT_EQ(removal_count, 0u);
}

TEST_F(GraphInputLanesTest, DeletingLastInstancePublishesTimelineRemovals)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder builder;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});
    size_t created_count = 0;
    for (auto const &batch : witness.timeline_batches) {
        created_count += batch.upserts.size();
    }
    witness.timeline_batches.clear();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .deleted_instance_ids = {"instance:1"},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 2});

    ASSERT_GE(witness.timeline_batches.size(), 1u);
    size_t removal_count = 0;
    for (auto const &batch : witness.timeline_batches) {
        removal_count += batch.removals.size();
    }
    EXPECT_EQ(removal_count, created_count);
}

TEST_F(GraphInputLanesTest, UpdatedInstancePublishesTimelineBatch)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder builder;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});

    ASSERT_GE(witness.timeline_batches.size(), 1u);
    size_t upsert_count = 0;
    for (auto const &batch : witness.timeline_batches) {
        upsert_count += batch.upserts.size();
    }
    EXPECT_GE(upsert_count, 1u);
}

TEST_F(GraphInputLanesTest, LogicalSampleInputOverrideDoesNotPublishTimelineBatchByDefault)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder builder;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    witness.timeline_batches.clear();

    lanes.set_sample_input_value(iv::ProjectSetSampleInputValueRequest{
        .node_id = "node-1",
        .member_ordinal = std::nullopt,
        .input_ordinal = 0,
        .value = iv::Sample{0.5f},
    });

    EXPECT_TRUE(witness.timeline_batches.empty());
}

TEST_F(GraphInputLanesTest, SampleValueChangesDoNotQueueRebuilds)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder builder;

    auto node = iv::_annotate_node_source_info(
        builder.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    witness.rebuild_requests.clear();

    lanes.set_sample_input_value(iv::ProjectSetSampleInputValueRequest{
        .node_id = "node-1",
        .member_ordinal = std::nullopt,
        .input_ordinal = 0,
        .value = iv::Sample{0.5f},
    });

    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});
    EXPECT_TRUE(witness.rebuild_requests.empty());
}

TEST_F(GraphInputLanesTest, VacantSampleInputsDefaultToLogicalFollowWithoutTimelineDependencies)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder builder;

    auto node = iv::_annotate_node_source_info(
        builder.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)node;

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    }, &ack);
    EXPECT_EQ(
        ack.prerequisite_lanes_for("instance:1").value_or(std::vector<iv::LaneId>{}).size(),
        0u);
}

TEST_F(GraphInputLanesTest, LogicalSampleInputTimelineStatePublishesTimelineDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder builder;

    auto node = iv::_annotate_node_source_info(
        builder.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 0});

    witness.timeline_batches.clear();
    witness.rebuild_requests.clear();

    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = "node-1",
        .member_ordinal = std::nullopt,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});

    ASSERT_EQ(witness.timeline_batches.size(), 1u);

    iv::GraphBuilder rebuilt;
    auto rebuilt_node = iv::_annotate_node_source_info(
        rebuilt.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)rebuilt_node;

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt}},
    }, &ack);
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1").value_or(std::vector<iv::LaneId>{}).size(),
        1u);
}

TEST_F(GraphInputLanesTest, ConcreteSampleInputOverrideDoesNotPublishTimelineBatchByDefault)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();
    iv::GraphBuilder builder;

    auto node = iv::_annotate_node_source_info(
        builder.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    witness.timeline_batches.clear();

    lanes.set_sample_input_value(iv::ProjectSetSampleInputValueRequest{
        .node_id = "node-1",
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .value = iv::Sample{0.25f},
    });

    EXPECT_TRUE(witness.timeline_batches.empty());
}

TEST_F(GraphInputLanesTest, ConcreteSampleInputTimelineStatePublishesTimelineDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();
    iv::GraphBuilder builder;

    auto node = iv::_annotate_node_source_info(
        builder.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 0});

    witness.timeline_batches.clear();
    witness.rebuild_requests.clear();

    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = "node-1",
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});

    ASSERT_EQ(witness.timeline_batches.size(), 1u);

    iv::GraphBuilder rebuilt;
    auto rebuilt_node = iv::_annotate_node_source_info(
        rebuilt.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)rebuilt_node;

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt}},
    }, &ack);
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1").value_or(std::vector<iv::LaneId>{}).size(),
        1u);
}

TEST_F(GraphInputLanesTest, ConcreteSampleInputDefaultClearsExplicitTimelineState)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();
    iv::GraphBuilder builder;

    auto node = iv::_annotate_node_source_info(
        builder.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 0});

    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = "node-1",
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});

    witness.timeline_batches.clear();
    witness.rebuild_requests.clear();

    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = "node-1",
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::default_,
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 2});

    ASSERT_EQ(witness.timeline_batches.size(), 1u);

    iv::GraphBuilder rebuilt;
    auto rebuilt_node = iv::_annotate_node_source_info(
        rebuilt.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)rebuilt_node;

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt}},
    }, &ack);
    EXPECT_EQ(
        ack.prerequisite_lanes_for("instance:1").value_or(std::vector<iv::LaneId>{}).size(),
        0u);
}

TEST_F(GraphInputLanesTest, VacantEventInputsDefaultToLogicalFollowWithTimelineDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder builder;

    auto node = iv::_annotate_node_source_info(
        builder.node<TestEventSink>().node_ref(),
        "node-1");
    (void)node;

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    }, &ack);
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 0});
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1").value_or(std::vector<iv::LaneId>{}).size(),
        1u);
}

TEST_F(GraphInputLanesTest, ConnectedEventInputsDefaultToDisconnectedWithoutTimelineDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder builder;

    auto trigger_source = builder.event_input("src", iv::EventTypeId::trigger);
    auto node = iv::_annotate_node_source_info(
        builder.node<TestEventSink>().connect_event_input(0, trigger_source).node_ref(),
        "node-1");
    (void)node;

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    }, &ack);
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 0});
    EXPECT_EQ(
        ack.prerequisite_lanes_for("instance:1").value_or(std::vector<iv::LaneId>{}).size(),
        0u);
}

TEST_F(GraphInputLanesTest, ConcreteEventInputTimelineStatePublishesTimelineDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();
    iv::GraphBuilder builder;

    auto node = iv::_annotate_node_source_info(
        builder.node<TestEventSink>().node_ref(),
        "node-1");
    (void)node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 0});
    witness.timeline_batches.clear();
    witness.rebuild_requests.clear();

    lanes.set_event_input_state(iv::ProjectSetEventInputStateRequest{
        .node_id = "node-1",
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectEventInputState::timeline_lane,
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});

    EXPECT_FALSE(witness.timeline_batches.empty());

    iv::GraphBuilder rebuilt;
    auto rebuilt_node = iv::_annotate_node_source_info(
        rebuilt.node<TestEventSink>().node_ref(),
        "node-1");
    (void)rebuilt_node;

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt}},
    }, &ack);
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1").value_or(std::vector<iv::LaneId>{}).size(),
        1u);
}

TEST_F(GraphInputLanesTest, ConcreteEventInputDefaultClearsExplicitTimelineState)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();
    iv::GraphBuilder builder;

    auto node = iv::_annotate_node_source_info(
        builder.node<TestEventSink>().node_ref(),
        "node-1");
    (void)node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 0});

    lanes.set_event_input_state(iv::ProjectSetEventInputStateRequest{
        .node_id = "node-1",
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectEventInputState::timeline_lane,
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});

    witness.timeline_batches.clear();
    witness.rebuild_requests.clear();

    lanes.set_event_input_state(iv::ProjectSetEventInputStateRequest{
        .node_id = "node-1",
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectEventInputState::default_,
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 2});

    EXPECT_FALSE(witness.timeline_batches.empty());

    iv::GraphBuilder rebuilt;
    auto rebuilt_node = iv::_annotate_node_source_info(
        rebuilt.node<TestEventSink>().node_ref(),
        "node-1");
    (void)rebuilt_node;

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt}},
    }, &ack);
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1").value_or(std::vector<iv::LaneId>{}).size(),
        1u);
}

namespace {
bool batch_has_output_lane(
    iv::TimelineLaneBatchUpdate const &batch,
    std::string_view extra_unit,
    iv::TimelineLaneUpsert const **out = nullptr)
{
    for (auto const &upsert : batch.upserts) {
        if (upsert.metadata.has_unit("dsp_graph.output")
            && upsert.metadata.has_unit(extra_unit)) {
            if (out != nullptr) {
                *out = &upsert;
            }
            return true;
        }
    }
    return false;
}
} // namespace

TEST_F(GraphInputLanesTest, DisconnectedOutputsCreateNoOutputLanes)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_output_ports();
    iv::GraphBuilder builder;
    auto node = iv::_annotate_node_source_info(
        builder.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});

    for (auto const &batch : witness.timeline_batches) {
        EXPECT_FALSE(batch_has_output_lane(batch, "dsp_graph.sample"));
    }
}

TEST_F(GraphInputLanesTest, LogicalSampleOutputTimelineStateCreatesAggregationLaneWithDspDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_output_ports();
    iv::GraphBuilder builder;
    auto node = iv::_annotate_node_source_info(
        builder.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 0});
    witness.timeline_batches.clear();
    lanes.set_sample_output_state(iv::ProjectSetSampleOutputStateRequest{
        .node_id = "node-1",
        .member_ordinal = std::nullopt,
        .output_ordinal = 0,
        .state = iv::ProjectSampleOutputState::timeline_lane,
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});

    ASSERT_EQ(witness.timeline_batches.size(), 1u);
    iv::TimelineLaneUpsert const *upsert = nullptr;
    ASSERT_TRUE(batch_has_output_lane(
        witness.timeline_batches.front(),
        "dsp_graph.logical",
        &upsert));
    ASSERT_NE(upsert, nullptr);

    iv::GraphBuilder rebuilt;
    auto rebuilt_node = iv::_annotate_node_source_info(
        rebuilt.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)rebuilt_node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt}},
    });

    ASSERT_EQ(upsert->external_task_dependencies.size(), 1u);
    EXPECT_EQ(
        upsert->external_task_dependencies.front(),
        iv::iv_module_instance_dsp_task_id("instance:1"));
}

TEST_F(GraphInputLanesTest, SettingSampleOutputStateMarksInstanceRebuild)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_output_ports();
    iv::GraphBuilder builder;
    auto node = iv::_annotate_node_source_info(
        builder.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 0});
    witness.rebuild_requests.clear();

    lanes.set_sample_output_state(iv::ProjectSetSampleOutputStateRequest{
        .node_id = "node-1",
        .member_ordinal = std::nullopt,
        .output_ordinal = 0,
        .state = iv::ProjectSampleOutputState::timeline_lane,
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});

    ASSERT_EQ(witness.rebuild_requests.size(), 1u);
    ASSERT_EQ(witness.rebuild_requests.front().size(), 1u);
    EXPECT_EQ(witness.rebuild_requests.front().front(), "instance:1");
}

TEST_F(GraphInputLanesTest, ConcreteSampleOutputTimelineStateCreatesDedicatedLane)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_output_ports();
    iv::GraphBuilder builder;
    auto node = iv::_annotate_node_source_info(
        builder.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 0});
    witness.timeline_batches.clear();
    lanes.set_sample_output_state(iv::ProjectSetSampleOutputStateRequest{
        .node_id = "node-1",
        .member_ordinal = 0u,
        .output_ordinal = 0,
        .state = iv::ProjectSampleOutputState::timeline_lane,
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});

    ASSERT_EQ(witness.timeline_batches.size(), 1u);
    iv::TimelineLaneUpsert const *upsert = nullptr;
    ASSERT_TRUE(batch_has_output_lane(
        witness.timeline_batches.front(),
        "dsp_graph.concrete",
        &upsert));
    ASSERT_NE(upsert, nullptr);

    iv::GraphBuilder rebuilt;
    auto rebuilt_node = iv::_annotate_node_source_info(
        rebuilt.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)rebuilt_node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt}},
    });

    ASSERT_EQ(upsert->external_task_dependencies.size(), 1u);
    EXPECT_EQ(
        upsert->external_task_dependencies.front(),
        iv::iv_module_instance_dsp_task_id("instance:1"));
}

TEST_F(GraphInputLanesTest, TogglingSampleOutputBackToDisconnectedRemovesLane)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_output_ports();
    iv::GraphBuilder builder;
    auto node = iv::_annotate_node_source_info(
        builder.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 0});

    lanes.set_sample_output_state(iv::ProjectSetSampleOutputStateRequest{
        .node_id = "node-1",
        .member_ordinal = std::nullopt,
        .output_ordinal = 0,
        .state = iv::ProjectSampleOutputState::timeline_lane,
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 1});
    iv::TimelineLaneUpsert const *created = nullptr;
    ASSERT_TRUE(batch_has_output_lane(
        witness.timeline_batches.back(),
        "dsp_graph.logical",
        &created));
    auto const created_lane = created->lane;
    witness.timeline_batches.clear();

    lanes.set_sample_output_state(iv::ProjectSetSampleOutputStateRequest{
        .node_id = "node-1",
        .member_ordinal = std::nullopt,
        .output_ordinal = 0,
        .state = iv::ProjectSampleOutputState::disconnected,
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 2});

    ASSERT_EQ(witness.timeline_batches.size(), 1u);
    auto const &removals = witness.timeline_batches.front().removals;
    EXPECT_NE(std::find(removals.begin(), removals.end(), created_lane), removals.end());
}

TEST_F(GraphInputLanesTest, SampleOutputBlockRoundTripsThroughGraphInputLanesStorage)
{
    iv::GraphInputLanes lanes;
    auto const lane = iv::LaneId{42};
    auto const block = std::array<iv::Sample, 3>{0.25f, -0.5f, 1.0f};

    lanes.handle_sample_block_published(lane, std::span<iv::Sample const>(block));

    EXPECT_EQ(
        lanes.handle_sample_block_requested(lane),
        (std::vector<iv::Sample>{0.25f, -0.5f, 1.0f}));
}

TEST_F(GraphInputLanesTest, EventOutputBlockRoundTripsThroughGraphInputLanesStorage)
{
    iv::GraphInputLanes lanes;
    auto const lane = iv::LaneId{43};
    std::vector<iv::TimedEvent> events{
        iv::TimedEvent{.time = 1, .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = 4, .value = iv::BoundaryEvent{.is_begin = true}},
    };

    lanes.handle_event_block_published(lane, std::span<iv::TimedEvent const>(events));

    auto const stored = lanes.handle_event_block_requested(lane);
    ASSERT_EQ(stored.size(), events.size());
    EXPECT_EQ(stored[0].time, events[0].time);
    EXPECT_TRUE(std::holds_alternative<iv::TriggerEvent>(stored[0].value));
    EXPECT_EQ(stored[1].time, events[1].time);
    auto const *boundary = std::get_if<iv::BoundaryEvent>(&stored[1].value);
    ASSERT_NE(boundary, nullptr);
    EXPECT_TRUE(boundary->is_begin);
}

TEST_F(GraphInputLanesTest, UnknownOutputBlockRequestsReturnEmpty)
{
    iv::GraphInputLanes lanes;

    EXPECT_TRUE(lanes.handle_sample_block_requested(iv::LaneId{77}).empty());
    EXPECT_TRUE(lanes.handle_event_block_requested(iv::LaneId{78}).empty());
}

TEST_F(GraphInputLanesTest, RepublishedSampleOutputBlockReplacesPreviousBlock)
{
    iv::GraphInputLanes lanes;
    auto const lane = iv::LaneId{44};
    auto const first = std::array<iv::Sample, 2>{1.0f, 2.0f};
    auto const second = std::array<iv::Sample, 1>{-3.0f};

    lanes.handle_sample_block_published(lane, std::span<iv::Sample const>(first));
    lanes.handle_sample_block_published(lane, std::span<iv::Sample const>(second));

    EXPECT_EQ(
        lanes.handle_sample_block_requested(lane),
        (std::vector<iv::Sample>{-3.0f}));
}
