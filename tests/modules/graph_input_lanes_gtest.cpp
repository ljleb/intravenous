#include <intravenous/runtime/graph_input_lanes.h>
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

iv::BorrowedSampleBlock stereo_interleaved_block(
    std::span<iv::Sample const> samples,
    size_t frame_count)
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

std::vector<iv::Sample> sample_values(iv::BorrowedSampleBlock const &block)
{
    return std::vector<iv::Sample>(block.samples.begin(), block.samples.end());
}

std::string runtime_node_id(
    std::string_view instance_id,
    std::string_view virtual_node_id)
{
    return std::string(instance_id) + "\x1fvirtual:" + std::string(virtual_node_id);
}

std::string lane_output_name(iv::TypeErasedLaneNode const &node)
{
    return std::visit([](auto const &output) { return output.name; }, node.output());
}

iv::SourceInfo source_info(
    std::string declaration_identity,
    std::string file_path = {},
    uint32_t begin = 0,
    uint32_t end = 0)
{
    return iv::SourceInfo {
        .declaration_identity = std::move(declaration_identity),
        .span = {
            .file_path = std::move(file_path),
            .begin = begin,
            .end = end,
        },
    };
}

iv::IvModuleInstance make_instance_base()
{
    iv::IvModuleInstance instance {};
    instance.instance_id = "instance:1";
    instance.definition_id = "definition:1";
    instance.module_root = std::filesystem::path("/tmp/module");
    instance.module_id = "iv.test.module";
    return instance;
}

iv::IntrospectionPortInfo sample_input_port(
    std::string name = "frequency",
    iv::ChannelTypeId channel_type = iv::ChannelTypeId::mono)
{
    return iv::IntrospectionPortInfo {
        .name = std::move(name),
        .type = "sample",
        .connectivity = iv::VirtualPortConnectivity::disconnected,
        .ordinal = 0,
        .default_value = 440.0f,
        .sample_channel_type = channel_type,
    };
}

iv::IntrospectionPortInfo event_input_port()
{
    return iv::IntrospectionPortInfo {
        .name = "trigger",
        .type = "event",
        .connectivity = iv::VirtualPortConnectivity::disconnected,
        .ordinal = 0,
    };
}

iv::IvModuleInstance make_instance_with_ports()
{
    auto instance = make_instance_base();
    iv::IntrospectionVirtualNode node {};
    node.id = "node-1";
    node.kind = "TestNode";
    node.sample_inputs.push_back(sample_input_port());
    node.event_inputs.push_back(event_input_port());
    instance.introspection.virtual_nodes.push_back(std::move(node));
    return instance;
}

iv::IvModuleInstance make_instance_with_member_ports()
{
    auto instance = make_instance_with_ports();
    iv::IntrospectionVirtualNode::Member member {};
    member.ordinal = 0;
    member.backing_node_id = "node-1";
    member.kind = "TestNode";
    member.sample_inputs.push_back(sample_input_port());
    member.event_inputs.push_back(event_input_port());
    instance.introspection.virtual_nodes.front().members.push_back(std::move(member));
    return instance;
}

iv::IvModuleInstance make_tiled_stereo_input_instance()
{
    auto instance = make_instance_base();
    iv::IntrospectionVirtualNode node {};
    node.id = "tiled-node";
    node.kind = "Sum";
    node.sample_inputs.push_back(sample_input_port("input", iv::ChannelTypeId::stereo));

    iv::IntrospectionVirtualNode::Member member {};
    member.ordinal = 0;
    member.backing_node_id = "tiled-node.member.0";
    member.kind = "Sum";
    member.sample_inputs.push_back(sample_input_port("input", iv::ChannelTypeId::stereo));
    node.members.push_back(std::move(member));

    instance.introspection.virtual_nodes.push_back(std::move(node));
    return instance;
}

iv::IvModuleInstance make_instance_with_output_ports()
{
    auto instance = make_instance_base();
    iv::IntrospectionVirtualNode node {};
    node.id = "node-1";
    node.kind = "TestNode";
    node.sample_outputs.push_back(iv::IntrospectionPortInfo {
        .name = "out",
        .type = "sample",
        .connectivity = iv::VirtualPortConnectivity::disconnected,
        .ordinal = 0,
        .sample_channel_type = iv::ChannelTypeId::mono,
    });
    instance.introspection.virtual_nodes.push_back(std::move(node));
    return instance;
}

