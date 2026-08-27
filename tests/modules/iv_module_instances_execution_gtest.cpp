#include <intravenous/runtime/iv_module_instances_execution.h>
#include <intravenous/runtime/iv_module_instances_iv_module_instances_execution_bridge.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/runtime_binding_nodes.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <vector>

namespace {
    struct NoopNode {
        void tick_block(iv::TickBlockContext<NoopNode> const&) const {}
    };

    struct CountingNode {
        int *ticks = nullptr;

        void tick_block(iv::TickBlockContext<CountingNode> const&) const
        {
            if (ticks) {
                *ticks += 1;
            }
        }
    };

    struct IndexRecordingNode {
        std::vector<size_t> *indices = nullptr;

        void tick_block(iv::TickBlockContext<IndexRecordingNode> const &ctx) const
        {
            if (indices) {
                indices->push_back(ctx.index);
            }
        }
    };

    struct SampleRateRecordingNode {
        size_t *sample_rate = nullptr;
        float *sample_period = nullptr;

        void tick_block(iv::TickBlockContext<SampleRateRecordingNode> const &ctx) const
        {
            if (sample_rate) *sample_rate = ctx.sample_rate;
            if (sample_period) *sample_period = static_cast<float>(ctx.sample_period());
        }
    };

    struct ReleaseRequiresLiveModuleNode {
        bool *module_is_live = nullptr;
        bool *release_saw_live_module = nullptr;

        void release(iv::ReleaseContext<ReleaseRequiresLiveModuleNode> const&) const
        {
            if (release_saw_live_module != nullptr) {
                *release_saw_live_module = module_is_live != nullptr && *module_is_live;
            }
        }

        void tick_block(iv::TickBlockContext<ReleaseRequiresLiveModuleNode> const&) const
        {}
    };

    struct RuntimeBindingExecutionWitness {
        struct SamplePublication {
            iv::LaneId lane {};
            std::vector<iv::Sample> samples {};
            iv::ChannelLayout layout {};
            size_t frame_count = 0;
        };

        std::vector<SamplePublication> sample_publications {};
        std::vector<iv::TimedEvent> event_input {};
        std::vector<iv::LaneId> event_lanes {};
        std::vector<std::vector<iv::TimedEvent>> event_publications {};
    };

    RuntimeBindingExecutionWitness *runtime_binding_witness = nullptr;

    struct RuntimeBindingWitnessScope {
        explicit RuntimeBindingWitnessScope(
            RuntimeBindingExecutionWitness& witness)
        {
            runtime_binding_witness = &witness;
        }

        ~RuntimeBindingWitnessScope()
        {
            runtime_binding_witness = nullptr;
        }
    };

    iv::RuntimeEventBlockView read_runtime_event_block(
        iv::LaneId,
        size_t,
        size_t)
    {
        if (!runtime_binding_witness) return {};
        return {
            .data = runtime_binding_witness->event_input.data(),
            .size = runtime_binding_witness->event_input.size(),
        };
    }

    void publish_runtime_sample_block(
        iv::LaneId lane,
        std::span<iv::Sample const> samples,
        iv::ChannelLayout layout,
        size_t frame_count)
    {
        if (!runtime_binding_witness) return;
        runtime_binding_witness->sample_publications.push_back({
            .lane = lane,
            .samples = {samples.begin(), samples.end()},
            .layout = layout,
            .frame_count = frame_count,
        });
    }

    void publish_runtime_event_block(
        iv::LaneId lane,
        std::span<iv::TimedEvent const> events)
    {
        if (!runtime_binding_witness) return;
        runtime_binding_witness->event_lanes.push_back(lane);
        runtime_binding_witness->event_publications.emplace_back(
            events.begin(), events.end());
    }

    std::shared_ptr<iv::GraphRuntimeBindings> make_test_runtime_bindings()
    {
        return std::make_shared<iv::GraphRuntimeBindings>(
            iv::GraphRuntimeBindings::Callbacks{
                .read_timeline_sample_block = nullptr,
                .read_timeline_event_block = &read_runtime_event_block,
                .publish_sample_block = &publish_runtime_sample_block,
                .publish_event_block = &publish_runtime_event_block,
            });
    }

