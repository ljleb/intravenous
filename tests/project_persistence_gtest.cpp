#include "module_test_utils.h"
#include <intravenous/graph/programmatic_inputs.h>
#include "event_source_test_support.h"
#include "node_instances_test_driver.h"

#include <intravenous/module/package_definitions.h>
#include <intravenous/project/node_instances.h>
#include <intravenous/packages/node_definitions_events.h>
#include <intravenous/packages/package_pipeline_types.h>
#include <intravenous/project/persistence/project_connection_json.h>
#include <intravenous/project/persistence/project_persistence.h>
#include <intravenous/project/persistence/project_persistence_builder.h>
#include <intravenous/project/persistence/project_persistence_events.h>
#include <intravenous/app/bridges/project_persistence_project_graph_bridge.h>
#include <intravenous/project/graph_connections.h>
#include <intravenous/project/project_graph.h>
#include <intravenous/app/bridges/project_graph_graph_connections_bridge.h>
#include <intravenous/app/bridges/project_graph_node_instances_bridge.h>
#include <intravenous/project/runtime_project_events.h>
#include <intravenous/rpc/socket_rpc_requests.h>
#include <intravenous/presentation/state_json.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace {
// The snapshot record also carries ProjectGraph's reply (its publication), so
// the handler takes it as an lvalue.
void publish_definitions_snapshot(
    iv::ProjectGraph& project_graph,
    iv::NodeDefinitionsSnapshotChanged change)
{
    in_event_source([&] { return project_graph.handle_node_definitions_snapshot_changed(change); });
}

void expect_same_configuration(iv::ProjectGraph const& project_graph, iv::ProjectGraphGeneration const& expected)
{
    // Execution status can publish a new snapshot of the same configuration.
    auto const current = project_graph.current_generation();
    ASSERT_TRUE(current);
    EXPECT_EQ(current->configured_generation, expected.configured_generation);
    EXPECT_EQ(current->graph, expected.graph);
}

using Json = nlohmann::ordered_json;

constexpr char persisted_original_definition_id[] =
    "iv.test.persistence.original";
constexpr char persisted_successor_definition_id[] =
    "iv.test.persistence.successor";
constexpr char persisted_package_root[] =
    "/tmp/iv-project-persistence-replacement-tests";
constexpr char persisted_source_file[] =
    "/tmp/iv-project-persistence-replacement-tests/module.cpp";
iv::details::RegisteredSignature const persisted_zero_signature{};
std::atomic<int> persisted_successor_configure_calls{0};

iv::details::RegisteredSignature const* persisted_signature_callback()
{
    return &persisted_zero_signature;
}

void configure_persisted_original(
    iv::GraphBuilder& graph,
    std::span<iv::details::ConfigurationArgument>)
{
    using namespace iv;
    auto input = iv::programmatic::declare_input(graph, "main");
    graph.outputs(input);
}

void configure_persisted_successor(
    iv::GraphBuilder& graph,
    std::span<iv::details::ConfigurationArgument>)
{
    ++persisted_successor_configure_calls;
    using namespace iv;
    auto input = iv::programmatic::declare_input(graph, "main");
    graph.outputs(input);
}

std::shared_ptr<iv::NodeDefinitionsSnapshot const>
persisted_replacement_definitions()
{
    using iv::details::PackageDefinition;
    using iv::details::PackageDefinitionKind;

    auto revision = std::make_shared<iv::PackageRevision>();
    revision->package_id = "iv.test.persistence.package";
    revision->package_root = persisted_package_root;
    revision->revision = 1;
    auto add_provider = [&](char const* id,
                            std::size_t id_size,
                            iv::details::IvModuleConfigureFunction configure) {
        revision->provider_definitions.push_back(PackageDefinition{
            .kind = PackageDefinitionKind::module,
            .id = id,
            .id_size = id_size,
            .source_file = persisted_source_file,
            .source_file_size = sizeof(persisted_source_file) - 1,
            .package_root = persisted_package_root,
            .package_root_size = sizeof(persisted_package_root) - 1,
            .module_build = configure,
            .signature = &persisted_signature_callback,
        });
    };
    add_provider(
        persisted_original_definition_id,
        sizeof(persisted_original_definition_id) - 1,
        &configure_persisted_original);
    add_provider(
        persisted_successor_definition_id,
        sizeof(persisted_successor_definition_id) - 1,
        &configure_persisted_successor);

    auto snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>();
    snapshot->generation = 1;
    snapshot->package_revisions.push_back(revision);
    auto add_definition = [&](std::string id,
                              iv::details::IvModuleConfigureFunction configure) {
        snapshot->by_id.emplace(id, iv::NodeDefinitionEntry{
            .definition_id = id,
            .kind = iv::NodeDefinitionKind::module,
            .version = 1,
            .definition = iv::ModuleNodeDefinition{
                .definition_id = id,
                .package_id = revision->package_id,
                .package_root = revision->package_root,
                .module_id = id,
                .provider = iv::NodeDefinitionProvider{
                    .module_build = configure,
                    .signature = &persisted_signature_callback,
                },
            },
        });
    };
    add_definition(
        persisted_original_definition_id,
        &configure_persisted_original);
    add_definition(
        persisted_successor_definition_id,
        &configure_persisted_successor);
    return snapshot;
}

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