iv::IvModuleInstance make_instance_with_member_output_ports()
{
    auto instance = make_instance_with_output_ports();
    iv::IntrospectionVirtualNode::Member member {};
    member.ordinal = 0;
    member.backing_node_id = "node-1";
    member.kind = "TestNode";
    member.sample_outputs.push_back(iv::IntrospectionPortInfo {
        .name = "out",
        .type = "sample",
        .connectivity = iv::VirtualPortConnectivity::disconnected,
        .ordinal = 0,
        .sample_channel_type = iv::ChannelTypeId::mono,
    });
    instance.introspection.virtual_nodes.front().members.push_back(std::move(member));
    return instance;
}

iv::GraphBuilderPublicSamplePortFamily &add_public_sample_input(
    iv::IvModuleInstance &instance,
    size_t ordinal,
    std::string name,
    iv::Sample default_value,
    std::optional<iv::Sample> min = std::nullopt,
    std::optional<iv::Sample> max = std::nullopt,
    std::optional<iv::SourceInfo> source = std::nullopt,
    bool authored_connected = false)
{
    auto &family = instance.introspection.public_sample_inputs.emplace_back();
    family.family_ordinal = ordinal;
    family.family_name = name;
    family.input_config.name = std::move(name);
    family.input_config.default_value = default_value;
    if (min.has_value()) {
        family.input_config.min = *min;
    }
    if (max.has_value()) {
        family.input_config.max = *max;
    }
    family.channel_type = iv::ChannelTypeId::mono;
    family.channels.resize(1);
    family.channels.front().port_ordinals.push_back(ordinal);
    if (source.has_value()) {
        family.source_infos.push_back(std::move(*source));
    }
    family.authored_connected = authored_connected;
    return family;
}

iv::GraphBuilderPublicSamplePortFamily &set_public_sample_output(
    iv::IvModuleInstance &instance,
    std::string name,
    iv::ChannelTypeId channel_type)
{
    instance.introspection.public_sample_outputs.clear();
    auto &family = instance.introspection.public_sample_outputs.emplace_back();
    family.family_ordinal = 0;
    family.family_name = name;
    family.output_config.name = name;
    family.output_config.channel_layout = {
        .channel_type = channel_type,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    family.channel_type = channel_type;
    family.channels.resize(iv::channel_count(channel_type));
    for (size_t channel = 0; channel < family.channels.size(); ++channel) {
        family.channels[channel].port_ordinals.push_back(channel);
    }
    return family;
}

iv::GraphBuilderPublicEventInput &add_public_event_input(
    iv::IvModuleInstance &instance,
    size_t ordinal,
    std::string name,
    std::optional<iv::SourceInfo> source = std::nullopt,
    bool graph_connected = false)
{
    auto &input = instance.introspection.public_event_inputs.emplace_back();
    input.port_ordinal = ordinal;
    input.config = {
        .name = std::move(name),
        .type = iv::EventTypeId::trigger,
    };
    if (source.has_value()) {
        input.source_infos.push_back(std::move(*source));
    }
    input.graph_connected = graph_connected;
    return input;
}

void add_public_event_output(
    iv::IvModuleInstance &instance,
    size_t ordinal,
    std::string name)
{
    instance.introspection.public_event_outputs.push_back(
        iv::GraphBuilderPublicEventOutput {
            .port_ordinal = ordinal,
            .config = {
                .name = std::move(name),
                .type = iv::EventTypeId::trigger,
            },
        });
}

struct GraphInputLanesWitness {
    std::vector<iv::TimelineLaneBatchUpdate> timeline_batches {};
    std::vector<std::vector<std::string>> rebuild_requests {};
};

GraphInputLanesWitness *g_graph_input_lanes_witness = nullptr;

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

TEST_F(GraphInputLanesTest, InstanceChangesPublishTimelineBatch)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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

TEST_F(GraphInputLanesTest, SampleInputStateUpdatesFixedRuntimeBinding)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();
    lanes.handle_iv_module_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged{
            .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
        });

    auto binding = instance.runtime_bindings->sample_input(
        iv::runtime_virtual_port_key(
            true, iv::PortKind::sample, "node-1", 0, 0));
    EXPECT_EQ(binding->mode, iv::RuntimeSampleInputMode::scalar);
    EXPECT_EQ(binding->value, iv::Sample{440.0f});

    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, "node-1"),
        .member_ordinal = 0,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
    });
    EXPECT_EQ(binding->mode, iv::RuntimeSampleInputMode::scalar);
    lanes.handle_task_runner_after_pass(
        iv::TasksRunnerAfterPass{.graph_revision = 1});

    EXPECT_EQ(binding->mode, iv::RuntimeSampleInputMode::timeline);
    EXPECT_TRUE(binding->timeline_lane);
    EXPECT_TRUE(witness.rebuild_requests.empty());
}

