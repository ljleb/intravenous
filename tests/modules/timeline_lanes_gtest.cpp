#include <intravenous/basic_lane_nodes/controls.h>
#include <intravenous/runtime/graph_input_lane_controller.h>
#include <intravenous/basic_lane_nodes/type_erased.h>
#include <intravenous/lane_node/graph.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/timeline.h>

#include <gtest/gtest.h>

#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <variant>

namespace {
    struct TestCompiledEventLaneNode {
        std::array<iv::CompiledEventLaneInputConfig, 1> compiled_event_inputs() const
        {
            return {
                iv::CompiledEventLaneInputConfig {
                    .name = "events",
                    .event_type = iv::EventTypeId::trigger,
                },
            };
        }

        iv::CompiledEventLaneOutputConfig output() const
        {
            return {
                .name = "events",
                .event_type = iv::EventTypeId::trigger,
            };
        }

        void tick_block_compiled(iv::CompiledLaneTickContext<TestCompiledEventLaneNode>& ctx)
        {
            static_assert(std::same_as<
                typename iv::CompiledLaneTickContext<TestCompiledEventLaneNode>::OutputView,
                iv::CompiledEventLaneOutput
            >);
            EXPECT_EQ(ctx.compiled_event_inputs().size(), 1u);
            ctx.out().push(iv::TimedEvent {
                .time = ctx.start_index(),
                .value = iv::TriggerEvent {},
            });
        }
    };

    struct TestDynamicOutputLaneNode {
        iv::LaneOutputConfig output() const
        {
            return iv::RealtimeSampleLaneOutputConfig {
                .name = "dynamic",
            };
        }

        void tick_block_realtime(iv::RealtimeLaneTickContext<TestDynamicOutputLaneNode>& ctx)
        {
            static_assert(std::same_as<
                typename iv::RealtimeLaneTickContext<TestDynamicOutputLaneNode>::OutputView,
                iv::LaneOutputView
            >);
            EXPECT_TRUE(std::holds_alternative<iv::RealtimeSampleLaneOutput>(ctx.out()));
        }
    };

    struct TestCompiledSampleSourceLaneNode {
        iv::CompiledSampleLaneOutputConfig output() const
        {
            return {
                .name = "samples",
            };
        }

        void tick_block_compiled(iv::CompiledLaneTickContext<TestCompiledSampleSourceLaneNode>& ctx)
        {
            std::array<iv::Sample, 4> samples {};
            for (size_t i = 0; i < samples.size(); ++i) {
                samples[i] = static_cast<iv::Sample>(ctx.start_index() + i);
            }
            ctx.out().write(ctx.start_index(), samples);
        }
    };

    struct TestCompiledSampleConsumerLaneNode {
        std::array<iv::CompiledSampleLaneInputConfig, 1> compiled_sample_inputs() const
        {
            return {
                iv::CompiledSampleLaneInputConfig {
                    .name = "samples",
                    .default_value = -1.0f,
                },
            };
        }

        iv::CompiledSampleLaneOutputConfig output() const
        {
            return {
                .name = "doubled",
            };
        }

        void tick_block_compiled(iv::CompiledLaneTickContext<TestCompiledSampleConsumerLaneNode>& ctx)
        {
            std::array<iv::Sample, 4> samples {};
            ctx.compiled_sample_input(0).read(ctx.start_index(), samples);
            for (auto& sample : samples) {
                sample *= 2.0f;
            }
            ctx.out().write(ctx.start_index(), samples);
        }
    };

    struct TestStaticRealtimeLaneNode {
        static auto realtime_sample_inputs()
        {
            return std::array {
                iv::RealtimeSampleLaneInputConfig {
                    .name = "signal",
                    .default_value = 3.0f,
                },
            };
        }

        static auto output()
        {
            return iv::RealtimeSampleLaneOutputConfig {
                .name = "out",
            };
        }

        void tick_block_realtime(iv::RealtimeLaneTickContext<TestStaticRealtimeLaneNode>& ctx)
        {
            for (size_t i = 0; i < ctx.sample_count(); ++i) {
                ctx.out().push(ctx.realtime_sample_input(0).get(i));
            }
        }
    };

}

TEST(Lanes, LaneIdsStartValidAndIncrease)
{
    iv::LaneIdAllocator allocator;

    auto const first = allocator.next();
    auto const second = allocator.next();

    EXPECT_TRUE(first);
    EXPECT_TRUE(second);
    EXPECT_NE(first, second);
    EXPECT_LT(first.value, second.value);
}