struct ProjectOverrideSettingsWitness {
    std::vector<iv::ProjectOverrideSettingsRequest> requests {};

    void handle_override_settings(iv::ProjectOverrideSettingsRequest const &request)
    {
        requests.push_back(request);
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
    project_override_settings_witness_bridge,
    iv::ProjectPersistence,
    ProjectOverrideSettingsWitness);
IV_DECLARE_BRIDGE(
    project_collect_state_contributor_bridge,
    iv::ProjectPersistence,
    ProjectCollectStateContributor);
IV_DEFINE_BRIDGE(project_override_settings_witness_bridge)
IV_DEFINE_BRIDGE(project_collect_state_contributor_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    project_override_settings_witness_bridge,
    iv_runtime_project_override_settings_requested_event,
    &ProjectOverrideSettingsWitness::handle_override_settings)
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
    startup.toolchain.c_compiler = "/installation/toolchain/clang";
    startup.toolchain.cxx_compiler = "/installation/toolchain/clang++";
    startup.toolchain.cmake_program = "/installation/cmake";
    startup.toolchain.make_program = "/installation/ninja";
    startup.toolchain.juce_dir = "/installation/juce";
    startup.toolchain.iv_package_pch = "/installation/iv_dsl.pch";

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
            .package_root = workspace / "packages" / "b",
            .display_name = "B",
            .configuration_source = "{.gain = 0.25}",
        },
        iv::ProjectNodeInstanceDeclaration{
            .instance_id = "instance:a",
            .definition_id = "iv.test.a",
            .package_root = workspace / "packages" / "a",
        },
    });
    builder.add_graph_connections({iv::ProjectSampleConnection{
        .connection_id = "connection:main",
        .source_type = iv::ChannelTypeId::stereo,
        .outputs = {{
            .instance_id = "instance:a",
            .path = {
                iv::ProjectSymbolicNodeSelector{
                    .source_identity = "processor",
                    .type_identity = "filter",
                },
                iv::ProjectSymbolicMemberSelector{.index = 1},
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
    builder.add_manual_controls({iv::ProjectManualControl{
        .id = {
            .node = {
                .instance_id = "instance:b",
                .path = {iv::ProjectSymbolicNodeSelector{
                    .source_identity = "processor",
                }},
            },
            .port_name = "gain",
        },
        .stored_value = iv::Sample{0.625f},
        .participation =
            iv::ManualControlParticipation::participate_with_connections,
    }});

    auto const commands = builder.build();
    ASSERT_EQ(commands.size(), 5u);
    EXPECT_EQ(commands[0].command, "project.overrideSettings");
    EXPECT_EQ(commands[0].args["cmake_generator"], "Unix Makefiles");
    EXPECT_EQ(commands[0].args["c_compiler"], "toolchain/clang");
    EXPECT_EQ(commands[0].args["output_device_id"], "out-1");
    EXPECT_TRUE(commands[0].args["input_device_id"].is_null());

    EXPECT_EQ(commands[1].command, "nodeInstances.upsert");
    EXPECT_EQ(commands[1].args["instance_id"], "instance:a");
    EXPECT_EQ(commands[1].args["definition_id"], "iv.test.a");
    EXPECT_EQ(commands[1].args["package_root"], "packages/a");
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
    EXPECT_EQ(output["path"][0]["kind"], "symbolicNode");
    EXPECT_EQ(output["path"][0]["source_identity"], "processor");
    EXPECT_EQ(output["path"][0]["type_identity"], "filter");
    EXPECT_EQ(output["path"][1]["kind"], "symbolicMember");
    EXPECT_EQ(output["path"][1]["index"], 1u);
    EXPECT_EQ(output["path"][2]["kind"], "tiledChild");
    EXPECT_EQ(output["path"][2]["name"], "right");
    EXPECT_EQ(output["port"]["channel"], 1u);

    EXPECT_EQ(commands[4].command, "manualControls.upsert");
    EXPECT_EQ(commands[4].args["node"]["instance_id"], "instance:b");
    ASSERT_EQ(commands[4].args["node"]["path"].size(), 1u);
    EXPECT_EQ(
        commands[4].args["node"]["path"][0]["source_identity"],
        "processor");
    EXPECT_EQ(commands[4].args["input_name"], "gain");
    EXPECT_FLOAT_EQ(commands[4].args["value"].get<float>(), 0.625f);
    EXPECT_EQ(
        commands[4].args["participation"],
        "participate_with_connections");
    EXPECT_TRUE(commands[4].args["enabled"]);

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
                iv::ProjectSymbolicNodeSelector{
                    .source_identity = "processor",
                    .type_identity = "filter",
                },
                iv::ProjectSymbolicMemberSelector{.index = 5},
                iv::ProjectTiledChildSelector{.name = "right"},
            },
            .port = {.name = "audio", .index = 2, .channel = 1},
        }},
        .target_type = iv::ChannelTypeId::mono,
        .inputs = {{
            .instance_id = "instance:target",
            .path = {iv::ProjectSymbolicNodeSelector{
                .source_identity = "sink",
            }},
            .port = {.index = 0, .channel = 0},
        }},
        .loop_extra_latency = 4,
    };

    for (auto const names : {
             iv::ProjectConnectionJsonNames::project_file,
             iv::ProjectConnectionJsonNames::socket_rpc,
         }) {
        auto const json = iv::project_connection_json(original, names);
        EXPECT_EQ(json.at(names == iv::ProjectConnectionJsonNames::project_file ? "loop_extra_latency" : "loopExtraLatency"), 4u);
        EXPECT_EQ(iv::parse_project_connection_json(json, names), original);
    }
}

TEST(ProjectConnectionJson, RoundTripsBeatEventTypes)
{
    iv::ProjectConnection const connection = iv::ProjectEventConnection{
        .connection_id = "connection:beats",
        .source_type = iv::EventTypeId::beat,
        .outputs = {{.instance_id = "beats", .port = {.name = "beats"}}},
        .target_type = iv::EventTypeId::trigger,
        .inputs = {{.instance_id = "sink", .port = {.name = "clock"}}},
        .loop_extra_latency = 1,
    };
    for (auto const names : {iv::ProjectConnectionJsonNames::project_file,
             iv::ProjectConnectionJsonNames::socket_rpc}) {
        auto const json = iv::project_connection_json(connection, names);
        EXPECT_EQ(iv::parse_project_connection_json(json, names), connection);
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
            .path = {iv::ProjectSymbolicNodeSelector{
                .source_identity = "processor",
            }},
        },
        .definition_id = "iv.test.successor",
        .package_root = workspace / "packages" / "successor",
        .configuration_arguments_json =
            R"([17,{"mode":"highpass"},[1,2,3]])",
    }});

    auto const commands = builder.build();
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].command, "nodeReplacements.upsert");
    EXPECT_EQ(commands[0].args["definition_id"], "iv.test.successor");
    EXPECT_EQ(commands[0].args["package_root"], "packages/successor");
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
        R"({"command":"nodeInstances.upsert","args":{"instance_id":"instance:0","definition_id":"iv.test.legacy","package_root":"packages/legacy","display_name":null}})" "\n"
        R"({"command":"nodeInstances.upsert","args":{"instance_id":"instance:removed","definition_id":"iv.test.removed","package_root":null,"display_name":null,"configuration_source":""}})" "\n"
        R"({"command":"nodeInstances.delete","args":{"instance_id":"instance:removed"}})" "\n"
        R"({"command":"nodeInstances.upsert","args":{"instance_id":"instance:1","definition_id":"iv.test.replayed","package_root":"packages/replayed","display_name":"Replayed","configuration_source":"12 + 4"}})" "\n"
        R"({"command":"manualControls.upsert","args":{"node":{"instance_id":"instance:0","path":[{"kind":"symbolicNode","source_identity":"processor"}]},"input_name":"gain","value":0.75,"participation":"participate_with_connections","enabled":true}})" "\n"
        R"({"command":"manualControls.upsert","args":{"node":{"instance_id":"instance:1","path":[]},"input_name":"removed","value":0.5,"participation":"disconnected_only","enabled":true}})" "\n"
        R"({"command":"manualControls.delete","args":{"node":{"instance_id":"instance:1","path":[]},"input_name":"removed"}})" "\n"
        R"({"command":"nodeReplacements.upsert","args":{"address":{"instance_id":"instance:0","path":[{"kind":"symbolicNode","source_identity":"processor"}]},"definition_id":"iv.test.successor","package_root":"packages/successor","configuration_arguments":[17,{"mode":"highpass"}]}})" "\n"
        R"({"command":"graphConnections.upsert","args":{"kind":"event","connection_id":"event:removed","source_type":"trigger","outputs":[{"instance_id":"instance:0","path":[],"port":{"name":"out"}}],"target_type":"empty","inputs":[{"instance_id":"instance:1","path":[],"port":{"index":0}}]}})" "\n"
        R"({"command":"graphConnections.delete","args":{"connection_id":"event:removed"}})" "\n"
        R"({"command":"graphConnections.upsert","args":{"kind":"event","connection_id":"event:dangling","source_type":"trigger","outputs":[{"instance_id":"instance:0","path":[],"port":{"name":"out"}}],"target_type":"empty","inputs":[{"instance_id":"instance:1","path":[],"port":{"index":0}}]}})" "\n"
        R"({"command":"graphConnections.upsert","args":{"kind":"sample","connection_id":"sample:structured","source_type":"stereo","outputs":[{"instance_id":"instance:0","path":[{"kind":"symbolicNode","source_identity":"processor","type_identity":"filter"},{"kind":"symbolicMember","index":2},{"kind":"tiledChild","name":"left"}],"port":{"name":"audio","index":1,"channel":0}}],"target_type":"mono","inputs":[{"instance_id":"instance:1","path":[],"port":{"name":"audio"}}]}})" "\n");

    iv::ProjectPersistence persistence(workspace, startup);
    iv::NodeInstances instances;
    iv::GraphConnections graph_connections;
    iv::ProjectGraph project_graph;
    auto project_graph_scope =
        iv::project_persistence_project_graph_bridge::bind(persistence, project_graph);
    auto project_graph_instances_scope =
        iv::project_graph_node_instances_bridge::bind(project_graph, instances);
    auto project_graph_connections_scope =
        iv::project_graph_graph_connections_bridge::bind(project_graph, graph_connections);

    auto const messages = persistence.load();

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
    // None of the loaded definitions exist, so the first configuration pass
    // fails and no generation becomes current; the desired revision is still
    // complete.
    EXPECT_FALSE(project_graph.current_generation());
    EXPECT_TRUE(project_graph.configuration_failure());
    auto const manual_controls = project_graph.manual_controls();
    ASSERT_EQ(manual_controls.size(), 1u);
    EXPECT_EQ(
        manual_controls[0].id.node.instance_id,
        "instance:0");
    ASSERT_EQ(manual_controls[0].id.node.path.size(), 1u);
    EXPECT_EQ(manual_controls[0].id.port_name, "gain");
    EXPECT_FLOAT_EQ(
        static_cast<float>(manual_controls[0].stored_value),
        0.75f);
    EXPECT_EQ(
        manual_controls[0].participation,
        iv::ManualControlParticipation::participate_with_connections);
    EXPECT_TRUE(std::ranges::any_of(messages, [](auto const &message) {
        return message.level == "error"
            && message.message.contains("unknown project command: removed.timelineCommand");
    }));
}