TEST_F(GraphInputLanesTest, TiledBundleUsesOneStereoVirtualPortAndOneConcreteMemberBinding)
{
    iv::GraphInputLanes lanes;
    auto instance = make_tiled_stereo_input_instance();
    auto const authored_virtual_node_id = instance.introspection.virtual_nodes.front().id;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, authored_virtual_node_id),
        .member_ordinal = std::nullopt,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
    });
    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, authored_virtual_node_id),
        .member_ordinal = 0,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    auto const virtual_id = runtime_node_id(instance.instance_id, authored_virtual_node_id);
    auto const bindings = lanes.graph_input_lane_bindings(
        iv::ProjectGraphInputLaneBindingsRequest{.ports = {
            iv::GraphInputPortDescriptor{
                .virtual_node_id = virtual_id,
                .port_kind = iv::PortKind::sample,
                .port_ordinal = 0,
                .sample_channel_type = iv::ChannelTypeId::stereo,
            },
            iv::GraphInputPortDescriptor{
                .virtual_node_id = virtual_id,
                .node_bundle_port_ordinal = 0,
                .port_kind = iv::PortKind::sample,
                .port_ordinal = 0,
                .sample_channel_type = iv::ChannelTypeId::stereo,
            },
        }});
    ASSERT_EQ(bindings.virtual_sample_knobs.size(), 1u);
    ASSERT_EQ(bindings.sample_inputs.size(), 1u);
    EXPECT_EQ(
        bindings.virtual_sample_knobs.front().port.sample_channel_type,
        iv::ChannelTypeId::stereo);
    EXPECT_EQ(bindings.sample_inputs.front().port.node_bundle_port_ordinal, 0u);

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 2});
    auto const rebuilt_bindings = lanes.graph_input_lane_bindings(
        iv::ProjectGraphInputLaneBindingsRequest{.ports = {
            iv::GraphInputPortDescriptor{
                .virtual_node_id = virtual_id,
                .port_kind = iv::PortKind::sample,
                .port_ordinal = 0,
                .sample_channel_type = iv::ChannelTypeId::stereo,
            },
        }});
    ASSERT_EQ(rebuilt_bindings.virtual_sample_knobs.size(), 1u);
    EXPECT_EQ(
        rebuilt_bindings.virtual_sample_knobs.front().knob_lane,
        bindings.virtual_sample_knobs.front().knob_lane);
}

TEST_F(GraphInputLanesTest, AnnotatedPublicInputsGroupRepeatedSourceIntoOneVirtualLane)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    auto const gain_source = source_info(
        "public-gain", "/tmp/module/module.cpp", 10, 11);
    add_public_sample_input(
        instance,
        0,
        "gain",
        iv::Sample{1.0f},
        iv::Sample{0.0f},
        iv::Sample{2.0f},
        gain_source);
    add_public_sample_input(
        instance,
        1,
        "gain",
        iv::Sample{1.0f},
        iv::Sample{0.0f},
        iv::Sample{2.0f},
        gain_source);

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });

    auto const inputs = lanes.public_sample_inputs();
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].source_identity, "public-gain");
    EXPECT_EQ(inputs[0].virtual_state, "timelineLane");
    EXPECT_EQ(inputs[0].member_ordinals, (std::vector<size_t>{0, 1}));
    EXPECT_EQ(inputs[0].default_value, iv::Sample{1.0f});
    EXPECT_EQ(inputs[0].min, std::optional<iv::Sample>{iv::Sample{0.0f}});
    EXPECT_EQ(inputs[0].max, std::optional<iv::Sample>{iv::Sample{2.0f}});
}