TEST(Lanes, ConnectionsUseSharedPortKind)
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

TEST(Lanes, GraphInputPortsCanDescribeConcreteEventPorts)
{
    iv::GraphInputPortDescriptor target {
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

TEST(Lanes, GraphInputLanesReuseIdsForStablePorts)
{
    iv::GraphInputLaneRegistry registry;
    std::vector<iv::GraphInputPortDescriptor> ports {
        iv::GraphInputPortDescriptor {
            .logical_node_id = "node",
            .port_kind = iv::PortKind::sample,
            .port_ordinal = 1,
            .port_name = "frequency",
            .port_type = "sample",
        },
    };

    auto const first = registry.reconcile(ports);
    auto const second = registry.reconcile(ports);

    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(first[0].id, second[0].id);
}

TEST(Lanes, GraphInputLanesDropMissingPortsWithoutDeletingConnections)
{
    iv::GraphInputLaneRegistry registry;
    auto const lanes = registry.reconcile({
        iv::GraphInputPortDescriptor {
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

    EXPECT_TRUE(registry.port_descriptor_for(lanes[0].id).has_value());

    auto const after_reload = registry.reconcile({});
    EXPECT_TRUE(after_reload.empty());
    EXPECT_FALSE(registry.port_descriptor_for(lanes[0].id).has_value());

    auto const dangling = registry.dangling_connections({ connection });
    ASSERT_EQ(dangling.size(), 1u);
    EXPECT_EQ(dangling[0].connection, connection);
    ASSERT_TRUE(dangling[0].missing_port.has_value());
    EXPECT_EQ(dangling[0].missing_port->logical_node_id, "node");
    ASSERT_TRUE(dangling[0].missing_port->concrete_member_ordinal.has_value());
    EXPECT_EQ(*dangling[0].missing_port->concrete_member_ordinal, 2u);
}

TEST(Lanes, TypeErasedLaneNodeIntrospectsKnobLane)
{
    iv::TypeErasedLaneNode node = iv::KnobLaneNode {
        .value = 0.25f,
    };

    EXPECT_TRUE(node.compiled_sample_inputs().empty());
    EXPECT_TRUE(node.compiled_event_inputs().empty());
    EXPECT_FALSE(node.supports_tick_block_compiled());
    EXPECT_TRUE(node.supports_tick_block_realtime());
    EXPECT_FALSE(iv::supports_tick_block_compiled(node));
    EXPECT_TRUE(iv::supports_tick_block_realtime(node));
    ASSERT_EQ(node.realtime_sample_inputs().size(), 1u);
    EXPECT_EQ(node.realtime_sample_inputs()[0].name, "value");
    EXPECT_TRUE(node.realtime_event_inputs().empty());
    ASSERT_TRUE(std::holds_alternative<iv::RealtimeSampleLaneOutputConfig>(node.output()));
    EXPECT_EQ(std::get<iv::RealtimeSampleLaneOutputConfig>(node.output()).name, "value");

    auto* knob = node.try_as<iv::KnobLaneNode>();
    ASSERT_NE(knob, nullptr);
    knob->value = 0.75f;
    EXPECT_FLOAT_EQ(node.try_as<iv::KnobLaneNode>()->value, 0.75f);

    iv::UntypedRealtimeLaneTickContext untyped_ctx {
        .request = iv::RealtimeLaneTickRequest {
            .start_index = 12,
            .sample_count = 64,
        },
        .output = iv::RealtimeSampleLaneOutput {},
    };
    iv::RealtimeLaneTickContext<iv::TypeErasedLaneNode> ctx(untyped_ctx);
    EXPECT_EQ(ctx.start_index(), 12u);
    EXPECT_EQ(ctx.sample_count(), 64u);
    EXPECT_TRUE(std::holds_alternative<iv::RealtimeSampleLaneOutput>(ctx.out()));
    node.tick_block_realtime(ctx);
}

TEST(Lanes, TypeErasedLaneNodeIntrospectsGraphSampleInputLane)
{
    iv::TypeErasedLaneNode node = iv::GraphSampleInputLaneNode {
        .default_value = 440.0f,
    };

    EXPECT_TRUE(node.compiled_sample_inputs().empty());
    EXPECT_TRUE(node.compiled_event_inputs().empty());
    ASSERT_EQ(node.realtime_sample_inputs().size(), 1u);
    EXPECT_EQ(node.realtime_sample_inputs()[0].name, "value");
    EXPECT_FLOAT_EQ(node.realtime_sample_inputs()[0].default_value, 440.0f);
    EXPECT_TRUE(node.realtime_event_inputs().empty());
    ASSERT_TRUE(std::holds_alternative<iv::RealtimeSampleLaneOutputConfig>(node.output()));
    EXPECT_EQ(std::get<iv::RealtimeSampleLaneOutputConfig>(node.output()).name, "value");

    auto const* graph_input = node.try_as<iv::GraphSampleInputLaneNode>();
    ASSERT_NE(graph_input, nullptr);
    EXPECT_FLOAT_EQ(graph_input->default_value, 440.0f);
}

TEST(Lanes, TypeErasedLaneNodeIntrospectsGraphEventInputLane)
{
    iv::TypeErasedLaneNode node = iv::GraphEventInputLaneNode {};

    EXPECT_TRUE(node.compiled_sample_inputs().empty());
    EXPECT_TRUE(node.compiled_event_inputs().empty());
    EXPECT_FALSE(node.supports_tick_block_compiled());
    EXPECT_TRUE(node.supports_tick_block_realtime());
    EXPECT_FALSE(iv::supports_tick_block_compiled(node));
    EXPECT_TRUE(iv::supports_tick_block_realtime(node));
    EXPECT_TRUE(node.realtime_sample_inputs().empty());
    ASSERT_EQ(node.realtime_event_inputs().size(), 1u);
    EXPECT_EQ(node.realtime_event_inputs()[0].name, "events");
    ASSERT_TRUE(std::holds_alternative<iv::RealtimeEventLaneOutputConfig>(node.output()));
    EXPECT_EQ(std::get<iv::RealtimeEventLaneOutputConfig>(node.output()).name, "events");

    auto const* graph_input = node.try_as<iv::GraphEventInputLaneNode>();
    ASSERT_NE(graph_input, nullptr);
}

TEST(Lanes, TypeErasedLaneNodeCarriesCompiledCapabilitiesThroughTraits)
{
    iv::TypeErasedLaneNode node = TestCompiledEventLaneNode {};

    EXPECT_TRUE(node.supports_tick_block_compiled());
    EXPECT_TRUE(node.supports_tick_block_realtime());
    EXPECT_TRUE(iv::supports_tick_block_compiled(node));
    EXPECT_TRUE(iv::supports_tick_block_realtime(node));
}

TEST(Lanes, ContextOutputTypeFollowsConcreteOutputConfig)
{
    TestCompiledEventLaneNode node;
    std::array<iv::CompiledEventLaneInput, 1> compiled_event_inputs {};
    iv::UntypedCompiledLaneTickContext untyped_ctx {
        .request = iv::CompiledLaneTickRequest {
            .start_index = 32,
            .end_index = 40,
            .sample_count = 8,
        },
        .compiled_event_inputs = compiled_event_inputs,
        .output = iv::CompiledEventLaneOutput {},
    };
    iv::CompiledLaneTickContext<TestCompiledEventLaneNode> ctx(untyped_ctx);

    node.tick_block_compiled(ctx);
}

TEST(Lanes, ContextOutputTypeIsVariantForDynamicOutputConfig)
{
    TestDynamicOutputLaneNode node;
    iv::UntypedRealtimeLaneTickContext untyped_ctx {
        .request = iv::RealtimeLaneTickRequest {
            .start_index = 0,
            .sample_count = 16,
        },
        .output = iv::RealtimeSampleLaneOutput {},
    };
    iv::RealtimeLaneTickContext<TestDynamicOutputLaneNode> ctx(untyped_ctx);

    node.tick_block_realtime(ctx);
}

TEST(Lanes, StaticLaneDeclarationsAreDetectedWithoutInstanceDispatch)
{
    TestStaticRealtimeLaneNode node;

    auto const sample_inputs = iv::get_realtime_sample_lane_inputs(node);
    ASSERT_EQ(sample_inputs.size(), 1u);
    EXPECT_EQ(sample_inputs[0].name, "signal");
    EXPECT_FLOAT_EQ(sample_inputs[0].default_value, 3.0f);

    auto const output = iv::get_lane_output(node);
    ASSERT_TRUE(std::holds_alternative<iv::RealtimeSampleLaneOutputConfig>(
        iv::normalize_lane_output(output)));
    EXPECT_EQ(output.name, "out");
}

TEST(Lanes, CompiledLaneNodesCanBeTickedThroughRealtimeDispatch)
{
    TestCompiledEventLaneNode node;
    std::vector<iv::TimedEvent> events;
    iv::UntypedRealtimeLaneTickContext untyped_ctx {
        .request = iv::RealtimeLaneTickRequest {
            .start_index = 48,
            .sample_count = 8,
        },
        .output = iv::CompiledEventLaneOutput {
            .events = &events,
        },
    };
    iv::RealtimeLaneTickContext<TestCompiledEventLaneNode> ctx(untyped_ctx);

    iv::do_tick_block_realtime(node, ctx);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].time, 48u);
    EXPECT_TRUE(std::holds_alternative<iv::TriggerEvent>(events[0].value));
}

TEST(Lanes, LaneGraphStoresNodesByStableLaneId)
{
    iv::LaneGraph graph;
    auto const knob = graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode {
        .value = 0.5f,
    }));
    auto const events = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledEventLaneNode {}));

    EXPECT_TRUE(graph.contains(knob));
    EXPECT_TRUE(graph.contains(events));
    EXPECT_NE(knob, events);
    EXPECT_EQ(graph.lane_count(), 2u);
    EXPECT_EQ(graph.realtime_lanes().size(), 1u);
    EXPECT_EQ(graph.compiled_lanes().size(), 1u);
    EXPECT_TRUE(std::holds_alternative<iv::RealtimeSampleLaneOutputConfig>(graph.lane(knob).output));
    EXPECT_TRUE(std::holds_alternative<iv::CompiledEventLaneOutputConfig>(graph.lane(events).output));
}

