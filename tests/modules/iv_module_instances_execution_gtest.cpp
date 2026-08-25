#include <intravenous/runtime/iv_module_instances_execution.h>
#include <intravenous/runtime/iv_module_instances_iv_module_instances_execution_bridge.h>

#include <gtest/gtest.h>

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