TEST_F(GraphInputLanesTest, PublicInputVirtualAndConcreteStatesReconcileIndependently)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    auto const loop_source = source_info(
        "loop-input", "/tmp/module/module.cpp", 20, 21);
    add_public_sample_input(
        instance, 0, {}, iv::Sample{1.0f}, std::nullopt, std::nullopt, loop_source);
    add_public_sample_input(
        instance, 1, {}, iv::Sample{1.0f}, std::nullopt, std::nullopt, loop_source);

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });

    lanes.set_public_sample_input_state(iv::ProjectSetPublicSampleInputStateRequest{
        .instance_id = instance.instance_id,
        .source_identity = "loop-input",
        .member_ordinal = 1,
        .state = iv::ProjectSampleInputState::disconnected,
    });
    lanes.set_public_sample_input_state(iv::ProjectSetPublicSampleInputStateRequest{
        .instance_id = instance.instance_id,
        .source_identity = "loop-input",
        .member_ordinal = std::nullopt,
        .state = iv::ProjectSampleInputState::disconnected,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    EXPECT_FALSE(witness.timeline_batches.empty());
    EXPECT_TRUE(witness.rebuild_requests.empty());
}

TEST_F(GraphInputLanesTest, AnnotatedPublicEventInputsShareVirtualTimelineLane)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    auto const trigger_source = source_info(
        "public-trigger", "/tmp/module/module.cpp", 30, 37);
    add_public_event_input(instance, 0, "trigger", trigger_source);
    add_public_event_input(instance, 1, "trigger", trigger_source);

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    auto const inputs = lanes.public_event_inputs();
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].source_identity, "public-trigger");
    EXPECT_EQ(inputs[0].virtual_state, "timelineLane");
    EXPECT_EQ(inputs[0].member_ordinals, (std::vector<size_t>{0, 1}));
    EXPECT_EQ(
        inputs[0].member_states,
        (std::vector<std::string>{"virtualFollow", "virtualFollow"}));
}

TEST_F(GraphInputLanesTest, DeletingLastInstancePublishesTimelineRemovals)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    ASSERT_GE(witness.timeline_batches.size(), 1u);
    size_t upsert_count = 0;
    for (auto const &batch : witness.timeline_batches) {
        upsert_count += batch.upserts.size();
    }
    EXPECT_GE(upsert_count, 1u);
}

TEST_F(GraphInputLanesTest, VirtualSampleInputOverrideDoesNotPublishTimelineBatchByDefault)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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

TEST_F(GraphInputLanesTest, VacantSampleInputsDefaultToVirtualFollowWithoutTimelineDependencies)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    }, &ack);
    EXPECT_EQ(
        ack.prerequisite_lanes_for("instance:1")
            .value_or(std::vector<iv::LaneId>{})
            .size(),
        0u);
}

TEST_F(GraphInputLanesTest, VirtualSampleInputTimelineStatePublishesTimelineDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();
    auto const authored_virtual_node_id = instance.introspection.virtual_nodes.front().id;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    witness.timeline_batches.clear();
    witness.rebuild_requests.clear();

    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, authored_virtual_node_id),
        .member_ordinal = std::nullopt,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    ASSERT_EQ(witness.timeline_batches.size(), 1u);
    iv::TimelineLaneUpsert const *created_lane = nullptr;
    for (auto const &upsert : witness.timeline_batches.front().upserts) {
        if (upsert.metadata.has_unit("dsp_graph.graph_input")
            && upsert.metadata.has_unit("dsp_graph.knob")
            && upsert.metadata.has_unit("dsp_graph.virtual")
            && upsert.metadata.has_unit("dsp_graph.sample")) {
            created_lane = &upsert;
            break;
        }
    }
    ASSERT_NE(created_lane, nullptr);
    auto const created_lane_id = created_lane->lane;
    EXPECT_EQ(lane_output_name(created_lane->make_node()), "frequency");

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    }, &ack);
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1")
            .value_or(std::vector<iv::LaneId>{})
            .size(),
        1u);
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 2});

    auto const bindings = lanes.graph_input_lane_bindings(
        iv::ProjectGraphInputLaneBindingsRequest{
            .ports = {iv::GraphInputPortDescriptor{
                .virtual_node_id = runtime_node_id(
                    instance.instance_id, authored_virtual_node_id),
                .port_kind = iv::PortKind::sample,
                .port_ordinal = 0,
                .sample_channel_type = iv::ChannelTypeId::mono,
            }},
        });
    ASSERT_EQ(bindings.virtual_sample_knobs.size(), 1u);
    EXPECT_EQ(bindings.virtual_sample_knobs.front().knob_lane, created_lane_id);
    for (auto const &batch : witness.timeline_batches) {
        EXPECT_EQ(
            std::find(batch.removals.begin(), batch.removals.end(), created_lane_id),
            batch.removals.end());
    }
}