TEST(Lanes, LaneGraphIndexesConnectionsByInputAndOutput)
{
    iv::LaneGraph graph;
    auto const source = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledEventLaneNode {}));
    auto const target = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledEventLaneNode {}));
    iv::LanePortId const target_input {
        .domain = iv::LanePortDomain::compiled,
        .kind = iv::PortKind::event,
        .ordinal = 0,
    };

    graph.connect(source, target, target_input);
    graph.connect(source, target, target_input);

    ASSERT_EQ(graph.inputs_for(target).size(), 1u);
    EXPECT_EQ(graph.inputs_for(target)[0].source, source);
    EXPECT_EQ(graph.inputs_for(target)[0].input, target_input);
    ASSERT_EQ(graph.outputs_for(source).size(), 1u);
    EXPECT_EQ(graph.outputs_for(source)[0].target, target);
    EXPECT_EQ(graph.outputs_for(source)[0].input, target_input);

    graph.disconnect(source, target, target_input);
    EXPECT_TRUE(graph.inputs_for(target).empty());
    EXPECT_TRUE(graph.outputs_for(source).empty());
}

TEST(Lanes, LaneGraphKeepsConnectionIndexesPerOutputDomain)
{
    iv::LaneGraph graph;
    auto const compiled_source = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledEventLaneNode {}));
    auto const compiled_target = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledEventLaneNode {}));
    auto const realtime_source = graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode {}));
    auto const realtime_target = graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode {}));

    graph.connect(compiled_source, compiled_target, iv::LanePortId {
        .domain = iv::LanePortDomain::compiled,
        .kind = iv::PortKind::event,
        .ordinal = 0,
    });
    graph.connect(realtime_source, realtime_target, iv::realtime_sample_input());

    EXPECT_EQ(graph.compiled_outputs_for(compiled_source).size(), 1u);
    EXPECT_TRUE(graph.realtime_outputs_for(compiled_source).empty());
    EXPECT_EQ(graph.realtime_outputs_for(realtime_source).size(), 1u);
    EXPECT_TRUE(graph.compiled_outputs_for(realtime_source).empty());
}

