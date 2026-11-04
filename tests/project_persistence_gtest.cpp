#include "module_test_utils.h"

#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/project_connection_json.h>
#include <intravenous/runtime/project_persistence.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/project_persistence_node_instances_bridge.h>
#include <intravenous/runtime/project_persistence_graph_connections_bridge.h>
#include <intravenous/runtime/project_persistence_project_graph_bridge.h>
#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/project_graph_graph_connections_bridge.h>
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
#include <unordered_map>
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
    builder.add_node_instances({
        iv::ProjectNodeInstanceDeclaration{
            .instance_id = "instance:b",
            .definition_id = "iv.test.b",
            .package_root = workspace / "modules" / "b",
            .display_name = "B",
            .configuration_source = "{.gain = 0.25}",
        },
        iv::ProjectNodeInstanceDeclaration{
            .instance_id = "instance:a",
            .definition_id = "iv.test.a",
            .package_root = workspace / "modules" / "a",
        },
    });
    builder.add_graph_connections({iv::ProjectSampleConnection{
        .connection_id = "connection:main",
        .source_type = iv::ChannelTypeId::stereo,
        .outputs = {{
            .instance_id = "instance:a",
            .path = {
                iv::ProjectVirtualNodeSelector{
                    .source_identity = "processor",
                    .type_identity = "filter",
                },
                iv::ProjectVirtualMemberSelector{.index = 1},
                iv::ProjectTiledChildSelector{.name = "right"},
            },
            .port = {.name = "main", .index = 2, .channel = 1},
        }},
        .target_type = iv::ChannelTypeId::mono,
        .inputs = {{
            .instance_id = "instance:b",
            .port = {.name = "main"},
        }},
    }});

    auto const commands = builder.build();
    ASSERT_EQ(commands.size(), 4u);
    EXPECT_EQ(commands[0].command, "project.overrideSettings");
    EXPECT_EQ(commands[0].args["cmake_generator"], "Unix Makefiles");
    EXPECT_EQ(commands[0].args["c_compiler"], "toolchain/clang");
    EXPECT_EQ(commands[0].args["output_device_id"], "out-1");
    EXPECT_TRUE(commands[0].args["input_device_id"].is_null());

    EXPECT_EQ(commands[1].command, "nodeInstances.upsert");
    EXPECT_EQ(commands[1].args["instance_id"], "instance:a");
    EXPECT_EQ(commands[1].args["definition_id"], "iv.test.a");
    EXPECT_EQ(commands[1].args["package_root"], "modules/a");
    EXPECT_TRUE(commands[1].args["display_name"].is_null());
    EXPECT_EQ(commands[1].args["configuration_source"], "");

    EXPECT_EQ(commands[2].command, "nodeInstances.upsert");
    EXPECT_EQ(commands[2].args["instance_id"], "instance:b");
    EXPECT_EQ(commands[2].args["display_name"], "B");
    EXPECT_EQ(commands[2].args["configuration_source"], "{.gain = 0.25}");

    EXPECT_EQ(commands[3].command, "graphConnections.upsert");
    EXPECT_EQ(commands[3].args["kind"], "sample");
    EXPECT_EQ(commands[3].args["connection_id"], "connection:main");
    EXPECT_EQ(commands[3].args["source_type"], "stereo");
    EXPECT_EQ(commands[3].args["target_type"], "mono");
    auto const& output = commands[3].args["outputs"][0];
    EXPECT_EQ(output["instance_id"], "instance:a");
    ASSERT_EQ(output["path"].size(), 3u);
    EXPECT_EQ(output["path"][0]["kind"], "virtualNode");
    EXPECT_EQ(output["path"][0]["source_identity"], "processor");
    EXPECT_EQ(output["path"][0]["type_identity"], "filter");
    EXPECT_EQ(output["path"][1]["kind"], "virtualMember");
    EXPECT_EQ(output["path"][1]["index"], 1u);
    EXPECT_EQ(output["path"][2]["kind"], "tiledChild");
    EXPECT_EQ(output["path"][2]["name"], "right");
    EXPECT_EQ(output["port"]["channel"], 1u);

    for (auto const &command : commands) {
        EXPECT_FALSE(command.args.contains("graph_input"));
        EXPECT_FALSE(command.args.contains("compiled_sample_cache_chunk_size_multiplier"));
    }
}