    consteval auto make_runtime_sample_binding_graph()
    {
        iv::GraphBuilder graph;
        auto input = graph.input<"input">(iv::Sample{0.0f});
        graph.outputs(iv::PortName<"output">{} = input);
        return graph.build_execution_root_node().graph;
    }

    consteval auto make_runtime_event_binding_graph()
    {
        iv::GraphBuilder graph;
        auto input = graph.event_input<"input">(iv::EventTypeId::trigger);
        graph.event_outputs(iv::PortName<"output">{} = input);
        graph.outputs();
        return graph.build_execution_root_node().graph;
    }

    static constexpr auto runtime_sample_binding_graph =
        make_runtime_sample_binding_graph();
    static constexpr auto runtime_event_binding_graph =
        make_runtime_event_binding_graph();
    static constexpr iv::StaticGraphRoot<runtime_sample_binding_graph>
        runtime_sample_binding_root {};
    static constexpr iv::StaticGraphRoot<runtime_event_binding_graph>
        runtime_event_binding_root {};

    iv::IvModuleInstance make_instance(std::string instance_id)
    {
        iv::IvModuleInstance instance {};
        instance.instance_id = std::move(instance_id);
        instance.definition_id = "definition:1";
        instance.module_id = "module.test";
        return instance;
    }
}

TEST(IvModuleInstancesExecution, CreatesOneTaskPerRootInstance)
{
    iv::IvModuleInstancesExecution execution;
    auto instance = make_instance("instance:1");
    NoopNode root;

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .created = {
                iv::IvModuleInstanceBuilderRef {
                    .instance = &instance,
                    .root = iv::WeakTypeErasedNode(root),
                },
            },
        });

    ASSERT_EQ(update.update.to_create.size(), 1u);
    EXPECT_EQ(update.update.to_create[0].id, "iv_module_instance:dsp:instance:1");
    EXPECT_TRUE(update.update.to_create[0].depends_on.empty());
    EXPECT_TRUE(update.update.to_delete.empty());
}

TEST(IvModuleInstancesExecution, RootPrerequisiteLanesBecomeTaskDependencies)
{
    iv::IvModuleInstancesExecution execution;
    auto instance = make_instance("instance:1");
    NoopNode root;

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .created = {
                iv::IvModuleInstanceBuilderRef {
                    .instance = &instance,
                    .root = iv::WeakTypeErasedNode(root),
                    .prerequisite_lanes = {
                        iv::LaneId {4},
                        iv::LaneId {8},
                    },
                },
            },
        });

    ASSERT_EQ(update.update.to_create.size(), 1u);
    EXPECT_EQ(update.update.to_create[0].id, "iv_module_instance:dsp:instance:1");
    EXPECT_EQ(
        update.update.to_create[0].depends_on,
        (std::vector<std::string> {
            "timeline:lane:4",
            "timeline:lane:8",
        }));
}

TEST(IvModuleInstancesExecution, RuntimeRouteChangesUpdateDependenciesWithoutReloadingExecutor)
{
    iv::IvModuleInstancesExecution execution;
    auto instance = make_instance("instance:1");
    NoopNode root;
    (void)execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged{
            .created = {iv::IvModuleInstanceBuilderRef{
                .instance = &instance,
                .root = iv::WeakTypeErasedNode(root),
            }},
        });

    auto const update = execution.handle_runtime_dependencies_changed(
        iv::GraphInputLanesRuntimeDependenciesChanged{
            .version_index = 7,
            .instances = {iv::GraphInputLanesRuntimeDependency{
                .instance_id = "instance:1",
                .prerequisite_lanes = {iv::LaneId{3}, iv::LaneId{9}},
            }},
        });

    EXPECT_EQ(update.version_index, 7u);
    ASSERT_EQ(update.update.to_update.size(), 1u);
    EXPECT_EQ(
        *update.update.to_update.front().depends_on,
        (std::vector<std::string>{"timeline:lane:3", "timeline:lane:9"}));
    EXPECT_FALSE(update.update.to_update.front().callback.has_value());
}