TEST(Lanes, LaneGraphCanReplaceOneInputContributor)
{
    iv::LaneGraph graph;
    auto const first = graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode {}));
    auto const second = graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode {}));
    auto const target = graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode {}));

    graph.connect(first, target, iv::realtime_sample_input());
    graph.connect_exclusive(second, target, iv::realtime_sample_input());

    ASSERT_EQ(graph.inputs_for(target).size(), 1u);
    EXPECT_EQ(graph.inputs_for(target)[0].source, second);
    EXPECT_TRUE(graph.outputs_for(first).empty());
    ASSERT_EQ(graph.outputs_for(second).size(), 1u);

    graph.disconnect_input(target, iv::realtime_sample_input());
    EXPECT_TRUE(graph.inputs_for(target).empty());
    EXPECT_TRUE(graph.outputs_for(second).empty());
}

TEST(Lanes, LaneGraphTracksOrganizationalParentsWithoutAffectingConnections)
{
    iv::LaneGraph graph;
    auto const parent = graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode {}));
    auto const child = graph.add_lane(iv::TypeErasedLaneNode(iv::GraphSampleInputLaneNode {}));

    graph.add_child(parent, child);
    graph.add_child(parent, child);

    ASSERT_EQ(graph.children_for(parent).size(), 1u);
    EXPECT_EQ(graph.children_for(parent)[0], child);
    ASSERT_EQ(graph.parents_for(child).size(), 1u);
    EXPECT_EQ(graph.parents_for(child)[0], parent);
    EXPECT_TRUE(graph.outputs_for(parent).empty());
    EXPECT_TRUE(graph.inputs_for(child).empty());

    graph.remove_lane(child);
    EXPECT_TRUE(graph.children_for(parent).empty());
}

