#include "../module_test_utils.h"

#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_instances_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_execution.h>
#include <intravenous/runtime/iv_module_instances_execution_events.h>
#include <intravenous/runtime/iv_module_instances_execution_task_runner_bridge.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/lane_query_schema_service.h>
#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_events.h>
#include <intravenous/runtime/timeline_lane_query_schema_bridge.h>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <thread>

namespace iv {
void publish_iv_module_instances_execution_tasks_for_test(
    VersionedTaskGraphUpdate const &update)
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_instances_execution_tasks_changed_event,
        update);
}
} // namespace iv

namespace {
using iv::test_support::fresh_module_fixture_workspace;
using iv::test_support::make_loaded_definition;
using iv::test_support::read_only_module_fixture_workspace;

bool wait_until(std::function<bool()> const &predicate)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

void noop_task(void *) {}
}

TEST(IntrospectionBridges, DefinitionsToIvModuleSourceIntrospectionRequiresBinding)
{
    auto const workspace =
        fresh_module_fixture_workspace("runtime_bridges_defs_to_introspection_unbound");
    iv::IvModuleDefinitions definitions;
    iv::IvModuleSourceIntrospection introspection;

    definitions.seed_loaded_definition(make_loaded_definition(workspace));
    auto const result = introspection.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {
            iv::SourceRange{
                .start = {.line = 1, .column = 1},
                .end = {.line = 2, .column = 1},
            },
        });
    EXPECT_TRUE(result.nodes.empty());
}

TEST(IntrospectionBridges, DefinitionsToIvModuleSourceIntrospectionForwardsWhenBound)
{
    auto const workspace =
        read_only_module_fixture_workspace("local_cmake");
    iv::IvModuleDefinitions definitions;
    iv::IvModuleSourceIntrospection introspection;
    auto bridge_scope =
        iv::iv_module_definitions_iv_module_source_introspection_bridge::bind(
            definitions,
            introspection);

    auto const startup = iv::StartupConfig(workspace, iv::test::repo_root(), {}).initialize();
    auto loaded = iv::test::load_runtime_iv_module_definition(
        startup,
        std::filesystem::weakly_canonical(workspace));
    definitions.seed_loaded_definition(iv::IvModuleReloadedDefinition{
        .definition_id = loaded.definition_id,
        .module_root = loaded.module_root,
        .module_id = loaded.module_id,
        .introspection = loaded.introspection,
        .dependencies = loaded.dependencies,
        .module_refs = std::move(loaded.module_refs),
        .root = loaded.root,
    });
    auto const result = introspection.query_active_regions(
        std::filesystem::weakly_canonical(workspace / "module.cpp"));

    EXPECT_FALSE(result.source_spans.empty());

}

TEST(IntrospectionBridges, InstancesToDefinitionsRequiresBinding)
{
    auto const workspace =
        fresh_module_fixture_workspace("runtime_bridges_instances_to_definitions_unbound");

    iv::IvModuleInstances instances;
    iv::IvModuleDefinitions definitions;

    (void)instances.create_instance(
        "iv.test.runtime_module_bridges",
        std::filesystem::weakly_canonical(workspace));

    EXPECT_TRUE(definitions.loaded_definitions().empty());
}

TEST(ExecutionTaskRunnerBridge, ReleasesDeferredGraphsOnTheRunnersOwnAfterPass)
{
    iv::IvModuleInstancesExecution execution;
    iv::TasksRunner runner(1);
    auto bridge_scope =
        iv::iv_module_instances_execution_task_runner_bridge::bind(execution, runner);

    iv::publish_iv_module_instances_execution_tasks_for_test(
        iv::VersionedTaskGraphUpdate{
            .version_index = 1,
            .update = iv::TaskGraphUpdate{
                .to_create = {iv::TaskRecord{
                    .id = "test",
                    .callback = {.invoke = &noop_task},
                }},
            },
            .activation_deferred = true,
        });

    EXPECT_TRUE(wait_until([&] { return runner.active_graph_revision() == 1; }));
}

TEST(TimelineLaneQuerySchemaBridge, UsesTheSchemaSnapshotPublishedByTimeline)
{
    iv::Timeline timeline;
    iv::LaneQuerySchemaService service;
    service.initialize({});
    auto bridge_scope =
        iv::timeline_lane_query_schema_bridge::bind(timeline, service);

    auto const schema = iv::query::LaneQuerySchema::from_entries(
        {{"gain", iv::query::LaneQueryValueType::float_}}, 1);
    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_timeline_lanes_changed_event,
        iv::TimelineLanesChanged{
            .schema_change = {
                .changed = true,
                .old_revision = 0,
                .new_revision = 1,
            },
            .schema = schema,
        });

    auto const snapshot = service.snapshot();
    EXPECT_EQ(snapshot.revision(), 1u);
    ASSERT_TRUE(snapshot.find("gain").has_value());
    EXPECT_EQ(
        snapshot.type_of(*snapshot.find("gain")),
        iv::query::LaneQueryValueType::float_);
}