TEST(ProjectConnectionJson, RoundTripsEveryStructuredMatcherFieldInBothNameStyles)
{
    iv::ProjectConnection const original = iv::ProjectSampleConnection{
        .connection_id = "connection:round-trip",
        .source_type = iv::ChannelTypeId::stereo,
        .outputs = {{
            .instance_id = "instance:source",
            .path = {
                iv::ProjectVirtualNodeSelector{
                    .source_identity = "processor",
                    .type_identity = "filter",
                },
                iv::ProjectVirtualMemberSelector{.index = 5},
                iv::ProjectTiledChildSelector{.name = "right"},
            },
            .port = {.name = "audio", .index = 2, .channel = 1},
        }},
        .target_type = iv::ChannelTypeId::mono,
        .inputs = {{
            .instance_id = "instance:target",
            .path = {iv::ProjectVirtualNodeSelector{
                .source_identity = "sink",
            }},
            .port = {.index = 0, .channel = 0},
        }},
    };

    for (auto const names : {
             iv::ProjectConnectionJsonNames::project_file,
             iv::ProjectConnectionJsonNames::socket_rpc,
         }) {
        auto const json = iv::project_connection_json(original, names);
        EXPECT_EQ(iv::parse_project_connection_json(json, names), original);
    }
}

TEST(ProjectPersistenceBuilder, StoresReplacementArgumentsAsStructuredJson)
{
    auto const workspace = fresh_workspace(
        "project_persistence_builder_node_replacement");
    iv::ProjectPersistenceBuilder builder(workspace, startup_for(workspace));
    builder.add_node_replacements({iv::ProjectNodeReplacement{
        .address = {
            .instance_id = "instance:source",
            .path = {iv::ProjectVirtualNodeSelector{
                .source_identity = "processor",
            }},
        },
        .definition_id = "iv.test.successor",
        .package_root = workspace / "modules" / "successor",
        .configuration_arguments_json =
            R"([17,{"mode":"highpass"},[1,2,3]])",
    }});

    auto const commands = builder.build();
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].command, "nodeReplacements.upsert");
    EXPECT_EQ(commands[0].args["definition_id"], "iv.test.successor");
    EXPECT_EQ(commands[0].args["package_root"], "modules/successor");
    EXPECT_EQ(
        commands[0].args["configuration_arguments"],
        nlohmann::ordered_json::array({
            17,
            nlohmann::ordered_json{{"mode", "highpass"}},
            nlohmann::ordered_json::array({1, 2, 3}),
        }));
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
        R"({"command":"nodeInstances.upsert","args":{"instance_id":"instance:0","definition_id":"iv.test.legacy","package_root":"modules/legacy","display_name":null}})" "\n"
        R"({"command":"nodeInstances.upsert","args":{"instance_id":"instance:removed","definition_id":"iv.test.removed","package_root":null,"display_name":null,"configuration_source":""}})" "\n"
        R"({"command":"nodeInstances.delete","args":{"instance_id":"instance:removed"}})" "\n"
        R"({"command":"nodeInstances.upsert","args":{"instance_id":"instance:1","definition_id":"iv.test.replayed","package_root":"modules/replayed","display_name":"Replayed","configuration_source":"12 + 4"}})" "\n"
        R"({"command":"nodeReplacements.upsert","args":{"address":{"instance_id":"instance:0","path":[{"kind":"virtualNode","source_identity":"processor"}]},"definition_id":"iv.test.successor","package_root":"modules/successor","configuration_arguments":[17,{"mode":"highpass"}]}})" "\n"
        R"({"command":"graphConnections.upsert","args":{"kind":"event","connection_id":"event:removed","source_type":"trigger","outputs":[{"instance_id":"instance:0","path":[],"port":{"name":"out"}}],"target_type":"empty","inputs":[{"instance_id":"instance:1","path":[],"port":{"index":0}}]}})" "\n"
        R"({"command":"graphConnections.delete","args":{"connection_id":"event:removed"}})" "\n"
        R"({"command":"graphConnections.upsert","args":{"kind":"event","connection_id":"event:dangling","source_type":"trigger","outputs":[{"instance_id":"instance:0","path":[],"port":{"name":"out"}}],"target_type":"empty","inputs":[{"instance_id":"instance:1","path":[],"port":{"index":0}}]}})" "\n"
        R"({"command":"graphConnections.upsert","args":{"kind":"sample","connection_id":"sample:structured","source_type":"stereo","outputs":[{"instance_id":"instance:0","path":[{"kind":"virtualNode","source_identity":"processor","type_identity":"filter"},{"kind":"virtualMember","index":2},{"kind":"tiledChild","name":"left"}],"port":{"name":"audio","index":1,"channel":0}}],"target_type":"mono","inputs":[{"instance_id":"instance:1","path":[],"port":{"name":"audio"}}]}})" "\n");

    iv::ProjectPersistence persistence(workspace, startup);
    iv::NodeInstances instances;
    iv::GraphConnections graph_connections;
    iv::ProjectGraph project_graph;
    ProjectNotificationWitness witness;
    auto project_graph_scope =
        iv::project_persistence_project_graph_bridge::bind(persistence, project_graph);
    auto project_graph_instances_scope =
        iv::project_graph_node_instances_bridge::bind(project_graph, instances);
    auto project_graph_connections_scope =
        iv::project_graph_graph_connections_bridge::bind(project_graph, graph_connections);
    auto instances_scope =
        iv::project_persistence_node_instances_bridge::bind(persistence, instances);
    auto witness_scope = project_notification_witness_bridge::bind(persistence, witness);

    persistence.load();

    auto const listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(listed[0].instance_id, "instance:0");
    EXPECT_EQ(listed[0].definition_id, "iv.test.legacy");
    EXPECT_TRUE(listed[0].configuration_source.empty());
    EXPECT_EQ(listed[1].instance_id, "instance:1");
    EXPECT_EQ(listed[1].definition_id, "iv.test.replayed");
    EXPECT_EQ(listed[1].display_name, "Replayed");
    EXPECT_EQ(listed[1].configuration_source, "12 + 4");
    auto const replacements = instances.desired_node_replacements();
    ASSERT_EQ(replacements.size(), 1u);
    EXPECT_EQ(replacements[0].address.instance_id, "instance:0");
    ASSERT_EQ(replacements[0].address.path.size(), 1u);
    EXPECT_EQ(replacements[0].definition_id, "iv.test.successor");
    EXPECT_EQ(
        nlohmann::ordered_json::parse(
            replacements[0].configuration_arguments_json),
        nlohmann::ordered_json::array({
            17,
            nlohmann::ordered_json{{"mode", "highpass"}},
        }));
    auto const desired_connections = graph_connections.desired_connections();
    ASSERT_EQ(desired_connections.size(), 2u);
    EXPECT_EQ(iv::project_connection_id(desired_connections[0]), "event:dangling");
    EXPECT_EQ(iv::project_connection_id(desired_connections[1]), "sample:structured");
    auto const* sample = std::get_if<iv::ProjectSampleConnection>(
        &desired_connections[1]);
    ASSERT_NE(sample, nullptr);
    EXPECT_EQ(sample->source_type, iv::ChannelTypeId::stereo);
    ASSERT_EQ(sample->outputs[0].path.size(), 3u);
    EXPECT_EQ(sample->outputs[0].port.channel, 0u);
    auto const generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->generation, 1u);
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
        "Persisted",
        "{.cutoff = 1200}");

    persistence.save();

    auto const commands = parse_project_file(workspace / "iv_project.jsonl");
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0]["command"], "nodeInstances.upsert");
    EXPECT_EQ(commands[0]["args"]["instance_id"], "instance:stable");
    EXPECT_EQ(commands[0]["args"]["definition_id"], "iv.test.persisted");
    EXPECT_EQ(commands[0]["args"]["package_root"], "modules/persisted");
    EXPECT_EQ(commands[0]["args"]["display_name"], "Persisted");
    EXPECT_EQ(commands[0]["args"]["configuration_source"], "{.cutoff = 1200}");
    EXPECT_EQ(commands[0]["args"].size(), 5u);
}