TEST_F(GraphInputLanesTest, ConcreteSampleInputOverrideDoesNotPublishTimelineBatchByDefault)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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
    auto const authored_virtual_node_id = instance.introspection.virtual_nodes.front().id;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    witness.timeline_batches.clear();
    witness.rebuild_requests.clear();

    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, authored_virtual_node_id),
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    ASSERT_EQ(witness.timeline_batches.size(), 1u);

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    }, &ack);
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1")
            .value_or(std::vector<iv::LaneId>{})
            .size(),
        1u);
}

TEST_F(GraphInputLanesTest, ConcreteSampleInputDefaultClearsExplicitTimelineState)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();
    auto const authored_virtual_node_id = instance.introspection.virtual_nodes.front().id;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, authored_virtual_node_id),
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    witness.timeline_batches.clear();
    witness.rebuild_requests.clear();

    lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, authored_virtual_node_id),
        .member_ordinal = 0u,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::default_,
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 2});

    ASSERT_EQ(witness.timeline_batches.size(), 1u);

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    }, &ack);
    EXPECT_EQ(
        ack.prerequisite_lanes_for("instance:1")
            .value_or(std::vector<iv::LaneId>{})
            .size(),
        0u);
}

TEST_F(GraphInputLanesTest, VacantEventInputsDefaultToDisconnectedWithoutTimelineDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    }, &ack);
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1")
            .value_or(std::vector<iv::LaneId>{})
            .size(),
        0u);
}

TEST_F(GraphInputLanesTest, ConnectedEventInputsDefaultToDisconnectedWithoutTimelineDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();
    instance.introspection.virtual_nodes.front().event_inputs.front().connectivity =
        iv::VirtualPortConnectivity::connected;
    instance.introspection.virtual_nodes.front().members.front().event_inputs.front().connectivity =
        iv::VirtualPortConnectivity::connected;

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    }, &ack);
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});
    EXPECT_EQ(
        ack.prerequisite_lanes_for("instance:1")
            .value_or(std::vector<iv::LaneId>{})
            .size(),
        0u);
}

TEST_F(GraphInputLanesTest, ConcreteEventInputTimelineStatePublishesTimelineDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    }, &ack);
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1")
            .value_or(std::vector<iv::LaneId>{})
            .size(),
        1u);
}

TEST_F(GraphInputLanesTest, ConcreteEventInputDefaultClearsExplicitTimelineState)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_ports();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    }, &ack);
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1")
            .value_or(std::vector<iv::LaneId>{})
            .size(),
        0u);
}

TEST_F(GraphInputLanesTest, VirtualOutputsDoNotAutoCreateTimelineLanesWithoutExplicitState)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_output_ports();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    bool saw_output_lane = false;
    for (auto const &batch : witness.timeline_batches) {
        saw_output_lane = saw_output_lane
            || batch_has_output_lane(batch, "dsp_graph.virtual");
    }
    EXPECT_FALSE(saw_output_lane);
}

TEST_F(GraphInputLanesTest, PublicSampleOutputCreatesAutomaticTimelineLane)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    set_public_sample_output(instance, {}, iv::ChannelTypeId::mono);

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    bool saw_public_output = false;
    for (auto const &batch : witness.timeline_batches) {
        saw_public_output = saw_public_output
            || batch_has_public_lane(
                batch, "dsp_graph.public_output", "dsp_graph.sample");
    }
    EXPECT_TRUE(saw_public_output);
}