TEST(Lanes, GraphInputLaneBindingsWireLogicalKnobsToConcreteKnobsToGraphInputs)
{
    iv::LaneGraph graph;
    auto const lanes = iv::GraphInputLaneController {}.reconcile(graph,
        std::array {
            iv::GraphInputPortDescriptor {
                .logical_node_id = "osc",
                .port_kind = iv::PortKind::sample,
                .port_ordinal = 1,
                .port_name = "frequency",
                .port_type = "sample",
            },
            iv::GraphInputPortDescriptor {
                .logical_node_id = "osc",
                .concrete_member_ordinal = 0u,
                .port_kind = iv::PortKind::sample,
                .port_ordinal = 1,
                .port_name = "frequency",
                .port_type = "sample",
            },
            iv::GraphInputPortDescriptor {
                .logical_node_id = "osc",
                .concrete_member_ordinal = 1u,
                .port_kind = iv::PortKind::sample,
                .port_ordinal = 1,
                .port_name = "frequency",
                .port_type = "sample",
            },
        }
    );

    ASSERT_EQ(lanes.sample_inputs.size(), 2u);
    ASSERT_TRUE(lanes.sample_inputs[0].logical_knob_lane.has_value());
    EXPECT_EQ(lanes.sample_inputs[0].logical_knob_lane, lanes.sample_inputs[1].logical_knob_lane);

    for (auto const& control : lanes.sample_inputs) {
        ASSERT_EQ(graph.inputs_for(control.knob_lane).size(), 1u);
        EXPECT_EQ(graph.inputs_for(control.knob_lane)[0].source, *control.logical_knob_lane);
        EXPECT_EQ(graph.inputs_for(control.knob_lane)[0].input, iv::realtime_sample_input());

        ASSERT_EQ(graph.inputs_for(control.graph_input_lane).size(), 1u);
        EXPECT_EQ(graph.inputs_for(control.graph_input_lane)[0].source, control.knob_lane);

        ASSERT_EQ(graph.children_for(control.knob_lane).size(), 1u);
        EXPECT_EQ(graph.children_for(control.knob_lane)[0], control.graph_input_lane);
    }
}

TEST(Lanes, ConcreteKnobOwnershipIsJustDisconnectingTheOverrideInput)
{
    iv::LaneGraph graph;
    auto const lanes = iv::GraphInputLaneController {}.reconcile(graph,
        std::array {
            iv::GraphInputPortDescriptor {
                .logical_node_id = "osc",
                .port_kind = iv::PortKind::sample,
                .port_ordinal = 1,
                .port_name = "frequency",
            },
            iv::GraphInputPortDescriptor {
                .logical_node_id = "osc",
                .concrete_member_ordinal = 0u,
                .port_kind = iv::PortKind::sample,
                .port_ordinal = 1,
                .port_name = "frequency",
            },
        }
    );
    ASSERT_EQ(lanes.sample_inputs.size(), 1u);
    auto const concrete_knob = lanes.sample_inputs[0].knob_lane;
    auto const graph_input = lanes.sample_inputs[0].graph_input_lane;

    ASSERT_FALSE(graph.inputs_for(concrete_knob).empty());
    graph.disconnect_input(concrete_knob, iv::realtime_sample_input());

    EXPECT_TRUE(graph.inputs_for(concrete_knob).empty());
    ASSERT_EQ(graph.inputs_for(graph_input).size(), 1u);
    EXPECT_EQ(graph.inputs_for(graph_input)[0].source, concrete_knob);
}

