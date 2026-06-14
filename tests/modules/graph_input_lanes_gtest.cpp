#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes_execution_edges_events.h>

#include <intravenous/dsl.h>
#include <intravenous/graph/build_types.h>
#include <intravenous/linker_event.h>
#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/task_runner_events.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
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
    std::vector<iv::GraphInputLanesDspTaskDependenciesChanged> dsp_dependencies {};
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
    iv::GraphInputLanesDspTaskDependenciesChangedEvent,
    iv_runtime_graph_input_lanes_dsp_task_dependencies_changed_event,
    +[](iv::GraphInputLanesDspTaskDependenciesChanged const &changed) {
        if (g_graph_input_lanes_witness != nullptr) {
            g_graph_input_lanes_witness->dsp_dependencies.push_back(changed);
        }
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

    ASSERT_EQ(witness.timeline_batches.size(), 1u);
    EXPECT_GE(witness.timeline_batches.front().upserts.size(), 1u);
    EXPECT_TRUE(witness.timeline_batches.front().removals.empty());
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
    auto const created_count = witness.timeline_batches.front().upserts.size();
    witness.timeline_batches.clear();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .deleted_instance_ids = {"instance:1"},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 2});

    ASSERT_EQ(witness.timeline_batches.size(), 1u);
    EXPECT_EQ(witness.timeline_batches.front().removals.size(), created_count);
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

    ASSERT_EQ(witness.timeline_batches.size(), 1u);
    EXPECT_GE(witness.timeline_batches.front().upserts.size(), 1u);
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

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });

    ASSERT_EQ(witness.dsp_dependencies.size(), 1u);
    ASSERT_EQ(witness.dsp_dependencies.front().created_or_updated.size(), 1u);
    EXPECT_TRUE(witness.dsp_dependencies.front().deleted_instance_ids.empty());

    auto const &dependencies = witness.dsp_dependencies.front().created_or_updated.front();
    EXPECT_EQ(dependencies.instance_id, "instance:1");
    EXPECT_TRUE(dependencies.prerequisite_lanes.empty());
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
    witness.dsp_dependencies.clear();

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

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt}},
    });

    ASSERT_EQ(witness.dsp_dependencies.size(), 1u);
    ASSERT_EQ(witness.dsp_dependencies.front().created_or_updated.size(), 1u);
    auto const &dependencies = witness.dsp_dependencies.front().created_or_updated.front();
    EXPECT_EQ(dependencies.instance_id, "instance:1");
    ASSERT_EQ(dependencies.prerequisite_lanes.size(), 1u);
    EXPECT_TRUE(static_cast<bool>(dependencies.prerequisite_lanes.front()));
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
    witness.dsp_dependencies.clear();

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

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt}},
    });

    ASSERT_EQ(witness.dsp_dependencies.size(), 1u);
    ASSERT_EQ(witness.dsp_dependencies.front().created_or_updated.size(), 1u);
    auto const &dependencies = witness.dsp_dependencies.front().created_or_updated.front();
    EXPECT_EQ(dependencies.instance_id, "instance:1");
    ASSERT_EQ(dependencies.prerequisite_lanes.size(), 1u);
    EXPECT_TRUE(static_cast<bool>(dependencies.prerequisite_lanes.front()));
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
    witness.dsp_dependencies.clear();

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

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt}},
    });

    ASSERT_EQ(witness.dsp_dependencies.size(), 1u);
    auto const &dependencies = witness.dsp_dependencies.front().created_or_updated.front();
    EXPECT_TRUE(dependencies.prerequisite_lanes.empty());
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

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 0});

    ASSERT_EQ(witness.dsp_dependencies.size(), 1u);
    auto const &dependencies = witness.dsp_dependencies.front().created_or_updated.front();
    ASSERT_EQ(dependencies.prerequisite_lanes.size(), 1u);
    EXPECT_TRUE(static_cast<bool>(dependencies.prerequisite_lanes.front()));
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

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_pass_finished(iv::TaskRunnerPassFinished{.graph_revision = 0});

    ASSERT_EQ(witness.dsp_dependencies.size(), 1u);
    auto const &dependencies = witness.dsp_dependencies.front().created_or_updated.front();
    EXPECT_TRUE(dependencies.prerequisite_lanes.empty());
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
    witness.dsp_dependencies.clear();

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

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt}},
    });

    ASSERT_EQ(witness.dsp_dependencies.size(), 1u);
    auto const &dependencies = witness.dsp_dependencies.front().created_or_updated.front();
    ASSERT_EQ(dependencies.prerequisite_lanes.size(), 1u);
    EXPECT_TRUE(static_cast<bool>(dependencies.prerequisite_lanes.front()));
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
    witness.dsp_dependencies.clear();

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

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt}},
    });

    ASSERT_EQ(witness.dsp_dependencies.size(), 1u);
    auto const &dependencies = witness.dsp_dependencies.front().created_or_updated.front();
    ASSERT_EQ(dependencies.prerequisite_lanes.size(), 1u);
    EXPECT_TRUE(static_cast<bool>(dependencies.prerequisite_lanes.front()));
}