TEST(ProjectPersistence, ReplayRaisesEveryOverrideCommandAsOneMergedRequest)
{
    auto const workspace = fresh_workspace("project_persistence_merged_overrides");
    auto startup = startup_for(workspace);
    write_text(
        workspace / "iv_project.jsonl",
        R"({"command":"project.overrideSettings","args":{"cmake_generator":"Ninja","output_device_id":"first-output"}})" "\n"
        R"({"command":"project.overrideSettings","args":{"output_device_id":"second-output","input_device_id":"second-input"}})" "\n");

    iv::ProjectPersistence persistence(workspace, startup);
    ProjectOverrideSettingsWitness witness;
    auto witness_scope = project_override_settings_witness_bridge::bind(persistence, witness);

    (void)persistence.load();

    ASSERT_EQ(witness.requests.size(), 1u);
    auto const& request = witness.requests.front();
    EXPECT_EQ(request.cmake_generator, std::optional<std::string>("Ninja"));
    EXPECT_EQ(request.output_device_id, std::optional<std::string>("second-output"));
    EXPECT_EQ(request.input_device_id, std::optional<std::string>("second-input"));
}

TEST(ProjectPersistence, LoadedReplacementConfiguresAfterDefinitionsArrive)
{
    auto const workspace = fresh_workspace(
        "project_persistence_reconstructs_node_replacement");
    auto startup = startup_for(workspace);
    write_text(
        workspace / "iv_project.jsonl",
        R"({"command":"nodeInstances.upsert","args":{"instance_id":"instance:original","definition_id":"iv.test.persistence.original","package_root":null,"display_name":null,"configuration_source":""}})" "\n"
        R"({"command":"nodeReplacements.upsert","args":{"address":{"instance_id":"instance:original","path":[]},"definition_id":"iv.test.persistence.successor","package_root":null,"configuration_arguments":[]}})" "\n");

    iv::ProjectPersistence persistence(workspace, startup);
    iv::NodeInstances instances;
    iv::GraphConnections graph_connections;
    iv::ProjectGraph project_graph;
    auto persistence_graph_scope =
        iv::project_persistence_project_graph_bridge::bind(
            persistence, project_graph);
    auto graph_instances_scope =
        iv::project_graph_node_instances_bridge::bind(
            project_graph, instances);
    auto graph_connections_scope =
        iv::project_graph_graph_connections_bridge::bind(
            project_graph, graph_connections);
    persisted_successor_configure_calls = 0;

    (void)persistence.load();
    ASSERT_EQ(instances.desired_node_replacements().size(), 1u);
    // The definitions have not arrived, so the first configuration pass fails.
    EXPECT_FALSE(project_graph.current_generation());
    EXPECT_TRUE(project_graph.configuration_failure());

    publish_definitions_snapshot(project_graph, 
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = persisted_replacement_definitions(),
        });

    auto const generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    ASSERT_TRUE(generation->placements.contains("instance:original"));
    EXPECT_EQ(
        generation->placements.at("instance:original")
            .configured->definition_id,
        persisted_successor_definition_id);
    ASSERT_EQ(generation->node_replacements.size(), 1u);
    EXPECT_EQ(
        generation->node_replacements[0].definition_id,
        persisted_successor_definition_id);
    EXPECT_EQ(persisted_successor_configure_calls.load(), 1);
}

