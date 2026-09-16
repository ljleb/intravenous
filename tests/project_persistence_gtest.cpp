#include "module_test_utils.h"

#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/project_persistence.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/project_persistence_node_instances_bridge.h>
#include <intravenous/runtime/project_persistence_project_graph_bridge.h>
#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/project_graph_node_instances_bridge.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {
using Json = nlohmann::ordered_json;

std::filesystem::path fresh_workspace(std::string_view name)
{
    return iv::test::fresh_module_fixture_workspace(name);
}

void write_text(std::filesystem::path const &path, std::string_view text)
{
    std::ofstream out(path, std::ios::trunc);
    ASSERT_TRUE(out);
    out << text;
}

std::vector<Json> parse_project_file(std::filesystem::path const &path)
{
    std::ifstream in(path);
    EXPECT_TRUE(in);
    std::vector<Json> commands;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) commands.push_back(Json::parse(line));
    }
    return commands;
}

iv::StartupConfigState startup_for(std::filesystem::path const &workspace)
{
    iv::StartupConfigState startup;
    startup.workspace_root = workspace;
    startup.discovery_start = workspace;
    startup.output_device_id = "default";
    startup.input_device_id = "default";
    return startup;
}

struct ProjectNotificationWitness {
    std::vector<iv::ProjectMessageNotification> messages {};

    void handle_notification(iv::ProjectNotification const &notification)
    {
        if (auto const *message = std::get_if<iv::ProjectMessageNotification>(&notification)) {
            messages.push_back(*message);
        }
    }
};

struct ProjectCollectStateContributor {
    bool fail = false;

    void handle_collect_state(iv::ProjectPersistenceBuilder &)
    {
        if (fail) throw std::runtime_error("collect-state failure");
    }
};

using namespace iv;
IV_DECLARE_BRIDGE(
    project_notification_witness_bridge,
    iv::ProjectPersistence,
    ProjectNotificationWitness);
IV_DECLARE_BRIDGE(
    project_collect_state_contributor_bridge,
    iv::ProjectPersistence,
    ProjectCollectStateContributor);
IV_DEFINE_BRIDGE(project_notification_witness_bridge)
IV_DEFINE_BRIDGE(project_collect_state_contributor_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    project_notification_witness_bridge,
    iv_runtime_project_notification_event,
    &ProjectNotificationWitness::handle_notification)
IV_SUBSCRIBE_LINKER_EVENT(
    project_collect_state_contributor_bridge,
    iv_runtime_project_persistence_collect_state_event,
    &ProjectCollectStateContributor::handle_collect_state)
} // namespace

TEST(ProjectPersistenceBuilder, SerializesOnlyDurableConfiguredStateInStableOrder)
{
    auto const workspace = fresh_workspace("project_persistence_builder_durable_state");
    auto startup = startup_for(workspace);
    startup.toolchain.cmake_generator = "Ninja";

    iv::ProjectPersistenceBuilder builder(workspace, startup);
    auto toolchain = startup.toolchain;
    toolchain.cmake_generator = "Unix Makefiles";
    toolchain.c_compiler = workspace / "toolchain" / "clang";
    builder.add_project_toolchain_config(toolchain);
    builder.add_project_audio_device_selection("out-1", std::nullopt);
    builder.add_iv_module_instances({
        iv::IvModuleInstanceInfo{
            .instance_id = "instance:b",
            .definition_id = "iv.test.b",
            .display_name = "B",
            .package_root = workspace / "modules" / "b",
        },
        iv::IvModuleInstanceInfo{
            .instance_id = "instance:a",
            .definition_id = "iv.test.a",
            .display_name = "iv.test.a",
            .package_root = workspace / "modules" / "a",
        },
    });

    auto const commands = builder.build();
    ASSERT_EQ(commands.size(), 3u);
    EXPECT_EQ(commands[0].command, "project.overrideSettings");
    EXPECT_EQ(commands[0].args["cmake_generator"], "Unix Makefiles");
    EXPECT_EQ(commands[0].args["c_compiler"], "toolchain/clang");
    EXPECT_EQ(commands[0].args["output_device_id"], "out-1");
    EXPECT_TRUE(commands[0].args["input_device_id"].is_null());

    EXPECT_EQ(commands[1].command, "ivModuleInstances.create");
    EXPECT_EQ(commands[1].args["instance_id"], "instance:a");
    EXPECT_EQ(commands[1].args["module_id"], "iv.test.a");
    EXPECT_EQ(commands[1].args["package_root"], "modules/a");
    EXPECT_TRUE(commands[1].args["display_name"].is_null());

    EXPECT_EQ(commands[2].command, "ivModuleInstances.create");
    EXPECT_EQ(commands[2].args["instance_id"], "instance:b");
    EXPECT_EQ(commands[2].args["display_name"], "B");

    for (auto const &command : commands) {
        EXPECT_FALSE(command.args.contains("lane_id"));
        EXPECT_FALSE(command.args.contains("graph_input"));
        EXPECT_FALSE(command.args.contains("compiled_sample_cache_chunk_size_multiplier"));
    }
}

