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
#include <array>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace {
iv::BorrowedSampleBlock mono_block(std::span<iv::Sample const> samples)
{
    return iv::BorrowedSampleBlock {
        .samples = samples,
        .channel_layout =
            iv::ChannelLayout {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
        .frame_count = samples.size(),
    };
}

iv::BorrowedSampleBlock stereo_interleaved_block(std::span<iv::Sample const> samples, size_t frame_count)
{
    return iv::BorrowedSampleBlock {
        .samples = samples,
        .channel_layout =
            iv::ChannelLayout {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            },
        .frame_count = frame_count,
    };
}

std::vector<iv::Sample> sample_values(iv::OwnedSampleBlock const &block)
{
    return std::vector<iv::Sample>(block.samples.begin(), block.samples.end());
}

std::string runtime_node_id(std::string_view instance_id, std::string_view logical_node_id)
{
    return std::string(instance_id) + "\x1flogical:" + std::string(logical_node_id);
}

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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});
    size_t created_count = 0;
    for (auto const &batch : witness.timeline_batches) {
        created_count += batch.upserts.size();
    }
    witness.timeline_batches.clear();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .deleted_instance_ids = {"instance:1"},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 2});

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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

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
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
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
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = std::nullopt,
        .input_ordinal = 0,
        .value = iv::Sample{0.5f},
    });

    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});
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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    witness.timeline_batches.clear();
    witness.rebuild_requests.clear();

    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = std::nullopt,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

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
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    witness.timeline_batches.clear();
    witness.rebuild_requests.clear();

    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    witness.timeline_batches.clear();
    witness.rebuild_requests.clear();

    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::default_,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 2});

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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1").value_or(std::vector<iv::LaneId>{}).size(),
        1u);
}

TEST_F(GraphInputLanesTest, ConnectedEventInputsDefaultToDisconnectedWithoutTimelineDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder builder;

    auto trigger_source = builder.node<iv::EventConcatenation>(0, iv::EventTypeId::trigger).event_port(0);
    auto node = iv::_annotate_node_source_info(
        builder.node<TestEventSink>().connect_event_input(0, trigger_source).node_ref(),
        "node-1");
    (void)node;

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    }, &ack);
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});
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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});
    witness.timeline_batches.clear();
    witness.rebuild_requests.clear();

    lanes.set_event_input_state(iv::ProjectSetEventInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectEventInputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    lanes.set_event_input_state(iv::ProjectSetEventInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectEventInputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    witness.timeline_batches.clear();
    witness.rebuild_requests.clear();

    lanes.set_event_input_state(iv::ProjectSetEventInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectEventInputState::default_,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 2});

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

bool batch_has_public_lane(
    iv::TimelineLaneBatchUpdate const &batch,
    std::string_view direction_unit,
    std::string_view kind_unit)
{
    for (auto const &upsert : batch.upserts) {
        if (upsert.metadata.has_unit("dsp_graph.public")
            && upsert.metadata.has_unit(direction_unit)
            && upsert.metadata.has_unit(kind_unit)) {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_F(GraphInputLanesTest, LogicalOutputsDoNotAutoCreateTimelineLanesWithoutExplicitState)
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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    bool saw_output_lane = false;
    for (auto const &batch : witness.timeline_batches) {
        saw_output_lane = saw_output_lane || batch_has_output_lane(batch, "dsp_graph.logical");
    }
    EXPECT_FALSE(saw_output_lane);
}

TEST_F(GraphInputLanesTest, PublicSampleOutputCreatesAutomaticTimelineLaneAndExecutionRoot)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder builder;
    builder.outputs(0.25f);

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    bool saw_public_output = false;
    for (auto const &batch : witness.timeline_batches) {
        saw_public_output = saw_public_output
            || batch_has_public_lane(batch, "dsp_graph.public_output", "dsp_graph.sample");
    }
    EXPECT_TRUE(saw_public_output);
    EXPECT_NO_THROW((void)builder.build_execution_root_node());
}

TEST_F(GraphInputLanesTest, UpdatingBuilderDoesNotRemoveExistingPublicSampleOutputLane)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder initial_builder;
    initial_builder.outputs(0.25f);

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &initial_builder}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    std::optional<iv::LaneId> public_output_lane;
    for (auto const &batch : witness.timeline_batches) {
        for (auto const &upsert : batch.upserts) {
            if (upsert.metadata.has_unit("dsp_graph.public_output")
                && upsert.metadata.has_unit("dsp_graph.sample")) {
                public_output_lane = upsert.lane;
                break;
            }
        }
        if (public_output_lane.has_value()) {
            break;
        }
    }
    ASSERT_TRUE(public_output_lane.has_value());

    witness.timeline_batches.clear();

    iv::GraphBuilder rebuilt_builder;
    rebuilt_builder.outputs(0.25f);
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt_builder}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    for (auto const &batch : witness.timeline_batches) {
        EXPECT_EQ(
            std::find(batch.removals.begin(), batch.removals.end(), *public_output_lane),
            batch.removals.end());
    }
}