TEST(IvModuleInstancesExecution, DeletingInstanceDeletesTask)
{
    iv::IvModuleInstancesExecution execution;
    auto instance = make_instance("instance:1");
    NoopNode root;

    (void)execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .created = {
                iv::IvModuleInstanceBuilderRef {
                    .instance = &instance,
                    .root = iv::WeakTypeErasedNode(root),
                },
            },
        });

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .deleted_instance_ids = { "instance:1" },
        });

    ASSERT_EQ(update.update.to_delete.size(), 1u);
    EXPECT_EQ(update.update.to_delete[0], "iv_module_instance:dsp:instance:1");
}

TEST(IvModuleInstancesExecution, TaskCallbackTicksTheRoot)
{
    iv::IvModuleInstancesExecution execution(8);
    auto instance = make_instance("instance:1");
    int ticks = 0;
    CountingNode root {&ticks};

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .created = {
                iv::IvModuleInstanceBuilderRef {
                    .instance = &instance,
                    .root = iv::WeakTypeErasedNode(root),
                },
            },
        });

    ASSERT_EQ(update.update.to_create.size(), 1u);
    auto const callback = update.update.to_create[0].callback;
    ASSERT_NE(callback.invoke, nullptr);
    callback.invoke(callback.context);
    callback.invoke(callback.context);

    EXPECT_EQ(ticks, 2);
}

TEST(IvModuleInstancesExecution, MaterializesLiveSampleBindingsIntoExecutorStorage)
{
    RuntimeBindingExecutionWitness witness;
    RuntimeBindingWitnessScope witness_scope(witness);

    iv::IvModuleInstancesExecution execution(8);
    auto instance = make_instance("instance:runtime-sample");
    instance.runtime_bindings = make_test_runtime_bindings();
    auto input = instance.runtime_bindings->sample_input(
        iv::runtime_public_port_key(true, iv::PortKind::sample, 0));
    input->mode = iv::RuntimeSampleInputMode::scalar;
    input->value = iv::Sample{0.25f};
    auto output = instance.runtime_bindings->output(
        iv::runtime_public_port_key(false, iv::PortKind::sample, 0));
    output->target_lane = iv::LaneId{41};

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged{
            .created = {iv::IvModuleInstanceBuilderRef{
                .instance = &instance,
                .root = iv::WeakTypeErasedNode(runtime_sample_binding_root),
            }},
        });
    auto const callback = update.update.to_create.front().callback;
    callback.invoke(callback.context);

    ASSERT_EQ(witness.sample_publications.size(), 1u);
    EXPECT_EQ(witness.sample_publications[0].lane, iv::LaneId{41});
    EXPECT_EQ(witness.sample_publications[0].frame_count, 8u);
    EXPECT_EQ(
        witness.sample_publications[0].samples,
        std::vector<iv::Sample>(8, iv::Sample{0.25f}));

    input->value = iv::Sample{0.75f};
    output->target_lane = iv::LaneId{42};
    callback.invoke(callback.context);

    ASSERT_EQ(witness.sample_publications.size(), 2u);
    EXPECT_EQ(witness.sample_publications[1].lane, iv::LaneId{42});
    EXPECT_EQ(
        witness.sample_publications[1].samples,
        std::vector<iv::Sample>(8, iv::Sample{0.75f}));
}