TEST(ProjectPersistenceBuilder, OmitsSettingsThatMatchStartupDefaults)
{
    auto const workspace = fresh_workspace("project_persistence_builder_defaults");
    auto startup = startup_for(workspace);
    startup.toolchain.cmake_generator = "Ninja";

    iv::ProjectPersistenceBuilder builder(workspace, startup);
    builder.add_project_toolchain_config(startup.toolchain);
    builder.add_project_audio_device_selection(startup.output_device_id, startup.input_device_id);

    EXPECT_TRUE(builder.build().empty());
}

TEST(ProjectPersistence, LoadContinuesAfterBadCommandAndReplaysLaterInstance)
{
    auto const workspace = fresh_workspace("project_persistence_best_effort_replay");
    auto startup = startup_for(workspace);
    write_text(
        workspace / "iv_project.jsonl",
        R"({"command":"removed.timelineCommand","args":{}})" "\n"
        R"({"command":"ivModuleInstances.create","args":{"instance_id":"instance:1","module_id":"iv.test.replayed","package_root":"modules/replayed","display_name":"Replayed"}})" "\n");

    iv::ProjectPersistence persistence(workspace, startup);
    iv::NodeInstances instances;
    iv::ProjectGraph project_graph;
    ProjectNotificationWitness witness;
    auto project_graph_scope =
        iv::project_persistence_project_graph_bridge::bind(persistence, project_graph);
    auto project_graph_instances_scope =
        iv::project_graph_node_instances_bridge::bind(project_graph, instances);
    auto instances_scope =
        iv::project_persistence_node_instances_bridge::bind(persistence, instances);
    auto witness_scope = project_notification_witness_bridge::bind(persistence, witness);

    persistence.load();

    auto const listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().instance_id, "instance:1");
    EXPECT_EQ(listed.front().definition_id, "iv.test.replayed");
    EXPECT_EQ(listed.front().display_name, "Replayed");
    EXPECT_TRUE(std::ranges::any_of(witness.messages, [](auto const &message) {
        return message.level == "error"
            && message.message.contains("unknown project command: removed.timelineCommand");
    }));
}

TEST(ProjectPersistence, SaveCollectsInstanceMetadataWithoutExecutionState)
{
    auto const workspace = fresh_workspace("project_persistence_instance_save");
    auto startup = startup_for(workspace);
    iv::ProjectPersistence persistence(workspace, startup);
    iv::NodeInstances instances;
    auto instances_scope =
        iv::project_persistence_node_instances_bridge::bind(persistence, instances);

    (void)instances.create_instance(
        "iv.test.persisted",
        workspace / "modules" / "persisted",
        "instance:stable",
        "Persisted");

    persistence.save();

    auto const commands = parse_project_file(workspace / "iv_project.jsonl");
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0]["command"], "ivModuleInstances.create");
    EXPECT_EQ(commands[0]["args"]["instance_id"], "instance:stable");
    EXPECT_EQ(commands[0]["args"]["module_id"], "iv.test.persisted");
    EXPECT_EQ(commands[0]["args"]["package_root"], "modules/persisted");
    EXPECT_EQ(commands[0]["args"]["display_name"], "Persisted");
    EXPECT_EQ(commands[0]["args"].size(), 4u);
}

TEST(ProjectPersistence, SaveWithNoContributorsWritesEmptyProjectFile)
{
    auto const workspace = fresh_workspace("project_persistence_empty_save");
    iv::ProjectPersistence persistence(workspace, startup_for(workspace));

    persistence.save();

    EXPECT_TRUE(parse_project_file(workspace / "iv_project.jsonl").empty());
}

TEST(ProjectPersistence, CollectStateFailureDoesNotReplaceExistingProjectFile)
{
    auto const workspace = fresh_workspace("project_persistence_atomic_failure");
    auto const project_file = workspace / "iv_project.jsonl";
    write_text(project_file, "sentinel\n");

    iv::ProjectPersistence persistence(workspace, startup_for(workspace));
    ProjectCollectStateContributor contributor{.fail = true};
    auto contributor_scope =
        project_collect_state_contributor_bridge::bind(persistence, contributor);

    EXPECT_THROW(persistence.save(), std::runtime_error);

    std::ifstream in(project_file);
    ASSERT_TRUE(in);
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(contents, "sentinel\n");
}