TEST_F(GraphInputLanesTest, UpdatingPublicSampleOutputFromMonoToStereoPreservesItsLane)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder mono_builder;
    mono_builder.outputs(iv::channels::mono = 0.25f);

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &mono_builder}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    std::optional<iv::LaneId> public_output_lane;
    for (auto const &batch : witness.timeline_batches) {
        for (auto const &upsert : batch.upserts) {
            if (upsert.metadata.has_unit("dsp_graph.public_output")
                && upsert.metadata.has_unit("dsp_graph.sample")) {
                public_output_lane = upsert.lane;
                break;
            }
        }
    }
    ASSERT_TRUE(public_output_lane.has_value());

    witness.timeline_batches.clear();

    iv::GraphBuilder stereo_builder;
    stereo_builder.outputs(
        iv::channels::stereo_left = 0.25f,
        iv::channels::stereo_right = 0.25f);
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &stereo_builder}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    bool saw_preserved_stereo_lane = false;
    for (auto const &batch : witness.timeline_batches) {
        EXPECT_EQ(
            std::find(batch.removals.begin(), batch.removals.end(), *public_output_lane),
            batch.removals.end());
        for (auto const &upsert : batch.upserts) {
            if (upsert.lane == *public_output_lane
                && upsert.sample_channel_type == iv::ChannelTypeId::stereo) {
                saw_preserved_stereo_lane = true;
            }
        }
    }
    EXPECT_TRUE(saw_preserved_stereo_lane);
}

TEST_F(GraphInputLanesTest, PublicSampleInputCreatesAutomaticTimelineLaneAndDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder builder;
    auto input = builder.input("frequency", 0.5f);
    auto node = iv::_annotate_node_source_info(
        builder.node<iv::Sum<1>>()(input).node_ref(),
        "node-1");
    (void)node;
    builder.outputs({});

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    }, &ack);
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    bool saw_public_input = false;
    for (auto const &batch : witness.timeline_batches) {
        saw_public_input = saw_public_input
            || batch_has_public_lane(batch, "dsp_graph.public_input", "dsp_graph.sample");
    }
    EXPECT_TRUE(saw_public_input);
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1").value_or(std::vector<iv::LaneId>{}).size(),
        1u);
    EXPECT_NO_THROW((void)builder.build_execution_root_node());
}

TEST_F(GraphInputLanesTest, PublicSampleInputSupportsUnnamedOptionalBounds)
{
    iv::GraphBuilder builder;
    auto input = builder.input(0.5f, -2.0f, 4.0f);
    builder.outputs(iv::channels::mono = input);

    auto const families = builder.public_sample_input_families();
    ASSERT_EQ(families.families.size(), 1u);
    auto const &config = families.families.front().input_config;
    EXPECT_TRUE(config.name.empty());
    EXPECT_FLOAT_EQ(static_cast<float>(config.default_value), 0.5f);
    ASSERT_TRUE(config.min.has_value());
    EXPECT_FLOAT_EQ(static_cast<float>(*config.min), -2.0f);
    ASSERT_TRUE(config.max.has_value());
    EXPECT_FLOAT_EQ(static_cast<float>(*config.max), 4.0f);
}

TEST_F(GraphInputLanesTest, PublicEventPortsCreateAutomaticLanesAndDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    iv::GraphBuilder builder;
    auto trigger = builder.event_input("trigger", iv::EventTypeId::trigger);
    auto node = iv::_annotate_node_source_info(
        builder.node<TestEventSink>().connect_event_input(0, trigger).node_ref(),
        "node-1");
    (void)node;
    builder.event_outputs(trigger);
    builder.outputs({});

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &builder}},
    }, &ack);
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    bool saw_public_event_input = false;
    bool saw_public_event_output = false;
    for (auto const &batch : witness.timeline_batches) {
        saw_public_event_input = saw_public_event_input
            || batch_has_public_lane(batch, "dsp_graph.public_input", "dsp_graph.event");
        saw_public_event_output = saw_public_event_output
            || batch_has_public_lane(batch, "dsp_graph.public_output", "dsp_graph.event");
    }
    EXPECT_TRUE(saw_public_event_input);
    EXPECT_TRUE(saw_public_event_output);
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1").value_or(std::vector<iv::LaneId>{}).size(),
        1u);
    EXPECT_NO_THROW((void)builder.build_execution_root_node());
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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});
    witness.timeline_batches.clear();
    lanes.set_sample_output_state(iv::ProjectSetSampleOutputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = std::nullopt,
        .output_ordinal = 0,
        .state = iv::ProjectSampleOutputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});
    witness.rebuild_requests.clear();

    lanes.set_sample_output_state(iv::ProjectSetSampleOutputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = std::nullopt,
        .output_ordinal = 0,
        .state = iv::ProjectSampleOutputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});
    witness.timeline_batches.clear();
    lanes.set_sample_output_state(iv::ProjectSetSampleOutputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = 0u,
        .output_ordinal = 0,
        .state = iv::ProjectSampleOutputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    lanes.set_sample_output_state(iv::ProjectSetSampleOutputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = std::nullopt,
        .output_ordinal = 0,
        .state = iv::ProjectSampleOutputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});
    iv::TimelineLaneUpsert const *created = nullptr;
    ASSERT_TRUE(batch_has_output_lane(
        witness.timeline_batches.back(),
        "dsp_graph.logical",
        &created));
    auto const created_lane = created->lane;
    witness.timeline_batches.clear();

    lanes.set_sample_output_state(iv::ProjectSetSampleOutputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = std::nullopt,
        .output_ordinal = 0,
        .state = iv::ProjectSampleOutputState::disconnected,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 2});

    ASSERT_EQ(witness.timeline_batches.size(), 1u);
    auto const &removals = witness.timeline_batches.front().removals;
    EXPECT_NE(std::find(removals.begin(), removals.end(), created_lane), removals.end());
}