TEST(ProjectPersistence, SaveCollectsInstanceMetadataWithoutExecutionState)
{
    auto const workspace = fresh_workspace("project_persistence_instance_save");
    auto startup = startup_for(workspace);
    iv::ProjectPersistence persistence(workspace, startup);
    iv::NodeInstances instances;
    iv::GraphConnections connections;
    iv::ProjectGraph graph;
    auto persistence_graph_scope = iv::project_persistence_project_graph_bridge::bind(persistence, graph);
    auto graph_instances_scope = iv::project_graph_node_instances_bridge::bind(graph, instances);
    auto graph_connections_scope = iv::project_graph_graph_connections_bridge::bind(graph, connections);

    (void)iv::test::NodeInstancesDriver(instances).create(
        "iv.test.persisted",
        workspace / "packages" / "persisted",
        "instance:stable",
        "Persisted",
        "{.cutoff = 1200}");

    in_event_source([&] { return persistence.save(); });

    auto const commands = parse_project_file(workspace / "iv_project.jsonl");
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0]["command"], "nodeInstances.upsert");
    EXPECT_EQ(commands[0]["args"]["instance_id"], "instance:stable");
    EXPECT_EQ(commands[0]["args"]["definition_id"], "iv.test.persisted");
    EXPECT_EQ(commands[0]["args"]["package_root"], "packages/persisted");
    EXPECT_EQ(commands[0]["args"]["display_name"], "Persisted");
    EXPECT_EQ(commands[0]["args"]["configuration_source"], "{.cutoff = 1200}");
    EXPECT_EQ(commands[0]["args"].size(), 5u);
}

