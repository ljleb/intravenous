#include "../module_test_utils.h"

#include <intravenous/linker_event.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_events.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {
struct IvModuleInstancesWitness {
    std::optional<iv::IvModuleRequiredDefinitionsChanged> required_diff {};
    std::optional<iv::IvModuleInstancesChanged> instances_diff {};
    std::optional<iv::IvModuleInstanceBuildersChanged> builders_completed {};
    std::optional<std::vector<iv::IvModuleInstanceInfo>> listed_instances {};

    void reset()
    {
        required_diff.reset();
        instances_diff.reset();
        builders_completed.reset();
        listed_instances.reset();
    }
};

IvModuleInstancesWitness *g_iv_module_instances_witness = nullptr;

iv::IvModuleDefinition make_definition(std::filesystem::path module_root)
{
    auto const normalized = std::filesystem::weakly_canonical(module_root).lexically_normal();
    return iv::IvModuleDefinition{
        .definition_id = normalized.string(),
        .module_root = normalized,
        .module_id = "iv.test.module",
    };
}

IV_SUBSCRIBE_LINKER_EVENT(
    iv::IvModuleRequiredDefinitionsChangedEvent,
    iv_runtime_iv_module_required_definitions_changed_event,
    +[](iv::IvModuleRequiredDefinitionsChanged const &diff) {
        if (g_iv_module_instances_witness != nullptr) {
            g_iv_module_instances_witness->required_diff = diff;
        }
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::IvModuleInstancesChangedEvent,
    iv_runtime_iv_module_instances_changed_event,
    +[](iv::IvModuleInstancesChanged const &diff) {
        if (g_iv_module_instances_witness != nullptr) {
            g_iv_module_instances_witness->instances_diff = diff;
        }
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::IvModuleInstancesListChangedEvent,
    iv_runtime_iv_module_instances_list_changed_event,
    +[](std::vector<iv::IvModuleInstanceInfo> const &instances) {
        if (g_iv_module_instances_witness != nullptr) {
            g_iv_module_instances_witness->listed_instances = instances;
        }
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::IvModuleInstanceBuildersCompletedEvent,
    iv_runtime_iv_module_instance_builders_completed_event,
    +[](iv::IvModuleInstanceBuildersChanged const &changed) {
        if (g_iv_module_instances_witness != nullptr) {
            g_iv_module_instances_witness->builders_completed = changed;
        }
    });

class IvModuleInstancesTest : public ::testing::Test {
protected:
    IvModuleInstancesWitness witness {};

    void SetUp() override
    {
        g_iv_module_instances_witness = &witness;
    }

    void TearDown() override
    {
        g_iv_module_instances_witness = nullptr;
    }
};
} // namespace

TEST_F(IvModuleInstancesTest, CreateInstancePublishesRequiredDefinitionAndListChange)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_create");
    auto const module_root = std::filesystem::weakly_canonical(workspace);
    iv::IvModuleInstances instances;

    auto const instance_id = instances.create_instance(module_root);

    EXPECT_FALSE(instance_id.empty());
    ASSERT_TRUE(witness.required_diff.has_value());
    ASSERT_EQ(witness.required_diff->created.size(), 1u);
    EXPECT_EQ(witness.required_diff->created.front().definition_id, module_root.string());
    EXPECT_EQ(witness.required_diff->created.front().module_root, module_root);

    ASSERT_TRUE(witness.listed_instances.has_value());
    ASSERT_EQ(witness.listed_instances->size(), 1u);
    EXPECT_EQ(witness.listed_instances->front().instance_id, instance_id);
    EXPECT_EQ(witness.listed_instances->front().definition_id, module_root.string());
    EXPECT_FALSE(witness.listed_instances->front().realized);
}

TEST_F(IvModuleInstancesTest, CreateSecondInstanceForSameDefinitionRepublishesLoadedDefinition)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_dedup_required");
    auto const module_root = std::filesystem::weakly_canonical(workspace);
    iv::IvModuleInstances instances;

    (void)instances.create_instance(module_root);
    witness.reset();

    auto const second_instance_id = instances.create_instance(module_root);

    EXPECT_FALSE(second_instance_id.empty());
    ASSERT_TRUE(witness.required_diff.has_value());
    ASSERT_EQ(witness.required_diff->updated.size(), 1u);
    EXPECT_EQ(witness.required_diff->updated.front().definition_id, module_root.string());
    EXPECT_EQ(witness.required_diff->updated.front().module_root, module_root);
    ASSERT_TRUE(witness.listed_instances.has_value());
    ASSERT_EQ(witness.listed_instances->size(), 2u);
}