TEST(ProjectPersistence, SaveCollectsDanglingStructuredGraphConnections)
{
    auto const workspace = fresh_workspace("project_persistence_connection_save");
    iv::ProjectPersistence persistence(workspace, startup_for(workspace));
    iv::GraphConnections connections;
    auto connections_scope =
        iv::project_persistence_graph_connections_bridge::bind(
            persistence, connections);

    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    iv::GraphConnectionsProjectGraphRequest request{
        .root_builder = &root,
        .placements = &placements,
        .mutation = iv::GraphConnectionUpsertMutation{
            .connection = iv::ProjectEventConnection{
                .connection_id = "event:dangling",
                .source_type = iv::EventTypeId::midi,
                .outputs = {{
                    .instance_id = "source",
                    .path = {iv::ProjectVirtualMemberSelector{.index = 3}},
                    .port = {.name = "events"},
                }},
                .target_type = iv::EventTypeId::trigger,
                .inputs = {{
                    .instance_id = "target",
                    .port = {.index = 1},
                }},
            },
        },
    };
    connections.handle_project_graph_transaction(request);

    persistence.save();

    auto const commands = parse_project_file(workspace / "iv_project.jsonl");
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0]["command"], "graphConnections.upsert");
    EXPECT_EQ(commands[0]["args"]["kind"], "event");
    EXPECT_EQ(commands[0]["args"]["connection_id"], "event:dangling");
    EXPECT_EQ(commands[0]["args"]["source_type"], "midi");
    EXPECT_EQ(commands[0]["args"]["target_type"], "trigger");
    EXPECT_EQ(commands[0]["args"]["outputs"][0]["path"][0]["index"], 3u);
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