TEST(ProjectPersistence, SaveCollectsDanglingStructuredGraphConnections)
{
    auto const workspace = fresh_workspace("project_persistence_connection_save");
    iv::ProjectPersistence persistence(workspace, startup_for(workspace));
    iv::GraphConnections connections;
    iv::NodeInstances instances;
    iv::ProjectGraph graph;
    auto persistence_graph_scope = iv::project_persistence_project_graph_bridge::bind(persistence, graph);
    auto graph_instances_scope = iv::project_graph_node_instances_bridge::bind(graph, instances);
    auto graph_connections_scope = iv::project_graph_graph_connections_bridge::bind(graph, connections);

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
                    .path = {iv::ProjectSymbolicMemberSelector{.index = 3}},
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
    in_event_source([&] { return connections.handle_project_graph_transaction(request); });

    in_event_source([&] { return persistence.save(); });

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

    in_event_source([&] { return persistence.save(); });

    EXPECT_TRUE(parse_project_file(workspace / "iv_project.jsonl").empty());
}

TEST(ProjectPersistence, LanePresentationReplaysAndFollowsNodeHistory)
{
    auto const workspace = fresh_workspace("project_persistence_lane_presentation");
    auto const project_file = workspace / "iv_project.jsonl";
    std::string lane_state = R"({"pan":3e0,"values":[1e10)";
    for (int index = 1; index < 20'000; ++index) lane_state += ",1e10";
    lane_state += "]}";
    auto lane_view = nlohmann::ordered_json::parse(
        R"({"command":"projectPresentation.setLaneView","args":{"address":{"instance_id":"curve","path":[]},"presentation_id":"curve","state":null}})");
    lane_view["args"]["state"] = lane_state;
    write_text(project_file,
        std::string{R"({"command":"nodeInstances.upsert","args":{"instance_id":"curve","definition_id":"iv.test.persistence.original"}})" "\n"
            R"({"command":"projectPresentation.setLaneOrder","args":{"order":[{"instance_id":"curve","path":[]}]}})" "\n"}
        + lane_view.dump() + "\n" + R"({"command":"projectPresentation.setTimelineSnapMemory","args":{"memory":{"beats":[120,4,[2]]}}})" "\n");
    iv::ProjectPersistence persistence(workspace, startup_for(workspace));
    iv::NodeInstances instances;
    iv::GraphConnections graph_connections;
    iv::ProjectGraph project_graph;
    auto persistence_graph_scope = iv::project_persistence_project_graph_bridge::bind(persistence, project_graph);
    auto graph_instances_scope = iv::project_graph_node_instances_bridge::bind(project_graph, instances);
    auto graph_connections_scope = iv::project_graph_graph_connections_bridge::bind(project_graph, graph_connections);
    publish_definitions_snapshot(project_graph, {.snapshot = persisted_replacement_definitions()});
    EXPECT_TRUE(persistence.load().empty());

    auto save_commands = [&] {
        in_event_source([&] { persistence.save(); });
        return parse_project_file(project_file);
    };
    auto edit = [&](iv::ProjectEditBatch const& batch) {
        iv::ProjectAckBuilder builder;
        in_event_source([&] { project_graph.handle_project_apply_edit(batch, builder); });
        builder.build();
        return std::move(builder.accepted_lane_views);
    };
    auto undo = [&] {
        iv::ProjectAckBuilder builder;
        in_event_source([&] { project_graph.handle_project_undo(builder); });
        builder.build();
    };
    auto const original = save_commands();
    ASSERT_EQ(original.size(), 4u);
    EXPECT_EQ(original[1]["args"]["order"][0]["instance_id"], "curve");
    EXPECT_EQ(original[2]["args"]["state"], lane_state);
    EXPECT_EQ(original[3]["args"]["memory"]["beats"][0], 120);

    auto const generation = project_graph.current_generation();
    ASSERT_TRUE(generation && generation->graph);
    iv::ProjectLaneView const view{{{.instance_id = "curve"}, "curve"}, R"({"pan":8})"};
    iv::ProjectEditBatch adjust;
    adjust.lane_views.push_back({view.id, view});
    auto const committed = edit(adjust);
    ASSERT_EQ(committed.size(), 1u);
    EXPECT_EQ(committed.front().id, view.id);
    EXPECT_EQ(committed.front().version, view.version());
    expect_same_configuration(project_graph, *generation);
    EXPECT_THROW(undo(), std::runtime_error);

    // An identical save leaves existing scope references readable.
    iv::ProjectLaneView const identical{view.id, view.state_json()};
    adjust.lane_views = {{identical.id, identical}};
    auto const unchanged = edit(adjust);
    ASSERT_EQ(unchanged.size(), 1u);
    EXPECT_EQ(unchanged.front().version, view.version());

    iv::ProjectEditBatch duplicate;
    duplicate.instance_duplicates.push_back({"curve", "copy"});
    duplicate.lane_order = std::vector<iv::NodeAddress>{{.instance_id = "curve"}, {.instance_id = "copy"}};
    edit(duplicate);
    auto const copied = save_commands();
    ASSERT_EQ(copied.size(), 6u);
    EXPECT_EQ(copied[2]["args"]["order"][1]["instance_id"], "copy");
    EXPECT_EQ(copied[3]["args"]["address"]["instance_id"], "copy");
    EXPECT_EQ(copied[3]["args"]["state"], view.state_json());

    std::vector<std::optional<std::string>> read;
    project_graph.handle_socket_rpc_get_project_lane_views({{view.version(), 0}}, read);
    ASSERT_EQ(read.size(), 2u);
    EXPECT_EQ(read[0], view.state_json());
    EXPECT_FALSE(read[1]);

    // A later pan on the copy does not prevent undo from removing the copy.
    iv::ProjectLaneView const changed{{{.instance_id = "copy"}, "curve"}, R"({"pan":5})"};
    adjust.lane_views = {{changed.id, changed}};
    edit(adjust);
    undo();
    auto const undone = save_commands();
    ASSERT_EQ(undone.size(), 4u);
    EXPECT_EQ(undone[1]["args"]["order"].size(), 1u);
    EXPECT_EQ(undone[2]["args"]["state"], view.state_json());
    read.clear();
    project_graph.handle_socket_rpc_get_project_lane_views({{changed.version()}}, read);
    ASSERT_EQ(read.size(), 1u);
    EXPECT_FALSE(read.front());

    auto redo = [&] {
        iv::ProjectAckBuilder builder;
        in_event_source([&] { project_graph.handle_project_redo(builder); });
        builder.build();
    };
    iv::ProjectLaneView const after_rename{view.id, R"({"pan":0.5})"};
    iv::ProjectLaneView const newer{view.id, R"({"pan":0.75})"};
    iv::ProjectEditBatch rename;
    rename.instance_updates.push_back({.instance_id = "curve", .display_name = "Renamed"});
    rename.lane_views.push_back({after_rename.id, after_rename});
    edit(rename);
    adjust.lane_views = {{newer.id, newer}};
    edit(adjust);
    auto check_view = [&] {
        std::vector<std::optional<std::string>> views;
        project_graph.handle_socket_rpc_get_project_lane_views({{newer.version(), after_rename.version()}}, views);
        ASSERT_EQ(views.size(), 2u);
        EXPECT_EQ(views[0], newer.state_json());
        EXPECT_FALSE(views[1]);
    };
    undo();
    check_view();
    redo();
    check_view();
}

TEST(ProjectPersistence, LaneNestingRoundTripsAndUndoesWithoutReconfiguringTheGraph)
{
    auto const workspace = fresh_workspace("project_persistence_lane_nesting");
    auto const project_file = workspace / "iv_project.jsonl";
    write_text(project_file,
        R"({"command":"nodeInstances.upsert","args":{"instance_id":"module","definition_id":"iv.test.persistence.original"}})" "\n"
        R"({"command":"nodeInstances.upsert","args":{"instance_id":"lane","definition_id":"iv.test.persistence.original"}})" "\n");
    iv::ProjectPersistence persistence(workspace, startup_for(workspace));
    iv::NodeInstances instances;
    iv::GraphConnections graph_connections;
    iv::ProjectGraph project_graph;
    auto persistence_graph_scope = iv::project_persistence_project_graph_bridge::bind(persistence, project_graph);
    auto graph_instances_scope = iv::project_graph_node_instances_bridge::bind(project_graph, instances);
    auto graph_connections_scope = iv::project_graph_graph_connections_bridge::bind(project_graph, graph_connections);
    publish_definitions_snapshot(project_graph, {.snapshot = persisted_replacement_definitions()});
    ASSERT_TRUE(persistence.load().empty());
    auto const generation = project_graph.current_generation();
    ASSERT_TRUE(generation && generation->graph);

    auto apply = [&](iv::ProjectEditBatch const& batch) {
        iv::ProjectAckBuilder builder;
        in_event_source([&] { project_graph.handle_project_apply_edit(batch, builder); });
        builder.build();
    };
    auto save = [&] {
        in_event_source([&] { persistence.save(); });
        return parse_project_file(project_file);
    };
    iv::ProjectEditBatch nest;
    nest.lane_order = std::vector<iv::NodeAddress>{{.instance_id = "module"}, {.instance_id = "lane"}};
    nest.lane_parents = std::vector<iv::ProjectLaneParent>{{"lane", {.instance_id = "module"}}};
    ASSERT_NO_THROW(apply(nest));
    expect_same_configuration(project_graph, *generation);
    auto const nested = save();
    ASSERT_EQ(nested.size(), 4u);
    EXPECT_EQ(nested[3]["command"], "projectPresentation.setLaneParents");
    EXPECT_EQ(nested[3]["args"]["parents"][0]["instance_id"], "lane");
    EXPECT_EQ(nested[3]["args"]["parents"][0]["parent"]["instance_id"], "module");

    iv::ProjectEditBatch cycle;
    cycle.lane_parents = std::vector<iv::ProjectLaneParent>{
        {"lane", {.instance_id = "module"}}, {"module", {.instance_id = "lane"}}};
    EXPECT_THROW(apply(cycle), std::invalid_argument);
    iv::ProjectAckBuilder undo;
    in_event_source([&] { project_graph.handle_project_undo(undo); });
    ASSERT_NO_THROW(undo.build());
    expect_same_configuration(project_graph, *generation);
    EXPECT_EQ(save().size(), 2u);
    iv::ProjectAckBuilder redo;
    in_event_source([&] { project_graph.handle_project_redo(redo); });
    ASSERT_NO_THROW(redo.build());
    expect_same_configuration(project_graph, *generation);
    EXPECT_EQ(save(), nested);
    ASSERT_TRUE(persistence.load().empty());
    EXPECT_EQ(save(), nested);
}

TEST(ProjectPersistence, SavingUnchangedContentsKeepsExistingFile)
{
    auto const workspace = fresh_workspace("project_persistence_clean_save");
    auto const project_file = workspace / "iv_project.jsonl";
    write_text(project_file, "");
    auto const recorded_time = std::filesystem::last_write_time(project_file) - std::chrono::hours(1);
    std::filesystem::last_write_time(project_file, recorded_time);
    auto const timestamp = std::filesystem::last_write_time(project_file);
    iv::ProjectPersistence persistence(workspace, startup_for(workspace));
    in_event_source([&] { persistence.save(); });
    EXPECT_EQ(std::filesystem::last_write_time(project_file), timestamp);
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

    EXPECT_THROW(in_event_source([&] { return persistence.save(); }), std::runtime_error);

    std::ifstream in(project_file);
    ASSERT_TRUE(in);
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(contents, "sentinel\n");
}

TEST(ProjectPersistence, ExplicitNullOverridesSurviveMergingAndDefaultAudioIdStaysLiteral)
{
    auto const workspace = fresh_workspace("project_settings_explicit_clears");
    auto startup = startup_for(workspace);
    write_text(workspace / "iv_project.jsonl",
        R"({"command":"project.overrideSettings","args":{"cxx_compiler":"/old/compiler","output_device_id":"out-1","input_device_id":"in-1"}})" "\n"
        R"({"command":"project.overrideSettings","args":{"cxx_compiler":null,"output_device_id":"default","input_device_id":null}})" "\n");
    iv::ProjectPersistence persistence(workspace, startup);
    ProjectOverrideSettingsWitness witness;
    auto scope = project_override_settings_witness_bridge::bind(persistence, witness);
    (void)persistence.load();
    ASSERT_EQ(witness.requests.size(), 1u);
    auto const& request = witness.requests[0];
    EXPECT_FALSE(request.cxx_compiler);
    EXPECT_TRUE(request.specifies(iv::ProjectSettingField::cxx_compiler));
    EXPECT_EQ(request.output_device_id, std::optional<std::string>{"default"});
    EXPECT_FALSE(request.input_device_id);
    EXPECT_TRUE(request.specifies(iv::ProjectSettingField::input_device_id));
    EXPECT_FALSE(request.specifies(iv::ProjectSettingField::c_compiler));
}


TEST(ProjectPersistenceBuilder, InputAndOutputControlsWithTheSameNameRemainDistinct)
{
    auto workspace = fresh_workspace("project_output_gain_identity");
    iv::ProjectPersistenceBuilder builder(workspace, startup_for(workspace));
    iv::ManualControlId input{.node = {.instance_id = "synth"}, .port_name = "main"};
    auto output = input;
    output.kind = iv::ManualControlKind::output_gain;
    builder.add_manual_controls({{.id = output, .stored_value = -.5f}, {.id = input, .stored_value = 440}});
    auto commands = builder.build();
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[0].args.at("input_name"), "main");
    EXPECT_FALSE(commands[0].args.contains("output_name"));
    EXPECT_EQ(commands[1].args.at("output_name"), "main");
    EXPECT_FALSE(commands[1].args.contains("input_name"));
    EXPECT_EQ(commands[1].args.at("value"), -.5f);
}