TEST_F(IvModuleInstancesTest, DefinitionsChangedRealizesMatchingInstancesAndPublishesDiff)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_realize");
    auto const module_root = std::filesystem::weakly_canonical(workspace);
    iv::IvModuleInstances instances;

    auto const instance_id = instances.create_instance(module_root);
    witness.reset();

    instances.handle_iv_module_definitions_changed(iv::IvModuleDefinitionsChanged{
        .created = {make_definition(module_root)},
    });

    ASSERT_TRUE(witness.instances_diff.has_value());
    ASSERT_EQ(witness.instances_diff->created.size(), 1u);
    EXPECT_EQ(witness.instances_diff->created.front().instance_id, instance_id);
    EXPECT_EQ(witness.instances_diff->created.front().definition_id, module_root.string());
    EXPECT_EQ(witness.instances_diff->created.front().module_id, "iv.test.module");

    ASSERT_TRUE(witness.listed_instances.has_value());
    ASSERT_EQ(witness.listed_instances->size(), 1u);
    EXPECT_TRUE(witness.listed_instances->front().realized);
    EXPECT_EQ(witness.listed_instances->front().module_id, "iv.test.module");
    EXPECT_FALSE(
        witness.listed_instances->front().default_silence_ttl_samples.has_value());
}

TEST_F(IvModuleInstancesTest, SettingPerInstanceDefaultSilenceTtlRepublishesRealizedBuilder)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_set_ttl");
    auto const module_root = std::filesystem::weakly_canonical(workspace);
    iv::IvModuleInstances instances;

    auto const instance_id = instances.create_instance(module_root);
    instances.handle_iv_module_definitions_changed(iv::IvModuleDefinitionsChanged{
        .created = {make_definition(module_root)},
    });
    witness.reset();

    instances.set_default_silence_ttl_samples(instance_id, 8192);

    ASSERT_TRUE(witness.builders_completed.has_value());
    ASSERT_EQ(witness.builders_completed->updated.size(), 1u);
    ASSERT_NE(witness.builders_completed->updated.front().instance, nullptr);
    EXPECT_EQ(
        witness.builders_completed->updated.front().instance->instance_id,
        instance_id);
    ASSERT_TRUE(
        witness.builders_completed->updated.front().default_silence_ttl_samples.has_value());
    EXPECT_EQ(
        *witness.builders_completed->updated.front().default_silence_ttl_samples,
        8192u);

    auto const listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    ASSERT_TRUE(listed.front().default_silence_ttl_samples.has_value());
    EXPECT_EQ(*listed.front().default_silence_ttl_samples, 8192u);
}

TEST_F(IvModuleInstancesTest, RemoveLastRealizedInstancePublishesDeleteAndDropsRequirement)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_remove");
    auto const module_root = std::filesystem::weakly_canonical(workspace);
    iv::IvModuleInstances instances;

    auto const instance_id = instances.create_instance(module_root);
    instances.handle_iv_module_definitions_changed(iv::IvModuleDefinitionsChanged{
        .created = {make_definition(module_root)},
    });
    witness.reset();

    instances.remove_instance(instance_id);

    ASSERT_TRUE(witness.instances_diff.has_value());
    ASSERT_EQ(witness.instances_diff->deleted_instance_ids.size(), 1u);
    EXPECT_EQ(witness.instances_diff->deleted_instance_ids.front(), instance_id);

    ASSERT_TRUE(witness.required_diff.has_value());
    ASSERT_EQ(witness.required_diff->deleted_definition_ids.size(), 1u);
    EXPECT_EQ(witness.required_diff->deleted_definition_ids.front(), module_root.string());

    ASSERT_TRUE(witness.listed_instances.has_value());
    EXPECT_TRUE(witness.listed_instances->empty());
}
