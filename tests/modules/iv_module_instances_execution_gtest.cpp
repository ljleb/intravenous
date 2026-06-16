#include <intravenous/runtime/iv_module_instances_execution.h>

#include <gtest/gtest.h>

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

TEST(IvModuleInstancesExecution, GraphInputLaneDependenciesUpdateTaskPrerequisites)
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

    auto update = execution.handle_timeline_batch(
        iv::TimelineLaneBatchUpdate {
            .task_dependencies_created_or_updated = {
                iv::TimelineTaskDependenciesUpdate {
                    .task_id = "iv_module_instance:dsp:instance:1",
                    .depends_on = {
                        "timeline:lane:4",
                        "timeline:lane:8",
                    },
                },
            },
        });

    ASSERT_EQ(update.update.to_update.size(), 1u);
    EXPECT_EQ(update.update.to_update[0].id, "iv_module_instance:dsp:instance:1");
    ASSERT_TRUE(update.update.to_update[0].depends_on.has_value());
    EXPECT_EQ(
        *update.update.to_update[0].depends_on,
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