TEST_F(GraphInputLanesTest, NamedPublicSampleOutputUsesItsDeclaredNameForTheLane)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    set_public_sample_output(instance, "main", iv::ChannelTypeId::mono);

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    iv::TimelineLaneUpsert const *public_output = nullptr;
    for (auto const &batch : witness.timeline_batches) {
        for (auto const &upsert : batch.upserts) {
            if (upsert.metadata.has_unit("dsp_graph.public_output")
                && upsert.metadata.has_unit("dsp_graph.sample")) {
                public_output = &upsert;
                break;
            }
        }
    }
    ASSERT_NE(public_output, nullptr);
    EXPECT_EQ(lane_output_name(public_output->make_node()), "main");
}

TEST_F(GraphInputLanesTest, PublicStereoOutputContributorsShareOneTimelineLane)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    auto &family = set_public_sample_output(
        instance, "main", iv::ChannelTypeId::stereo);
    auto const source_a = source_info("output-a", "test.cpp", 1, 2);
    auto const source_b = source_info("output-b", "test.cpp", 3, 4);
    family.channels[0].source_infos = {source_a, source_a};
    family.channels[1].source_infos = {source_b, source_b};

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    size_t public_stereo_lane_count = 0;
    for (auto const &batch : witness.timeline_batches) {
        for (auto const &upsert : batch.upserts) {
            if (upsert.metadata.has_unit("dsp_graph.public_output")
                && upsert.sample_channel_type == iv::ChannelTypeId::stereo) {
                ++public_stereo_lane_count;
            }
        }
    }
    EXPECT_EQ(public_stereo_lane_count, 1u);
}

TEST_F(GraphInputLanesTest, UpdatingInstanceDoesNotRemoveExistingPublicSampleOutputLane)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    set_public_sample_output(instance, {}, iv::ChannelTypeId::mono);

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    for (auto const &batch : witness.timeline_batches) {
        EXPECT_EQ(
            std::find(batch.removals.begin(), batch.removals.end(), *public_output_lane),
            batch.removals.end());
    }
}

TEST_F(GraphInputLanesTest, UpdatingPublicSampleOutputFromMonoToStereoRecreatesItsLane)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    set_public_sample_output(instance, "main", iv::ChannelTypeId::mono);

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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
    set_public_sample_output(instance, "main", iv::ChannelTypeId::stereo);
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    bool removed_old_lane = false;
    bool recreated_stereo_lane = false;
    for (auto const &batch : witness.timeline_batches) {
        removed_old_lane = removed_old_lane
            || std::ranges::contains(batch.removals, *public_output_lane);
        for (auto const &upsert : batch.upserts) {
            if (upsert.lane == *public_output_lane
                && upsert.sample_channel_type == iv::ChannelTypeId::stereo) {
                recreated_stereo_lane = true;
            }
        }
    }
    EXPECT_TRUE(removed_old_lane);
    EXPECT_TRUE(recreated_stereo_lane);
}

TEST_F(GraphInputLanesTest, RenamingPublicSampleOutputCreatesANewLane)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    set_public_sample_output(instance, "main", iv::ChannelTypeId::mono);
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    std::optional<iv::LaneId> initial_lane;
    std::optional<iv::InternedString> initial_external_id;
    for (auto const &batch : witness.timeline_batches) {
        for (auto const &upsert : batch.upserts) {
            if (upsert.metadata.has_unit("dsp_graph.public_output")) {
                initial_lane = upsert.lane;
                initial_external_id = upsert.external_id;
            }
        }
    }
    ASSERT_TRUE(initial_lane.has_value());
    ASSERT_TRUE(initial_external_id.has_value());
    witness.timeline_batches.clear();

    set_public_sample_output(instance, "main1", iv::ChannelTypeId::mono);
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    bool removed_old = false;
    bool added_new = false;
    bool assigned_new_external_id = false;
    for (auto const &batch : witness.timeline_batches) {
        removed_old = removed_old
            || std::ranges::contains(batch.removals, *initial_lane);
        for (auto const &upsert : batch.upserts) {
            if (upsert.lane != *initial_lane) {
                added_new = true;
                assigned_new_external_id = assigned_new_external_id
                    || upsert.external_id != *initial_external_id;
            }
        }
    }
    EXPECT_TRUE(removed_old);
    EXPECT_TRUE(added_new);
    EXPECT_TRUE(assigned_new_external_id);
}