TEST(Lanes, GraphInputLaneBindingReconciliationReusesSemanticKnobLaneIdentities)
{
    iv::LaneGraph graph;
    std::array ports {
        iv::GraphInputPortDescriptor {
            .logical_node_id = "osc",
            .port_kind = iv::PortKind::sample,
            .port_ordinal = 1,
            .port_name = "frequency",
        },
        iv::GraphInputPortDescriptor {
            .logical_node_id = "osc",
            .concrete_member_ordinal = 0u,
            .port_kind = iv::PortKind::sample,
            .port_ordinal = 1,
            .port_name = "frequency",
        },
    };

    auto const first = iv::GraphInputLaneController {}.reconcile(graph, ports);
    auto const second = iv::GraphInputLaneController {}.reconcile(graph, ports);

    ASSERT_EQ(first.logical_sample_knobs.size(), 1u);
    ASSERT_EQ(second.logical_sample_knobs.size(), 1u);
    ASSERT_EQ(first.sample_inputs.size(), 1u);
    ASSERT_EQ(second.sample_inputs.size(), 1u);
    EXPECT_EQ(first.logical_sample_knobs[0].knob_lane, second.logical_sample_knobs[0].knob_lane);
    EXPECT_EQ(first.sample_inputs[0].knob_lane, second.sample_inputs[0].knob_lane);
    EXPECT_EQ(first.sample_inputs[0].graph_input_lane, second.sample_inputs[0].graph_input_lane);
}

TEST(Lanes, LaneGraphRejectsInvalidConnections)
{
    iv::LaneGraph graph;
    auto const event_source = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledEventLaneNode {}));
    auto const realtime_target = graph.add_lane(iv::TypeErasedLaneNode(iv::GraphEventInputLaneNode {}));
    auto const compiled_target = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledEventLaneNode {}));

    EXPECT_THROW(
        graph.connect(event_source, realtime_target, iv::LanePortId {
            .domain = iv::LanePortDomain::realtime,
            .kind = iv::PortKind::sample,
            .ordinal = 0,
        }),
        std::runtime_error
    );
    EXPECT_THROW(
        graph.connect(event_source, compiled_target, iv::LanePortId {
            .domain = iv::LanePortDomain::compiled,
            .kind = iv::PortKind::event,
            .ordinal = 1,
        }),
        std::runtime_error
    );
}

TEST(Lanes, LaneGraphAllowsCompiledOutputsToFeedRealtimeInputs)
{
    iv::LaneGraph graph;
    auto const event_source = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledEventLaneNode {}));
    auto const realtime_target = graph.add_lane(iv::TypeErasedLaneNode(iv::GraphEventInputLaneNode {}));

    EXPECT_NO_THROW(
        graph.connect(event_source, realtime_target, iv::LanePortId {
            .domain = iv::LanePortDomain::realtime,
            .kind = iv::PortKind::event,
            .ordinal = 0,
        })
    );
}

TEST(Lanes, LaneGraphRemoveLaneDisconnectsDanglingConnections)
{
    iv::LaneGraph graph;
    auto const source = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledEventLaneNode {}));
    auto const middle = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledEventLaneNode {}));
    auto const target = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledEventLaneNode {}));
    iv::LanePortId const compiled_event_input {
        .domain = iv::LanePortDomain::compiled,
        .kind = iv::PortKind::event,
        .ordinal = 0,
    };

    graph.connect(source, middle, compiled_event_input);
    graph.connect(middle, target, compiled_event_input);
    graph.remove_lane(middle);

    EXPECT_FALSE(graph.contains(middle));
    EXPECT_TRUE(graph.inputs_for(middle).empty());
    EXPECT_TRUE(graph.outputs_for(middle).empty());
    EXPECT_TRUE(graph.outputs_for(source).empty());
    EXPECT_TRUE(graph.inputs_for(target).empty());
}