TEST(IvModuleInstancesExecution, MaterializesLiveEventBindingsIntoExecutorStorage)
{
    RuntimeBindingExecutionWitness witness;
    witness.event_input = {
        iv::TimedEvent{.time = 1, .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = 5, .value = iv::TriggerEvent{}},
    };
    RuntimeBindingWitnessScope witness_scope(witness);

    iv::IvModuleInstancesExecution execution(8);
    auto instance = make_instance("instance:runtime-event");
    instance.runtime_bindings = make_test_runtime_bindings();
    auto input = instance.runtime_bindings->event_input(
        iv::runtime_public_port_key(true, iv::PortKind::event, 0));
    input->timeline_lane = iv::LaneId{51};
    auto output = instance.runtime_bindings->output(
        iv::runtime_public_port_key(false, iv::PortKind::event, 0));
    output->target_lane = iv::LaneId{52};

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged{
            .created = {iv::IvModuleInstanceBuilderRef{
                .instance = &instance,
                .root = iv::WeakTypeErasedNode(runtime_event_binding_root),
            }},
        });
    auto const callback = update.update.to_create.front().callback;
    callback.invoke(callback.context);

    ASSERT_EQ(witness.event_publications.size(), 1u);
    EXPECT_EQ(witness.event_lanes.front(), iv::LaneId{52});
    ASSERT_EQ(witness.event_publications.front().size(), 2u);
    EXPECT_EQ(witness.event_publications.front()[0].time, 1u);
    EXPECT_EQ(witness.event_publications.front()[1].time, 5u);

    witness.event_input = {
        iv::TimedEvent{.time = 11, .value = iv::TriggerEvent{}},
    };
    output->target_lane = iv::LaneId{53};
    callback.invoke(callback.context);

    ASSERT_EQ(witness.event_publications.size(), 2u);
    EXPECT_EQ(witness.event_lanes.back(), iv::LaneId{53});
    ASSERT_EQ(witness.event_publications.back().size(), 1u);
    EXPECT_EQ(witness.event_publications.back().front().time, 11u);
}

TEST(IvModuleInstancesExecution, FamilyBindingsResolveDistinctAbiResources)
{
    static constexpr auto mono_planar = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    static constexpr std::array<iv::StaticRuntimeInputConfig, 2>
        sample_inputs{{
            {.channel_layout = mono_planar},
            {.channel_layout = mono_planar},
        }};
    static constexpr std::array<iv::StaticString, 2> sample_member_ids{{
        {.data = "sample-member-0", .size = 15},
        {.data = "sample-member-1", .size = 15},
    }};
    static constexpr std::array<iv::StaticString, 2> event_member_ids{{
        {.data = "event-member-0", .size = 14},
        {.data = "event-member-1", .size = 14},
    }};
    static constexpr iv::RuntimeSampleOutputFamilyNode sample_family{
        .input_configs = {sample_inputs.data(), sample_inputs.size()},
        .member_binding_ids = {
            sample_member_ids.data(), sample_member_ids.size()},
        .aggregate_binding_id = {
            .data = "sample-aggregate", .size = 16},
        .layout = mono_planar,
    };
    static constexpr iv::RuntimeEventOutputFamilyNode event_family{
        .type = iv::EventTypeId::trigger,
        .member_count = 2,
        .member_binding_ids = {
            event_member_ids.data(), event_member_ids.size()},
        .aggregate_binding_id = {
            .data = "event-aggregate", .size = 15},
    };

    iv::NodeLayoutBuilder builder(8);
    {
        iv::DeclarationContext<iv::RuntimeSampleOutputFamilyNode> context(
            builder, sample_family);
        sample_family.declare(context);
    }
    {
        iv::DeclarationContext<iv::RuntimeEventOutputFamilyNode> context(
            builder, event_family);
        event_family.declare(context);
    }
    iv::GraphRuntimeBindings bindings;
    auto sample_zero = bindings.output("sample-member-0");
    auto sample_one = bindings.output("sample-member-1");
    auto sample_aggregate = bindings.output("sample-aggregate");
    auto event_zero = bindings.output("event-member-0");
    auto event_one = bindings.output("event-member-1");
    auto event_aggregate = bindings.output("event-aggregate");

    auto layout = std::move(builder).build();
    iv::ResourceContext resources;
    resources.runtime_bindings = bindings.resources();
    auto storage = layout.create_storage(resources);
    storage.initialize();

    auto const& sample_state = *static_cast<
        iv::RuntimeSampleOutputFamilyNode::State const*>(storage.state_ptr(0));
    auto const& event_state = *static_cast<
        iv::RuntimeEventOutputFamilyNode::State const*>(storage.state_ptr(1));
    ASSERT_EQ(sample_state.member_bindings.size(), 2u);
    ASSERT_EQ(event_state.member_bindings.size(), 2u);
    EXPECT_EQ(sample_state.member_bindings[0], sample_zero.get());
    EXPECT_EQ(sample_state.member_bindings[1], sample_one.get());
    EXPECT_EQ(sample_state.aggregate_binding.front(), sample_aggregate.get());
    EXPECT_EQ(event_state.member_bindings[0], event_zero.get());
    EXPECT_EQ(event_state.member_bindings[1], event_one.get());
    EXPECT_EQ(event_state.aggregate_binding.front(), event_aggregate.get());
}