TEST_F(GraphInputLanesTest, SampleOutputLanesRemainMonoAcrossRebuild)
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
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});
    witness.timeline_batches.clear();

    lanes.set_sample_output_state(iv::ProjectSetSampleOutputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = std::nullopt,
        .output_ordinal = 0,
        .state = iv::ProjectSampleOutputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    iv::TimelineLaneUpsert const *created = nullptr;
    for (auto const &batch : witness.timeline_batches) {
        if (batch_has_output_lane(batch, "dsp_graph.logical", &created)) {
            break;
        }
    }
    ASSERT_NE(created, nullptr);
    ASSERT_TRUE(created->sample_channel_type.has_value());
    EXPECT_EQ(*created->sample_channel_type, iv::ChannelTypeId::mono);

    witness.timeline_batches.clear();
    iv::GraphBuilder rebuilt;
    auto rebuilt_node = iv::_annotate_node_source_info(
        rebuilt.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)rebuilt_node;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance, .builder = &rebuilt}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 2});

    iv::TimelineLaneUpsert const *rebuilt_upsert = nullptr;
    for (auto const &batch : witness.timeline_batches) {
        if (batch_has_output_lane(batch, "dsp_graph.logical", &rebuilt_upsert)) {
            break;
        }
    }
    ASSERT_NE(rebuilt_upsert, nullptr);
    ASSERT_TRUE(rebuilt_upsert->sample_channel_type.has_value());
    EXPECT_EQ(*rebuilt_upsert->sample_channel_type, iv::ChannelTypeId::mono);
}

TEST_F(GraphInputLanesTest, SampleOutputBlockRoundTripsThroughGraphInputLanesStorage)
{
    iv::GraphInputLanes lanes;
    auto const lane = iv::LaneId{42};
    auto const block = std::array<iv::Sample, 3>{0.25f, -0.5f, 1.0f};

    lanes.handle_sample_block_published(lane, mono_block(std::span<iv::Sample const>(block)));

    auto const stored = lanes.handle_sample_block_requested(lane);
    EXPECT_EQ(stored.channel_layout.channel_type, iv::ChannelTypeId::mono);
    EXPECT_EQ(stored.channel_layout.sample_layout, iv::SampleStreamLayout::planar);
    EXPECT_EQ(stored.frame_count, block.size());
    EXPECT_EQ(sample_values(stored), (std::vector<iv::Sample>{0.25f, -0.5f, 1.0f}));
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

    lanes.handle_sample_block_published(lane, mono_block(std::span<iv::Sample const>(first)));
    lanes.handle_sample_block_published(lane, mono_block(std::span<iv::Sample const>(second)));

    auto const stored = lanes.handle_sample_block_requested(lane);
    EXPECT_EQ(stored.channel_layout.channel_type, iv::ChannelTypeId::mono);
    EXPECT_EQ(stored.channel_layout.sample_layout, iv::SampleStreamLayout::planar);
    EXPECT_EQ(stored.frame_count, second.size());
    EXPECT_EQ(sample_values(stored), (std::vector<iv::Sample>{-3.0f}));
}

TEST_F(GraphInputLanesTest, StereoInterleavedSampleOutputBlockRoundTripsThroughGraphInputLanesStorage)
{
    iv::GraphInputLanes lanes;
    auto const lane = iv::LaneId{45};
    auto const block = std::array<iv::Sample, 6>{0.25f, -0.25f, 0.5f, -0.5f, 1.0f, -1.0f};

    lanes.handle_sample_block_published(
        lane,
        stereo_interleaved_block(std::span<iv::Sample const>(block), 3));

    auto const stored = lanes.handle_sample_block_requested(lane);
    EXPECT_EQ(stored.channel_layout.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(stored.channel_layout.sample_layout, iv::SampleStreamLayout::interleaved);
    EXPECT_EQ(stored.frame_count, 3u);
    EXPECT_EQ(sample_values(stored), (std::vector<iv::Sample>{0.25f, -0.25f, 0.5f, -0.5f, 1.0f, -1.0f}));
}