TEST_F(GraphInputLanesTest, PublicSampleInputCreatesAutomaticTimelineLaneAndDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    add_public_sample_input(
        instance,
        0,
        "frequency",
        iv::Sample{0.5f},
        std::nullopt,
        std::nullopt,
        std::nullopt,
        true);

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    }, &ack);
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    bool saw_public_input = false;
    for (auto const &batch : witness.timeline_batches) {
        saw_public_input = saw_public_input
            || batch_has_public_lane(
                batch, "dsp_graph.public_input", "dsp_graph.sample");
    }
    EXPECT_TRUE(saw_public_input);
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1")
            .value_or(std::vector<iv::LaneId>{})
            .size(),
        1u);
}

TEST_F(GraphInputLanesTest, PublicSampleInputPreservesBoundsFromIntrospection)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_base();
    add_public_sample_input(
        instance,
        0,
        {},
        iv::Sample{0.5f},
        iv::Sample{-2.0f},
        iv::Sample{4.0f},
        source_info("bounded-input"));

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });

    auto const inputs = lanes.public_sample_inputs();
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_TRUE(inputs.front().name.empty());
    EXPECT_FLOAT_EQ(static_cast<float>(inputs.front().default_value), 0.5f);
    EXPECT_EQ(inputs.front().min, std::optional<iv::Sample>{iv::Sample{-2.0f}});
    EXPECT_EQ(inputs.front().max, std::optional<iv::Sample>{iv::Sample{4.0f}});
}

TEST_F(GraphInputLanesTest, PublicEventPortsCreateAutomaticLanesAndDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_ports();
    add_public_event_input(instance, 0, "trigger", std::nullopt, true);
    add_public_event_output(instance, 0, "trigger");

    iv::IvModuleInstanceBuildersAckBuilder ack;
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    }, &ack);
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});

    bool saw_public_event_input = false;
    bool saw_public_event_output = false;
    for (auto const &batch : witness.timeline_batches) {
        saw_public_event_input = saw_public_event_input
            || batch_has_public_lane(
                batch, "dsp_graph.public_input", "dsp_graph.event");
        saw_public_event_output = saw_public_event_output
            || batch_has_public_lane(
                batch, "dsp_graph.public_output", "dsp_graph.event");
    }
    EXPECT_TRUE(saw_public_event_input);
    EXPECT_TRUE(saw_public_event_output);
    ASSERT_EQ(
        ack.prerequisite_lanes_for("instance:1")
            .value_or(std::vector<iv::LaneId>{})
            .size(),
        1u);
}

TEST_F(GraphInputLanesTest, VirtualSampleOutputTimelineStateCreatesAggregationLaneWithDspDependency)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_output_ports();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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
        "dsp_graph.virtual",
        &upsert));
    ASSERT_NE(upsert, nullptr);
    auto const runtime_binding = instance.runtime_bindings->output(
        iv::runtime_virtual_port_key(
            false,
            iv::PortKind::sample,
            "node-1",
            std::nullopt,
            0));
    EXPECT_EQ(runtime_binding->target_lane, upsert->lane);
    EXPECT_NE(runtime_binding->publish_sample_block, nullptr);

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });

    ASSERT_EQ(upsert->external_task_dependencies.size(), 1u);
    EXPECT_EQ(
        upsert->external_task_dependencies.front(),
        iv::iv_module_instance_dsp_task_id("instance:1"));
}

TEST_F(GraphInputLanesTest, SettingSampleOutputStateDoesNotRebuildInstance)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_output_ports();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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

    EXPECT_TRUE(witness.rebuild_requests.empty());
}

