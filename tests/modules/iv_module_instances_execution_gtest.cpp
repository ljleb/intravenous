#include <intravenous/runtime/iv_module_instances_execution.h>
#include <intravenous/runtime/iv_module_instances_iv_module_instances_execution_bridge.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

namespace {
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

TEST(IvModuleInstancesExecution, CreatesOneTaskPerBuilderInstance)
{
    iv::IvModuleInstancesExecution execution;
    auto instance = make_instance("instance:1");
    iv::GraphBuilder builder;
    builder.outputs();

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .created = {
                iv::IvModuleInstanceBuilderRef {
                    .instance = &instance,
                    .builder = &builder,
                },
            },
        });

    ASSERT_EQ(update.update.to_create.size(), 1u);
    EXPECT_EQ(update.update.to_create[0].id, "iv_module_instance:dsp:instance:1");
    EXPECT_TRUE(update.update.to_create[0].depends_on.empty());
    EXPECT_TRUE(update.update.to_delete.empty());
}

TEST(IvModuleInstancesExecution, BuilderPrerequisiteLanesBecomeTaskDependencies)
{
    iv::IvModuleInstancesExecution execution;
    auto instance = make_instance("instance:1");
    iv::GraphBuilder builder;
    builder.outputs();

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .created = {
                iv::IvModuleInstanceBuilderRef {
                    .instance = &instance,
                    .builder = &builder,
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

TEST(IvModuleInstancesExecution, DeletingInstanceDeletesTask)
{
    iv::IvModuleInstancesExecution execution;
    auto instance = make_instance("instance:1");
    iv::GraphBuilder builder;
    builder.outputs();

    (void)execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .created = {
                iv::IvModuleInstanceBuilderRef {
                    .instance = &instance,
                    .builder = &builder,
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

TEST(IvModuleInstancesExecution, TaskCallbackTicksTheBuiltGraph)
{
    iv::IvModuleInstancesExecution execution(8);
    auto instance = make_instance("instance:1");
    iv::GraphBuilder builder;
    int ticks = 0;
    (void)builder.node<CountingNode>(&ticks);
    builder.outputs();

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .created = {
                iv::IvModuleInstanceBuilderRef {
                    .instance = &instance,
                    .builder = &builder,
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

TEST(IvModuleInstancesExecution, ResumeResetsBlockIndexWhileOngoingTicksKeepAdvancing)
{
    iv::IvModuleInstancesExecution execution(8);
    auto instance = make_instance("instance:1");
    iv::GraphBuilder builder;
    std::vector<size_t> indices;
    (void)builder.node<IndexRecordingNode>(&indices);
    builder.outputs();

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .created = {
                iv::IvModuleInstanceBuilderRef {
                    .instance = &instance,
                    .builder = &builder,
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
    iv::GraphBuilder builder;
    std::vector<size_t> indices;
    (void)builder.node<IndexRecordingNode>(&indices);
    builder.outputs();

    auto update = execution.handle_instance_builders_changed(
        iv::IvModuleInstanceBuildersChanged {
            .created = {
                iv::IvModuleInstanceBuilderRef {
                    .instance = &instance,
                    .builder = &builder,
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

    iv::GraphBuilder old_builder;
    (void)old_builder.node<ReleaseRequiresLiveModuleNode>(
        &old_module_is_live,
        &old_release_saw_live_module);
    old_builder.outputs();
    instances.handle_iv_module_definitions_changed(
        iv::IvModuleDefinitionsChanged {
            .created = {
                iv::IvModuleDefinition {
                    .definition_id = definition_id,
                    .module_root = module_root,
                    .module_refs = old_module_refs,
                    .canonical_builder = &old_builder,
                },
            },
        });

    // This models the instance registry replacing its module generation before
    // execution releases the old graph.
    old_module_refs.clear();

    iv::GraphBuilder new_builder;
    new_builder.outputs();
    instances.handle_iv_module_definitions_changed(
        iv::IvModuleDefinitionsChanged {
            .updated = {
                iv::IvModuleDefinition {
                    .definition_id = definition_id,
                    .module_root = module_root,
                    .canonical_builder = &new_builder,
                },
            },
        });

    iv::unbind_iv_module_instances_iv_module_instances_execution_bridge(execution);

    EXPECT_TRUE(old_release_saw_live_module);
    EXPECT_FALSE(old_module_is_live);
}