TEST(PresentationStateJson, RoundTripsFiniteBinary64Extremes)
{
    constexpr auto max = std::numeric_limits<double>::max();
    constexpr auto tiny = std::numeric_limits<double>::denorm_min();
    constexpr auto normal = std::numeric_limits<double>::min();
    for (auto value : {max, -max, tiny, -tiny, normal, -0.0, 1.0 / 3.0}) {
        char bytes[1024]{};
        IvJsonWriter writer{bytes, sizeof(bytes), 0, 0, false, {}};
        iv_json_object(&writer);
        iv_json_number(&writer, "value", value);
        iv_json_end(&writer);
        int const size = iv_json_finish(&writer);
        ASSERT_GT(size, 0);
        auto const restored = iv_json_read_decimal_member({bytes, static_cast<std::uint32_t>(size)}, "value", 123);
        EXPECT_EQ(std::bit_cast<std::uint64_t>(restored), std::bit_cast<std::uint64_t>(value));
    }

    // The host parses saved state and may send a shorter decimal on restore.
    constexpr char max_from_host[] = "1.7976931348623157e308";
    constexpr char beyond_max[] = "1.7976931348623159e308";
    EXPECT_EQ(iv_json_read_number({max_from_host, max_from_host + sizeof(max_from_host) - 1}, 123), max);
    EXPECT_EQ(iv_json_read_number({beyond_max, beyond_max + sizeof(beyond_max) - 1}, 123), 123);
}