TEST_F(GraphInputLanesTest, ConcreteSampleOutputTimelineStateCreatesDedicatedLane)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_member_output_ports();
    auto const authored_virtual_node_id = instance.introspection.virtual_nodes.front().id;

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});
    witness.timeline_batches.clear();
    lanes.set_sample_output_state(iv::ProjectSetSampleOutputStateRequest{
        .node_id = runtime_node_id(instance.instance_id, authored_virtual_node_id),
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
    auto const runtime_binding = instance.runtime_bindings->output(
        iv::runtime_virtual_port_key(
            false,
            iv::PortKind::sample,
            authored_virtual_node_id,
            0,
            0));
    EXPECT_EQ(runtime_binding->target_lane, upsert->lane);
    EXPECT_NE(runtime_binding->publish_sample_block, nullptr);

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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
        "dsp_graph.virtual",
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
    EXPECT_NE(
        std::find(removals.begin(), removals.end(), created_lane),
        removals.end());
}

TEST_F(GraphInputLanesTest, SampleOutputLanesRemainMonoAcrossRebuild)
{
    iv::GraphInputLanes lanes;
    auto instance = make_instance_with_output_ports();

    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged {
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
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
        if (batch_has_output_lane(batch, "dsp_graph.virtual", &created)) {
            break;
        }
    }
    ASSERT_NE(created, nullptr);
    ASSERT_TRUE(created->sample_channel_type.has_value());
    EXPECT_EQ(*created->sample_channel_type, iv::ChannelTypeId::mono);

    witness.timeline_batches.clear();
    lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .updated = {iv::IvModuleInstanceBuilderRef{.instance = &instance}},
    });
    lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 2});

    for (auto const &batch : witness.timeline_batches) {
        EXPECT_EQ(
            std::find(batch.removals.begin(), batch.removals.end(), created->lane),
            batch.removals.end());
    }
}

TEST_F(GraphInputLanesTest, SampleOutputBlockRoundTripsThroughGraphInputLanesStorage)
{
    iv::GraphInputLanes lanes;
    auto const lane = iv::LaneId{42};
    lanes.prepare_sample_output_block(lane);
    auto const block = std::array<iv::Sample, 3>{0.25f, -0.5f, 1.0f};

    lanes.handle_sample_block_published(
        lane, mono_block(std::span<iv::Sample const>(block)));

    auto const stored = lanes.handle_sample_block_requested(lane);
    EXPECT_EQ(stored.channel_layout.channel_type, iv::ChannelTypeId::mono);
    EXPECT_EQ(stored.channel_layout.sample_layout, iv::SampleStreamLayout::planar);
    EXPECT_EQ(stored.frame_count, block.size());
    EXPECT_EQ(
        sample_values(stored),
        (std::vector<iv::Sample>{0.25f, -0.5f, 1.0f}));
}

TEST_F(GraphInputLanesTest, EventOutputBlockRoundTripsThroughGraphInputLanesStorage)
{
    iv::GraphInputLanes lanes;
    auto const lane = iv::LaneId{43};
    lanes.prepare_event_output_block(lane);
    std::vector<iv::TimedEvent> events{
        iv::TimedEvent{.time = 1, .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = 4, .value = iv::BoundaryEvent{.is_begin = true}},
    };

    lanes.handle_event_block_published(
        lane, std::span<iv::TimedEvent const>(events));

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
    lanes.prepare_sample_output_block(lane);
    auto const first = std::array<iv::Sample, 2>{1.0f, 2.0f};
    auto const second = std::array<iv::Sample, 1>{-3.0f};

    lanes.handle_sample_block_published(
        lane, mono_block(std::span<iv::Sample const>(first)));
    lanes.handle_sample_block_published(
        lane, mono_block(std::span<iv::Sample const>(second)));

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
    lanes.prepare_sample_output_block(lane);
    auto const block = std::array<iv::Sample, 6>{
        0.25f, -0.25f, 0.5f, -0.5f, 1.0f, -1.0f};

    lanes.handle_sample_block_published(
        lane,
        stereo_interleaved_block(std::span<iv::Sample const>(block), 3));

    auto const stored = lanes.handle_sample_block_requested(lane);
    EXPECT_EQ(stored.channel_layout.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(
        stored.channel_layout.sample_layout,
        iv::SampleStreamLayout::interleaved);
    EXPECT_EQ(stored.frame_count, 3u);
    EXPECT_EQ(
        sample_values(stored),
        (std::vector<iv::Sample>{0.25f, -0.25f, 0.5f, -0.5f, 1.0f, -1.0f}));
}