TEST(IvModuleInstancesExecution, ConnectionNodeResolvesItsRuntimeInputThroughAbi)
{
    static constexpr iv::ConnectionNode connection{
        .runtime_binding_id = {
            .data = "connection-runtime-input",
            .size = 24,
        },
        .output_channel_count = 1,
    };
    iv::GraphRuntimeBindings bindings;
    auto expected = bindings.sample_input("connection-runtime-input");

    iv::NodeLayoutBuilder builder(8);
    iv::DeclarationContext<iv::ConnectionNode> context(builder, connection);
    connection.declare(context);
    auto layout = std::move(builder).build();
    iv::ResourceContext resources;
    resources.runtime_bindings = bindings.resources();
    auto storage = layout.create_storage(resources);
    storage.initialize();

    auto const& state = *static_cast<iv::ConnectionNode::State const*>(
        storage.state_ptr(0));
    ASSERT_EQ(state.runtime_binding.size(), 1u);
    EXPECT_EQ(state.runtime_binding.front(), expected.get());
}

TEST(IvModuleInstancesExecution, PropagatesConfiguredSampleRateToTickContext)
{
    constexpr size_t sample_rate = 96000;
    iv::IvModuleInstancesExecution execution(8, true, sample_rate);
    auto instance = make_instance("instance:1");
    size_t observed_sample_rate = 0;
    float observed_sample_period = 0.0f;
    SampleRateRecordingNode root {&observed_sample_rate, &observed_sample_period};

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .created = {
                iv::IvModuleInstanceBuilderRef {
                    .instance = &instance,
                    .root = iv::WeakTypeErasedNode(root),
                },
            },
        });

    ASSERT_EQ(update.update.to_create.size(), 1u);
    auto const callback = update.update.to_create[0].callback;
    ASSERT_NE(callback.invoke, nullptr);
    callback.invoke(callback.context);

    EXPECT_EQ(observed_sample_rate, sample_rate);
    EXPECT_FLOAT_EQ(observed_sample_period, 1.0f / static_cast<float>(sample_rate));
}

TEST(IvModuleInstancesExecution, ResumeResetsBlockIndexWhileOngoingTicksKeepAdvancing)
{
    iv::IvModuleInstancesExecution execution(8);
    auto instance = make_instance("instance:1");
    std::vector<size_t> indices;
    IndexRecordingNode root {&indices};

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .created = {
                iv::IvModuleInstanceBuilderRef {
                    .instance = &instance,
                    .root = iv::WeakTypeErasedNode(root),
                },
            },
        });

    ASSERT_EQ(update.update.to_create.size(), 1u);
    auto const callback = update.update.to_create[0].callback;
    ASSERT_NE(callback.invoke, nullptr);

    callback.invoke(callback.context);
    callback.invoke(callback.context);
    execution.resume(64);
    callback.invoke(callback.context);
    callback.invoke(callback.context);

    EXPECT_EQ(indices, (std::vector<size_t>{0u, 8u, 64u, 72u}));
}

TEST(IvModuleInstancesExecution, PausedPreviewIgnoresTransportPlayheadUntilFollowingResumes)
{
    iv::IvModuleInstancesExecution execution(8, false);
    auto instance = make_instance("instance:1");
    std::vector<size_t> indices;
    IndexRecordingNode root {&indices};

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .created = {
                iv::IvModuleInstanceBuilderRef {
                    .instance = &instance,
                    .root = iv::WeakTypeErasedNode(root),
                },
            },
        });
    auto const callback = update.update.to_create[0].callback;
    callback.invoke(callback.context);

    execution.synchronize_transport_playhead(128);
    callback.invoke(callback.context);

    execution.set_follows_transport_playhead(true);
    execution.synchronize_transport_playhead(128);
    callback.invoke(callback.context);

    EXPECT_EQ(indices, (std::vector<size_t>{0u, 8u, 128u}));
}

TEST(IvModuleInstancesExecution, ReloadKeepsOldModuleGenerationAliveThroughExecutorRelease)
{
    iv::IvModuleInstancesExecution execution(8);
    iv::IvModuleInstances instances;
    auto const module_root = std::filesystem::absolute("reload-generation-test");
    auto const definition_id = std::string("iv.test.reload_generation");
    (void)instances.create_instance(definition_id, module_root, "instance:1");
    iv::bind_iv_module_instances_iv_module_instances_execution_bridge(execution);

    bool old_module_is_live = true;
    bool old_release_saw_live_module = false;
    std::vector<iv::ModuleRef> old_module_refs;
    old_module_refs.emplace_back(
        new int(1),
        [&old_module_is_live](void *value) {
            old_module_is_live = false;
            delete static_cast<int *>(value);
        });

    ReleaseRequiresLiveModuleNode old_root {
        &old_module_is_live,
        &old_release_saw_live_module,
    };
    instances.handle_iv_module_definitions_changed(
        iv::IvModuleDefinitionsChanged {
            .created = {
                iv::IvModuleDefinition {
                    .definition_id = definition_id,
                    .module_root = module_root,
                    .module_refs = old_module_refs,
                    .root = iv::WeakTypeErasedNode(old_root),
                },
            },
        });

    // This models the instance registry replacing its module generation before
    // execution releases the old graph.
    old_module_refs.clear();

    NoopNode new_root;
    instances.handle_iv_module_definitions_changed(
        iv::IvModuleDefinitionsChanged {
            .updated = {
                iv::IvModuleDefinition {
                    .definition_id = definition_id,
                    .module_root = module_root,
                    .root = iv::WeakTypeErasedNode(new_root),
                },
            },
        });

    execution.commit_prepared_reloads(1);

    EXPECT_TRUE(old_release_saw_live_module);
    EXPECT_TRUE(old_module_is_live);

    // A later completed task-graph revision proves that the old callback
    // context can no longer be referenced. Reclamation then happens on this
    // control-thread call, not in the after-pass callback.
    execution.commit_prepared_reloads(2);
    (void)execution.handle_instance_builders_changed({});

    iv::unbind_iv_module_instances_iv_module_instances_execution_bridge(execution);

    EXPECT_TRUE(old_release_saw_live_module);
    EXPECT_FALSE(old_module_is_live);
}

TEST(IvModuleInstancesExecution, PreparedReloadKeepsOldRootUntilAfterPassCommit)
{
    iv::IvModuleInstancesExecution execution(8);
    auto instance = make_instance("instance:1");
    int old_ticks = 0;
    CountingNode old_root {&old_ticks};

    auto created = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged{
            .created = {iv::IvModuleInstanceBuilderRef{
                .instance = &instance,
                .root = iv::WeakTypeErasedNode(old_root),
            }},
        });
    auto const old_callback = created.update.to_create.front().callback;
    old_callback.invoke(old_callback.context);

    int new_ticks = 0;
    CountingNode new_root {&new_ticks};
    auto changed = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged{
            .updated = {iv::IvModuleInstanceBuilderRef{
                .instance = &instance,
                .root = iv::WeakTypeErasedNode(new_root),
            }},
        });
    auto const new_callback = *changed.update.to_update.front().callback;

    old_callback.invoke(old_callback.context);
    EXPECT_EQ(old_ticks, 2);
    EXPECT_EQ(new_ticks, 0);

    execution.commit_prepared_reloads(1);
    new_callback.invoke(new_callback.context);

    EXPECT_EQ(old_ticks, 2);
    EXPECT_EQ(new_ticks, 1);
}
