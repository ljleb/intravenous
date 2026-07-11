#include "module_test_utils.h"
#include "fake_audio_device.h"

#include <intravenous/basic_lane_nodes/controls.h>
#include <intravenous/dsl.h>
#include <intravenous/linker_event.h>
#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/project_persistence.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/runtime_project_audio_device_lanes_bridge.h>
#include <intravenous/runtime/runtime_project_graph_input_lanes_bridge.h>
#include <intravenous/runtime/runtime_project_iv_module_instances_bridge.h>
#include <intravenous/runtime/runtime_project_iv_module_reload_bridge.h>
#include <intravenous/runtime/runtime_project_lane_views_bridge.h>
#include <intravenous/runtime/runtime_project_timeline_execution_bridge.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_execution.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <regex>
#include <sstream>
#include <set>
#include <string_view>
#include <vector>

namespace {
using Json = nlohmann::ordered_json;
using iv::test_support::mutable_module_fixture_workspace;
using iv::test_support::read_text;
using iv::test_support::write_text;

iv::InternedString intern(std::string_view value)
{
    return iv::InternedString::from_view(value);
}

std::string runtime_node_id(std::string_view instance_id, std::string_view logical_node_id)
{
    return std::string(instance_id) + "\x1flogical:" + std::string(logical_node_id);
}

struct TestEventLaneNode {
    static auto output()
    {
        return iv::RealtimeEventLaneOutputConfig{
            .name = "events",
            .event_type = iv::EventTypeId::trigger,
        };
    }

    void tick_block_realtime(iv::RealtimeLaneTickContext<TestEventLaneNode> &) {}
};

struct IdleFakeAudioInputDevice {
    iv::RenderConfig config_;

    explicit IdleFakeAudioInputDevice(iv::RenderConfig config)
        : config_(std::move(config))
    {
        iv::validate_render_config(config_);
    }

    iv::RenderConfig const &config() const
    {
        return config_;
    }

    iv::AudioInputBlock wait_for_captured_block()
    {
        throw std::logic_error("idle fake input device has no captured blocks");
    }

    void release_captured_block() {}
    void request_shutdown() {}
};

iv::AudioDeviceLanesBackend make_audio_backend()
{
    return iv::AudioDeviceLanesBackend{
        .list_output_devices = [] {
            return std::vector<iv::AudioDeviceDescriptor>{
                {.device_id = "default", .name = "System Default"},
                {.device_id = "out-1", .name = "Output 1"},
            };
        },
        .list_input_devices = [] {
            return std::vector<iv::AudioDeviceDescriptor>{
                {.device_id = "default", .name = "System Default"},
                {.device_id = "in-1", .name = "Input 1"},
            };
        },
        .make_output_device = [](std::string const &, iv::RenderConfig const &config) {
            return iv::AudioOutputDevice(std::in_place_type<iv::test::FakeAudioDevice>, config);
        },
        .make_input_device = [](std::string const &, iv::RenderConfig const &config) {
            return iv::AudioInputDevice(std::in_place_type<IdleFakeAudioInputDevice>, config);
        },
    };
}

struct ProjectNotificationWitness {
    std::vector<iv::ProjectMessageNotification> messages {};
};

ProjectNotificationWitness *g_project_notification_witness = nullptr;
bool g_throw_on_collect_state = false;

IV_SUBSCRIBE_LINKER_EVENT(
    iv::ProjectNotificationEvent,
    iv_runtime_project_notification_event,
    +[](iv::ProjectNotification const &notification) {
        if (g_project_notification_witness == nullptr) {
            return;
        }
        if (auto const *message =
                std::get_if<iv::ProjectMessageNotification>(&notification)) {
            g_project_notification_witness->messages.push_back(*message);
        }
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::ProjectPersistenceCollectStateEvent,
    iv_runtime_project_persistence_collect_state_event,
    +[](iv::ProjectPersistenceBuilder &) {
        if (g_throw_on_collect_state) {
            throw std::runtime_error("test contributor failure");
        }
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::GraphInputLanesTimelineBatchRequestedEvent,
    iv_runtime_graph_input_lanes_timeline_batch_requested_event,
    +[](iv::TimelineLaneBatchUpdate const &,
        iv::GraphInputLanesAckBuilder &builder) {
        builder.succeed();
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::GraphInputLanesRebuildRequestedEvent,
    iv_runtime_graph_input_lanes_rebuild_requested_event,
    +[](iv::GraphInputLanesRebuildRequested const &) {});

Json parse_json_line(std::string_view line)
{
    return Json::parse(line);
}

std::vector<Json> parse_project_file(std::filesystem::path const &path)
{
    std::vector<Json> commands;
    std::istringstream in(read_text(path));
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) {
            continue;
        }
        commands.push_back(Json::parse(line));
    }
    return commands;
}

iv::StartupConfigState make_startup(std::filesystem::path const &workspace)
{
    return iv::StartupConfigState{
        .workspace_root = workspace,
        .execution = iv::RuntimeExecutionConfig{
            .sample_rate = 48000,
            .block_size = 8,
            .compiled_sample_cache_chunk_size_multiplier = 16,
        },
        .output_device_id = std::optional<std::string>("default"),
        .input_device_id = std::optional<std::string>("default"),
    };
}

iv::TimelineLaneBatchUpdate timeline_batch_with_two_lanes()
{
    return iv::TimelineLaneBatchUpdate{
        .version_index = 1,
        .upserts = {
            iv::TimelineLaneUpsert{
                .lane = iv::LaneId{1},
                .external_id = intern("lane-a"),
                .make_node = [] { return iv::TypeErasedLaneNode(iv::KnobLaneNode{.value = 1.0f}); },
                .sample_channel_type = iv::ChannelTypeId::mono,
            },
            iv::TimelineLaneUpsert{
                .lane = iv::LaneId{2},
                .external_id = intern("lane-b"),
                .make_node = [] { return iv::TypeErasedLaneNode(iv::KnobLaneNode{.value = 2.0f}); },
                .sample_channel_type = iv::ChannelTypeId::mono,
            },
        },
    };
}

iv::IvModuleInstance make_instance_with_ports()
{
    iv::IvModuleInstance instance {};
    instance.instance_id = "instance:graph";
    instance.definition_id = "definition:graph";
    instance.module_root = std::filesystem::path("/tmp/module");
    instance.module_id = "iv.test.module";

    iv::IntrospectionLogicalNode node {};
    node.id = "node-1";
    node.kind = "TestNode";
    node.sample_inputs.push_back(iv::IntrospectionPortInfo{
        .name = "frequency",
        .type = "sample",
        .connectivity = iv::LogicalPortConnectivity::disconnected,
        .ordinal = 0,
        .default_value = 440.0f,
    });
    node.event_inputs.push_back(iv::IntrospectionPortInfo{
        .name = "trigger",
        .type = "event",
        .connectivity = iv::LogicalPortConnectivity::disconnected,
        .ordinal = 0,
    });
    instance.introspection.logical_nodes.push_back(std::move(node));
    return instance;
}

iv::IvModuleInstance make_instance_with_member_ports()
{
    auto instance = make_instance_with_ports();
    iv::IntrospectionLogicalNode::Member member {};
    member.ordinal = 0;
    member.backing_node_id = "node-1";
    member.kind = "TestNode";
    member.sample_inputs.push_back(iv::IntrospectionPortInfo{
        .name = "frequency",
        .type = "sample",
        .connectivity = iv::LogicalPortConnectivity::disconnected,
        .ordinal = 0,
        .default_value = 440.0f,
    });
    member.event_inputs.push_back(iv::IntrospectionPortInfo{
        .name = "trigger",
        .type = "event",
        .connectivity = iv::LogicalPortConnectivity::disconnected,
        .ordinal = 0,
    });
    instance.introspection.logical_nodes.front().members.push_back(std::move(member));
    return instance;
}

void initialize_two_timeline_lanes(iv::Timeline &timeline)
{
    timeline.apply_lane_batch(timeline_batch_with_two_lanes());
}

iv::TimelineLaneBatchUpdate timeline_batch_with_event_lane()
{
    return iv::TimelineLaneBatchUpdate{
        .version_index = 1,
        .upserts = {
            iv::TimelineLaneUpsert{
                .lane = iv::LaneId{1},
                .external_id = intern("event-lane"),
                .make_node = [] { return iv::TypeErasedLaneNode(TestEventLaneNode{}); },
            },
        },
    };
}

size_t count_messages_with_level(
    std::vector<iv::ProjectMessageNotification> const &messages,
    std::string_view level)
{
    return static_cast<size_t>(std::count_if(
        messages.begin(),
        messages.end(),
        [&](auto const &message) {
            return message.level == level;
        }));
}

class ProjectPersistenceTest : public ::testing::Test {
protected:
    ProjectNotificationWitness witness {};

    void SetUp() override
    {
        g_project_notification_witness = &witness;
        g_throw_on_collect_state = false;
    }

    void TearDown() override
    {
        g_project_notification_witness = nullptr;
        g_throw_on_collect_state = false;
    }
};
} // namespace

TEST_F(ProjectPersistenceTest, OverrideParsingAppliesTypedOverridesAcrossOwnersAndWarnsUnknownKeys)
{
    auto const workspace = mutable_module_fixture_workspace("project_override_matrix", "local_cmake");
    auto const toolchain_dir = workspace / "toolchain";
    std::filesystem::create_directories(toolchain_dir);
    write_text(toolchain_dir / "clang", "");
    write_text(toolchain_dir / "clangxx", "");
    write_text(toolchain_dir / "cmake", "");
    write_text(toolchain_dir / "make", "");
    write_text(toolchain_dir / "juce", "");
    write_text(
        workspace / "iv_project.jsonl",
        Json{
            {"command", "project.overrideSettings"},
            {"args", Json{
                {"c_compiler", "toolchain/clang"},
                {"cxx_compiler", "toolchain/clangxx"},
                {"cmake_program", "toolchain/cmake"},
                {"cmake_generator", "Ninja"},
                {"make_program", "toolchain/make"},
                {"juce_dir", "toolchain/juce"},
                {"compiled_sample_cache_chunk_size_multiplier", 8},
                {"output_device_id", "out-1"},
                {"input_device_id", "in-1"},
                {"unknown_key", "mystery"},
            }},
        }.dump() + "\n");

    auto const startup = make_startup(workspace);
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    iv::AudioDeviceLanes audio_device_lanes(48000, 8, make_audio_backend());
    iv::IvModuleReload reload(startup);
    iv::ProjectPersistence persistence(workspace, startup);

    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
    iv::bind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
    iv::bind_runtime_project_iv_module_reload_bridge(reload);
    iv::bind_project_persistence_bridge(persistence);

    persistence.load();

    EXPECT_EQ(execution.compiled_sample_cache_chunk_size_multiplier(), 8u);
    auto const snapshot = audio_device_lanes.audio_devices_snapshot();
    EXPECT_EQ(snapshot.selected_output.device_id, std::optional<std::string>("out-1"));
    EXPECT_EQ(snapshot.selected_input.device_id, std::optional<std::string>("in-1"));
    auto const toolchain = reload.toolchain_config();
    ASSERT_TRUE(toolchain.c_compiler.has_value());
    ASSERT_TRUE(toolchain.cxx_compiler.has_value());
    ASSERT_TRUE(toolchain.cmake_program.has_value());
    ASSERT_TRUE(toolchain.make_program.has_value());
    ASSERT_TRUE(toolchain.juce_dir.has_value());
    EXPECT_EQ(toolchain.cmake_generator, std::optional<std::string>("Ninja"));
    EXPECT_TRUE(toolchain.c_compiler->string().ends_with("toolchain/clang"));
    ASSERT_EQ(witness.messages.size(), 1u);
    EXPECT_EQ(witness.messages.front().level, "warning");
    EXPECT_TRUE(witness.messages.front().message.contains("unknown_key"));

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_iv_module_reload_bridge(reload);
    iv::unbind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
    iv::unbind_runtime_project_timeline_execution_bridge(execution);
}

TEST_F(ProjectPersistenceTest, OverrideParsingDefaultAndUnknownOnlyDoNotMutateState)
{
    auto const workspace = mutable_module_fixture_workspace("project_override_defaults", "local_cmake");
    write_text(
        workspace / "iv_project.jsonl",
        Json{
            {"command", "project.overrideSettings"},
            {"args", Json{
                {"compiled_sample_cache_chunk_size_multiplier", "default"},
                {"c_compiler", "default"},
                {"unknown_key", "mystery"},
            }},
        }.dump() + "\n");

    auto const startup = make_startup(workspace);
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    iv::IvModuleReload reload(startup);
    iv::ProjectPersistence persistence(workspace, startup);

    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
    iv::bind_runtime_project_iv_module_reload_bridge(reload);
    iv::bind_project_persistence_bridge(persistence);

    persistence.load();

    EXPECT_EQ(execution.compiled_sample_cache_chunk_size_multiplier(), 16u);
    EXPECT_FALSE(reload.toolchain_config().c_compiler.has_value());
    ASSERT_EQ(witness.messages.size(), 1u);
    EXPECT_EQ(witness.messages.front().level, "warning");

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_iv_module_reload_bridge(reload);
    iv::unbind_runtime_project_timeline_execution_bridge(execution);
}

TEST_F(ProjectPersistenceTest, OverrideParsingInvalidRecognizedKeyLogsErrorAndLaterCreateSucceeds)
{
    auto const workspace = mutable_module_fixture_workspace("project_override_invalid_then_create", "local_cmake");
    write_text(
        workspace / "iv_project.jsonl",
        Json{
            {"command", "project.overrideSettings"},
            {"args", Json{{"compiled_sample_cache_chunk_size_multiplier", -1}}},
        }.dump() + "\n" +
        Json{
            {"command", "ivModuleInstances.create"},
            {"args", Json{
                {"instance_id", "instance-z"},
                {"module_root", "."},
            }},
        }.dump() + "\n");

    auto const startup = make_startup(workspace);
    iv::IvModuleInstances instances;
    iv::ProjectPersistence persistence(workspace, startup);

    iv::bind_runtime_project_iv_module_instances_bridge(instances);
    iv::bind_project_persistence_bridge(persistence);

    persistence.load();

    auto const listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().instance_id, "instance-z");
    ASSERT_EQ(witness.messages.size(), 1u);
    EXPECT_EQ(witness.messages.front().level, "error");
    EXPECT_TRUE(witness.messages.front().message.contains("compiled_sample_cache_chunk_size_multiplier"));

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_iv_module_instances_bridge(instances);
}

TEST_F(ProjectPersistenceTest, OverrideParsingInvalidSupportedFieldTypesLogErrorsAndReplayContinues)
{
    struct Case {
        std::string name;
        Json args;
        std::string expected_message_fragment;
    };

    std::vector<Case> cases{
        {
            .name = "bad_path_type",
            .args = Json{{"c_compiler", 42}},
            .expected_message_fragment = "toolchain settings must be strings or null",
        },
        {
            .name = "bad_generator_type",
            .args = Json{{"cmake_generator", true}},
            .expected_message_fragment = "toolchain settings must be strings or null",
        },
        {
            .name = "bad_cxx_compiler_type",
            .args = Json{{"cxx_compiler", Json::array({1})}},
            .expected_message_fragment = "toolchain settings must be strings or null",
        },
        {
            .name = "bad_cmake_program_type",
            .args = Json{{"cmake_program", false}},
            .expected_message_fragment = "toolchain settings must be strings or null",
        },
        {
            .name = "bad_make_program_type",
            .args = Json{{"make_program", 3.14}},
            .expected_message_fragment = "toolchain settings must be strings or null",
        },
        {
            .name = "bad_juce_dir_type",
            .args = Json{{"juce_dir", Json::object({{"bad", "value"}})}},
            .expected_message_fragment = "toolchain settings must be strings or null",
        },
        {
            .name = "bad_device_id_type",
            .args = Json{{"output_device_id", Json::array({1, 2, 3})}},
            .expected_message_fragment = "audio device id override settings must be strings or null",
        },
        {
            .name = "bad_input_device_id_type",
            .args = Json{{"input_device_id", 9}},
            .expected_message_fragment = "audio device id override settings must be strings or null",
        },
        {
            .name = "bad_chunk_size_string",
            .args = Json{{"compiled_sample_cache_chunk_size_multiplier", "bogus"}},
            .expected_message_fragment = "compiled_sample_cache_chunk_size_multiplier",
        },
        {
            .name = "bad_chunk_size_negative",
            .args = Json{{"compiled_sample_cache_chunk_size_multiplier", -1}},
            .expected_message_fragment = "compiled_sample_cache_chunk_size_multiplier",
        },
    };

    for (size_t i = 0; i < cases.size(); ++i) {
        auto const workspace = mutable_module_fixture_workspace(
            "project_override_invalid_field_" + std::to_string(i),
            "local_cmake");
        write_text(
            workspace / "iv_project.jsonl",
            Json{
                {"command", "project.overrideSettings"},
                {"args", cases[i].args},
            }.dump() + "\n" +
            Json{
                {"command", "ivModuleInstances.create"},
                {"args", Json{
                    {"instance_id", "instance-ok"},
                    {"module_root", "."},
                }},
            }.dump() + "\n");

        auto const startup = make_startup(workspace);
        iv::IvModuleInstances instances;
        iv::ProjectPersistence persistence(workspace, startup);
        witness.messages.clear();

        iv::bind_runtime_project_iv_module_instances_bridge(instances);
        iv::bind_project_persistence_bridge(persistence);

        persistence.load();

        auto const listed = instances.list_instances();
        ASSERT_EQ(listed.size(), 1u) << cases[i].name;
        EXPECT_EQ(listed.front().instance_id, "instance-ok");
        ASSERT_EQ(count_messages_with_level(witness.messages, "error"), 1u) << cases[i].name;
        EXPECT_TRUE(witness.messages.front().message.contains(cases[i].expected_message_fragment))
            << cases[i].name;

        iv::unbind_project_persistence_bridge(persistence);
        iv::unbind_runtime_project_iv_module_instances_bridge(instances);
    }
}

TEST_F(ProjectPersistenceTest, DirectTypedOverrideRequestsMutateRelevantOwnersOnly)
{
    auto const workspace = mutable_module_fixture_workspace("project_override_direct_subscribers", "local_cmake");
    auto const startup = make_startup(workspace);
    auto const toolchain_dir = workspace / "toolchain";
    std::filesystem::create_directories(toolchain_dir);
    write_text(toolchain_dir / "clang", "");

    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    iv::AudioDeviceLanes audio_device_lanes(48000, 8, make_audio_backend());
    iv::IvModuleReload reload(startup);

    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
    iv::bind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
    iv::bind_runtime_project_iv_module_reload_bridge(reload);

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_project_override_settings_requested_event,
        iv::ProjectOverrideSettingsRequest{
            .c_compiler = std::filesystem::weakly_canonical(toolchain_dir / "clang").lexically_normal(),
            .compiled_sample_cache_chunk_size_multiplier = 8,
            .output_device_id = std::string("out-1"),
        });

    EXPECT_EQ(execution.compiled_sample_cache_chunk_size_multiplier(), 8u);
    EXPECT_EQ(
        audio_device_lanes.audio_devices_snapshot().selected_output.device_id,
        std::optional<std::string>("out-1"));
    ASSERT_TRUE(reload.toolchain_config().c_compiler.has_value());
    EXPECT_TRUE(reload.toolchain_config().c_compiler->string().ends_with("toolchain/clang"));
    EXPECT_EQ(
        audio_device_lanes.audio_devices_snapshot().selected_input.device_id,
        std::optional<std::string>("default"));

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_project_override_settings_requested_event,
        iv::ProjectOverrideSettingsRequest{});

    EXPECT_EQ(execution.compiled_sample_cache_chunk_size_multiplier(), 8u);

    iv::unbind_runtime_project_iv_module_reload_bridge(reload);
    iv::unbind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
    iv::unbind_runtime_project_timeline_execution_bridge(execution);
}

TEST_F(ProjectPersistenceTest, DirectTypedOverrideRequestsIgnoreIrrelevantFieldsPerSubscriber)
{
    auto const workspace = mutable_module_fixture_workspace("project_override_irrelevant_fields", "local_cmake");
    auto const startup = make_startup(workspace);
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    iv::AudioDeviceLanes audio_device_lanes(48000, 8, make_audio_backend());
    iv::IvModuleReload reload(startup);

    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
    iv::bind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
    iv::bind_runtime_project_iv_module_reload_bridge(reload);

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_project_override_settings_requested_event,
        iv::ProjectOverrideSettingsRequest{
            .cmake_generator = std::string("Ninja"),
            .input_device_id = std::string("in-1"),
        });

    EXPECT_EQ(execution.compiled_sample_cache_chunk_size_multiplier(), 16u);
    EXPECT_EQ(
        audio_device_lanes.audio_devices_snapshot().selected_output.device_id,
        std::optional<std::string>("default"));
    EXPECT_EQ(
        audio_device_lanes.audio_devices_snapshot().selected_input.device_id,
        std::optional<std::string>("in-1"));
    EXPECT_EQ(reload.toolchain_config().cmake_generator, std::optional<std::string>("Ninja"));
    EXPECT_FALSE(reload.toolchain_config().c_compiler.has_value());

    iv::unbind_runtime_project_iv_module_reload_bridge(reload);
    iv::unbind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
    iv::unbind_runtime_project_timeline_execution_bridge(execution);
}

TEST_F(ProjectPersistenceTest, DirectTypedOverrideRequestUpdatesOnlyAudioDeviceSubscriber)
{
    auto const workspace = mutable_module_fixture_workspace("project_override_audio_only", "local_cmake");
    auto const startup = make_startup(workspace);
    iv::AudioDeviceLanes audio_device_lanes(48000, 8, make_audio_backend());

    iv::bind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_project_override_settings_requested_event,
        iv::ProjectOverrideSettingsRequest{
            .output_device_id = std::string("out-1"),
            .input_device_id = std::string("in-1"),
        });

    auto const snapshot = audio_device_lanes.audio_devices_snapshot();
    EXPECT_EQ(snapshot.selected_output.device_id, std::optional<std::string>("out-1"));
    EXPECT_EQ(snapshot.selected_input.device_id, std::optional<std::string>("in-1"));

    iv::unbind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
}

TEST_F(ProjectPersistenceTest, DirectTypedOverrideRequestUpdatesOnlyReloadSubscriber)
{
    auto const workspace = mutable_module_fixture_workspace("project_override_reload_only", "local_cmake");
    auto const startup = make_startup(workspace);
    auto const toolchain_dir = workspace / "toolchain";
    std::filesystem::create_directories(toolchain_dir);
    write_text(toolchain_dir / "clang", "");
    write_text(toolchain_dir / "clangxx", "");

    iv::IvModuleReload reload(startup);
    iv::bind_runtime_project_iv_module_reload_bridge(reload);

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_project_override_settings_requested_event,
        iv::ProjectOverrideSettingsRequest{
            .c_compiler = std::filesystem::weakly_canonical(toolchain_dir / "clang").lexically_normal(),
            .cxx_compiler = std::filesystem::weakly_canonical(toolchain_dir / "clangxx").lexically_normal(),
            .cmake_generator = std::string("Ninja"),
        });

    auto const toolchain = reload.toolchain_config();
    ASSERT_TRUE(toolchain.c_compiler.has_value());
    ASSERT_TRUE(toolchain.cxx_compiler.has_value());
    EXPECT_EQ(toolchain.cmake_generator, std::optional<std::string>("Ninja"));

    iv::unbind_runtime_project_iv_module_reload_bridge(reload);
}

TEST_F(ProjectPersistenceTest, ReplayKeepsGoingAfterMissingInstanceMutationAndRestoresView)
{
    auto const workspace = mutable_module_fixture_workspace("project_replay_partial_failure", "local_cmake");
    write_text(
        workspace / "iv_project.jsonl",
        Json{
            {"command", "ivModuleInstances.setDefaultSilenceTtlSamples"},
            {"args", Json{
                {"instance_id", "missing"},
                {"default_silence_ttl_samples", 32},
            }},
        }.dump() + "\n" +
        Json{
            {"command", "timeline.openLaneView"},
            {"args", Json{
                {"view_id", "view-a"},
                {"query_source", "graph_input"},
                {"start_index", 4},
                {"visible_lane_count", 8},
                {"first_sample_index", 10},
                {"last_sample_index", 20},
                {"display_sample_count", 10},
            }},
        }.dump() + "\n");

    auto const startup = make_startup(workspace);
    iv::IvModuleInstances instances;
    iv::LaneViews lane_views;
    iv::ProjectPersistence persistence(workspace, startup);

    iv::bind_runtime_project_iv_module_instances_bridge(instances);
    iv::bind_runtime_project_lane_views_bridge(lane_views);
    iv::bind_project_persistence_bridge(persistence);

    persistence.load();

    auto const views = lane_views.active_view_requests();
    ASSERT_EQ(views.size(), 1u);
    EXPECT_EQ(views.front().view_id.str(), "view-a");
    EXPECT_EQ(views.front().start_index, 4u);
    ASSERT_EQ(witness.messages.size(), 1u);
    EXPECT_EQ(witness.messages.front().level, "error");
    EXPECT_TRUE(witness.messages.front().message.contains("missing"));

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_lane_views_bridge(lane_views);
    iv::unbind_runtime_project_iv_module_instances_bridge(instances);
}

TEST_F(ProjectPersistenceTest, ReplayKeepsGoingAfterMiddleCommandFailure)
{
    auto const workspace = mutable_module_fixture_workspace("project_replay_middle_failure", "local_cmake");
    write_text(
        workspace / "iv_project.jsonl",
        Json{
            {"command", "ivModuleInstances.create"},
            {"args", Json{
                {"instance_id", "instance-a"},
                {"module_root", "."},
            }},
        }.dump() + "\n" +
        Json{
            {"command", "ivModuleInstances.setDefaultSilenceTtlSamples"},
            {"args", Json{
                {"instance_id", "missing"},
                {"default_silence_ttl_samples", 32},
            }},
        }.dump() + "\n" +
        Json{
            {"command", "timeline.openLaneView"},
            {"args", Json{
                {"view_id", "view-b"},
                {"query_source", "graph_input"},
                {"start_index", 2},
                {"visible_lane_count", 4},
                {"first_sample_index", 0},
                {"last_sample_index", 8},
                {"display_sample_count", 8},
            }},
        }.dump() + "\n");

    auto const startup = make_startup(workspace);
    iv::IvModuleInstances instances;
    iv::LaneViews lane_views;
    iv::ProjectPersistence persistence(workspace, startup);

    iv::bind_runtime_project_iv_module_instances_bridge(instances);
    iv::bind_runtime_project_lane_views_bridge(lane_views);
    iv::bind_project_persistence_bridge(persistence);

    persistence.load();

    ASSERT_EQ(instances.list_instances().size(), 1u);
    EXPECT_EQ(instances.list_instances().front().instance_id, "instance-a");
    ASSERT_EQ(lane_views.active_view_requests().size(), 1u);
    EXPECT_EQ(lane_views.active_view_requests().front().view_id.str(), "view-b");
    EXPECT_EQ(count_messages_with_level(witness.messages, "error"), 1u);
    EXPECT_TRUE(witness.messages.front().message.contains("missing"));

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_lane_views_bridge(lane_views);
    iv::unbind_runtime_project_iv_module_instances_bridge(instances);
}

TEST_F(ProjectPersistenceTest, UnknownOverrideKeysWarnAndDoNotBlockLaterCommands)
{
    auto const workspace = mutable_module_fixture_workspace("project_override_unknown_then_continue", "local_cmake");
    write_text(
        workspace / "iv_project.jsonl",
        Json{
            {"command", "project.overrideSettings"},
            {"args", Json{
                {"unknown_a", "value-a"},
                {"unknown_b", "value-b"},
            }},
        }.dump() + "\n" +
        Json{
            {"command", "ivModuleInstances.create"},
            {"args", Json{
                {"instance_id", "instance-after-warning"},
                {"module_root", "."},
            }},
        }.dump() + "\n");

    auto const startup = make_startup(workspace);
    iv::IvModuleInstances instances;
    iv::ProjectPersistence persistence(workspace, startup);

    iv::bind_runtime_project_iv_module_instances_bridge(instances);
    iv::bind_project_persistence_bridge(persistence);

    persistence.load();

    ASSERT_EQ(instances.list_instances().size(), 1u);
    EXPECT_EQ(instances.list_instances().front().instance_id, "instance-after-warning");
    EXPECT_EQ(count_messages_with_level(witness.messages, "warning"), 2u);
    EXPECT_EQ(count_messages_with_level(witness.messages, "error"), 0u);

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_iv_module_instances_bridge(instances);
}

TEST_F(ProjectPersistenceTest, SaveLoadSaveRoundTripIsStableForCoreState)
{
    auto const workspace = mutable_module_fixture_workspace("project_round_trip_core", "local_cmake");
    auto const startup = make_startup(workspace);
    auto const toolchain_dir = workspace / "toolchain";
    std::filesystem::create_directories(toolchain_dir);
    write_text(toolchain_dir / "clang", "");

    iv::IvModuleInstances instances;
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    iv::AudioDeviceLanes audio_device_lanes(48000, 8, make_audio_backend());
    iv::LaneViews lane_views;
    iv::IvModuleReload reload(startup);
    iv::ProjectPersistence persistence(workspace, startup);

    initialize_two_timeline_lanes(timeline);
    instances.create_instance(workspace, "instance-a");
    instances.set_default_silence_ttl_samples("instance-a", 123);
    execution.set_compiled_sample_cache_chunk_size_multiplier(8);
    (void)audio_device_lanes.set_selected_devices("out-1", "in-1");
    audio_device_lanes.set_lane_external_ids(intern("audio-out"), intern("audio-in"));
    reload.set_toolchain_config(iv::ModuleLoaderToolchainConfig{
        .c_compiler = std::filesystem::weakly_canonical(toolchain_dir / "clang").lexically_normal(),
        .cxx_compiler = std::nullopt,
    });
    (void)lane_views.open_view(iv::LaneViewRequest{
        .view_id = intern("view-a"),
        .query = iv::LaneQuery{.filter = iv::LaneQueryFilter{.source = "graph_input"}},
        .start_index = 1,
        .visible_lane_count = 3,
        .first_sample_index = 10,
        .last_sample_index = 20,
        .display_sample_count = 10,
    });
    iv::ProjectAckBuilder connection_builder;
    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
    iv::ProjectAckBuilder channel_type_builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_project_set_timeline_lane_sample_channel_type_requested_event,
        iv::ProjectSetTimelineLaneSampleChannelTypeRequest{
            .lane_id = intern("lane-b"),
            .sample_channel_type = iv::ChannelTypeId::stereo,
        },
        channel_type_builder);
    channel_type_builder.build();
    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_project_connect_timeline_lanes_requested_event,
        iv::ProjectConnectTimelineLanesRequest{
            .source_lane_id = intern("lane-a"),
            .target_lane_id = intern("lane-b"),
            .port_domain = iv::LanePortDomain::realtime,
            .port_kind = iv::PortKind::sample,
            .port_ordinal = 0,
        },
        connection_builder);
    connection_builder.build();

    iv::bind_runtime_project_iv_module_instances_bridge(instances);
    iv::bind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
    iv::bind_runtime_project_lane_views_bridge(lane_views);
    iv::bind_runtime_project_iv_module_reload_bridge(reload);
    iv::bind_project_persistence_bridge(persistence);

    persistence.save();
    auto const original = read_text(workspace / "iv_project.jsonl");
    auto const original_commands = parse_project_file(workspace / "iv_project.jsonl");
    EXPECT_TRUE(std::ranges::any_of(original_commands, [](auto const &command) {
        return command["command"] == "timeline.setLaneSampleChannelType"
            && command["args"]["lane_id"] == "lane-b"
            && command["args"]["sample_channel_type"] == "stereo";
    }));

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_iv_module_reload_bridge(reload);
    iv::unbind_runtime_project_lane_views_bridge(lane_views);
    iv::unbind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
    iv::unbind_runtime_project_iv_module_instances_bridge(instances);
    iv::unbind_runtime_project_timeline_execution_bridge(execution);

    iv::IvModuleInstances fresh_instances;
    iv::Timeline fresh_timeline;
    iv::TimelineExecution fresh_execution(8, 16);
    iv::AudioDeviceLanes fresh_audio_device_lanes(48000, 8, make_audio_backend());
    iv::LaneViews fresh_lane_views;
    iv::IvModuleReload fresh_reload(startup);
    iv::ProjectPersistence fresh_persistence(workspace, startup);

    initialize_two_timeline_lanes(fresh_timeline);
    iv::bind_runtime_project_timeline_execution_bridge(fresh_timeline, fresh_execution, workspace);
    iv::bind_runtime_project_iv_module_instances_bridge(fresh_instances);
    iv::bind_runtime_project_audio_device_lanes_bridge(fresh_audio_device_lanes);
    iv::bind_runtime_project_lane_views_bridge(fresh_lane_views);
    iv::bind_runtime_project_iv_module_reload_bridge(fresh_reload);
    iv::bind_project_persistence_bridge(fresh_persistence);

    fresh_persistence.load();
    fresh_persistence.save();

    EXPECT_EQ(read_text(workspace / "iv_project.jsonl"), original);
    ASSERT_EQ(fresh_instances.list_instances().size(), 1u);
    EXPECT_EQ(fresh_instances.list_instances().front().instance_id, "instance-a");
    EXPECT_EQ(fresh_execution.compiled_sample_cache_chunk_size_multiplier(), 8u);
    EXPECT_EQ(
        fresh_audio_device_lanes.output_lane_external_id().str(),
        "audio-out");
    EXPECT_EQ(fresh_lane_views.active_view_requests().size(), 1u);
    ASSERT_TRUE(fresh_reload.toolchain_config().c_compiler.has_value());
    EXPECT_EQ(fresh_timeline.lane_connections().size(), 1u);
    ASSERT_TRUE(fresh_timeline.resolve_public_lane_id(intern("lane-b")).has_value());
    EXPECT_EQ(
        fresh_timeline.lane_sample_channel_type(*fresh_timeline.resolve_public_lane_id(intern("lane-b"))),
        std::optional<iv::ChannelTypeId>(iv::ChannelTypeId::stereo));

    iv::unbind_project_persistence_bridge(fresh_persistence);
    iv::unbind_runtime_project_iv_module_reload_bridge(fresh_reload);
    iv::unbind_runtime_project_lane_views_bridge(fresh_lane_views);
    iv::unbind_runtime_project_audio_device_lanes_bridge(fresh_audio_device_lanes);
    iv::unbind_runtime_project_iv_module_instances_bridge(fresh_instances);
    iv::unbind_runtime_project_timeline_execution_bridge(fresh_execution);
}

TEST(ProjectPersistenceBuilder, NormalizesSettingsPathsAndStableOrdering)
{
    auto const workspace = mutable_module_fixture_workspace("project_builder_normalization", "local_cmake");
    auto const startup = make_startup(workspace);
    auto const inside = workspace / "toolchain" / "clang";
    auto const outside = std::filesystem::path("/tmp/project_builder_make");
    std::filesystem::create_directories(inside.parent_path());
    write_text(inside, "");
    write_text(outside, "");

    iv::ProjectPersistenceBuilder builder(workspace, startup);
    builder.add_project_toolchain_config(iv::ModuleLoaderToolchainConfig{
        .c_compiler = std::filesystem::weakly_canonical(inside).lexically_normal(),
        .cxx_compiler = std::nullopt,
        .make_program = std::filesystem::weakly_canonical(outside).lexically_normal(),
    });
    builder.add_project_compiled_sample_cache_chunk_size_multiplier(16);
    builder.add_project_audio_device_selection(
        std::optional<std::string>("default"),
        std::optional<std::string>("default"));
    builder.add_iv_module_instances({
        iv::IvModuleInstanceInfo{
            .instance_id = "b",
            .module_root = workspace,
        },
        iv::IvModuleInstanceInfo{
            .instance_id = "a",
            .module_root = workspace,
        },
    });
    builder.add_lane_views({
        iv::LaneViewRequest{
            .view_id = intern("view-b"),
            .query = iv::LaneQuery{.filter = iv::LaneQueryFilter{.source = "two"}},
        },
        iv::LaneViewRequest{
            .view_id = intern("view-a"),
            .query = iv::LaneQuery{.filter = iv::LaneQueryFilter{.source = "one"}},
        },
    });
    builder.add_lane_connections({
        iv::ProjectConnectTimelineLanesRequest{
            .source_lane_id = intern("lane-z"),
            .target_lane_id = intern("lane-a"),
            .port_domain = iv::LanePortDomain::compiled,
            .port_kind = iv::PortKind::event,
            .port_ordinal = 2,
        },
        iv::ProjectConnectTimelineLanesRequest{
            .source_lane_id = intern("lane-a"),
            .target_lane_id = intern("lane-z"),
            .port_domain = iv::LanePortDomain::realtime,
            .port_kind = iv::PortKind::sample,
            .port_ordinal = 0,
        },
    });

    auto const commands = builder.build();
    ASSERT_FALSE(commands.empty());
    EXPECT_EQ(commands.front().command, "project.overrideSettings");
    EXPECT_EQ(commands.front().args["c_compiler"], "toolchain/clang");
    auto const make_program_path = std::filesystem::path(
        commands.front().args["make_program"].get<std::string>());
    EXPECT_FALSE(make_program_path.is_absolute());
    EXPECT_TRUE(commands.front().args["make_program"].get<std::string>().ends_with("project_builder_make"));
    EXPECT_FALSE(commands.front().args.contains("compiled_sample_cache_chunk_size_multiplier"));
    EXPECT_FALSE(commands.front().args.contains("output_device_id"));

    std::vector<std::string> created_instance_ids;
    std::vector<std::string> view_ids;
    std::vector<std::pair<std::string, std::string>> connection_order;
    for (auto const &command : commands) {
        if (command.command == "ivModuleInstances.create") {
            created_instance_ids.push_back(command.args["instance_id"].get<std::string>());
        } else if (command.command == "timeline.openLaneView") {
            view_ids.push_back(command.args["view_id"].get<std::string>());
        } else if (command.command == "timeline.connectLanes") {
            connection_order.emplace_back(
                command.args["source_lane_id"].get<std::string>(),
                command.args["port_domain"].get<std::string>());
        }
    }

    EXPECT_EQ(created_instance_ids, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(view_ids, (std::vector<std::string>{"view-a", "view-b"}));
    EXPECT_EQ(
        connection_order,
        (std::vector<std::pair<std::string, std::string>>{
            {"lane-a", "realtime"},
            {"lane-z", "compiled"},
        }));
}

TEST_F(ProjectPersistenceTest, GraphInputAuthoredStateCoversAllMutationKinds)
{
    iv::GraphInputLanes::AuthoredStateSnapshot snapshot;
    snapshot.sample_input_values.push_back(iv::ProjectSetSampleInputValueRequest{
        .node_id = "node-1",
        .member_ordinal = std::nullopt,
        .input_ordinal = 0,
        .value = iv::Sample{0.5f},
    });
    snapshot.sample_input_states.push_back(iv::ProjectSetSampleInputStateRequest{
        .node_id = "node-1",
        .member_ordinal = 0,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
        .lane_id = intern("sample-lane"),
    });
    snapshot.event_input_states.push_back(iv::ProjectSetEventInputStateRequest{
        .node_id = "node-1",
        .member_ordinal = 0,
        .input_ordinal = 0,
        .state = iv::ProjectEventInputState::timeline_lane,
        .lane_id = intern("event-lane"),
    });
    snapshot.sample_output_states.push_back(iv::ProjectSetSampleOutputStateRequest{
        .node_id = "node-1",
        .member_ordinal = std::nullopt,
        .output_ordinal = 0,
        .state = iv::ProjectSampleOutputState::timeline_lane,
        .lane_id = intern("sample-out"),
    });
    snapshot.event_output_states.push_back(iv::ProjectSetEventOutputStateRequest{
        .node_id = "node-1",
        .member_ordinal = 0,
        .output_ordinal = 0,
        .state = iv::ProjectEventOutputState::timeline_lane,
        .lane_id = intern("event-out"),
    });

    iv::ProjectPersistenceBuilder builder(std::filesystem::current_path(), make_startup(std::filesystem::current_path()));
    builder.add_graph_input_authored_state(snapshot);
    auto const commands = builder.build();
    std::multiset<std::string> names;
    for (auto const &command : commands) {
        names.insert(command.command);
    }
    EXPECT_TRUE(names.contains("graph.setSampleInputValue"));
    EXPECT_TRUE(names.contains("graph.setSampleInputState"));
    EXPECT_TRUE(names.contains("graph.setEventInputState"));
    EXPECT_TRUE(names.contains("graph.setSampleOutputState"));
    EXPECT_TRUE(names.contains("graph.setEventOutputState"));
}

TEST(ProjectPersistenceBuilder, GraphInputAuthoredStateSerializesStateVariantsForMemberAndLogicalPorts)
{
    auto const logical_sample_input_node_id = runtime_node_id("instance:a", "node-logical");
    auto const member_sample_input_node_id = runtime_node_id("instance:a", "node-member");
    auto const logical_event_input_node_id = runtime_node_id("instance:b", "event-logical");
    auto const member_event_input_node_id = runtime_node_id("instance:b", "event-member");
    auto const logical_sample_output_node_id = runtime_node_id("instance:c", "sample-out");
    auto const member_event_output_node_id = runtime_node_id("instance:c", "event-out");
    iv::GraphInputLanes::AuthoredStateSnapshot snapshot;
    snapshot.sample_input_states.push_back(iv::ProjectSetSampleInputStateRequest{
        .node_id = logical_sample_input_node_id,
        .member_ordinal = std::nullopt,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::disconnected,
        .lane_id = std::nullopt,
    });
    snapshot.sample_input_states.push_back(iv::ProjectSetSampleInputStateRequest{
        .node_id = member_sample_input_node_id,
        .member_ordinal = 1,
        .input_ordinal = 2,
        .state = iv::ProjectSampleInputState::logical_follow,
        .lane_id = std::nullopt,
    });
    snapshot.event_input_states.push_back(iv::ProjectSetEventInputStateRequest{
        .node_id = logical_event_input_node_id,
        .member_ordinal = std::nullopt,
        .input_ordinal = 0,
        .state = iv::ProjectEventInputState::default_,
        .lane_id = std::nullopt,
    });
    snapshot.event_input_states.push_back(iv::ProjectSetEventInputStateRequest{
        .node_id = member_event_input_node_id,
        .member_ordinal = 3,
        .input_ordinal = 1,
        .state = iv::ProjectEventInputState::disconnected,
        .lane_id = std::nullopt,
    });
    snapshot.sample_output_states.push_back(iv::ProjectSetSampleOutputStateRequest{
        .node_id = logical_sample_output_node_id,
        .member_ordinal = std::nullopt,
        .output_ordinal = 0,
        .state = iv::ProjectSampleOutputState::logical,
        .lane_id = std::nullopt,
    });
    snapshot.event_output_states.push_back(iv::ProjectSetEventOutputStateRequest{
        .node_id = member_event_output_node_id,
        .member_ordinal = 4,
        .output_ordinal = 1,
        .state = iv::ProjectEventOutputState::disconnected,
        .lane_id = std::nullopt,
    });

    iv::ProjectPersistenceBuilder builder(
        std::filesystem::current_path(),
        make_startup(std::filesystem::current_path()));
    builder.add_graph_input_authored_state(snapshot);
    auto const commands = builder.build();

    auto const find_command = [&](std::string_view name, std::string_view node_id) -> Json {
        for (auto const &command : commands) {
            if (command.command == name && command.args["node_id"] == node_id) {
                return command.args;
            }
        }
        throw std::runtime_error("command not found");
    };

    EXPECT_EQ(find_command("graph.setSampleInputState", logical_sample_input_node_id)["state"], "disconnected");
    EXPECT_TRUE(find_command("graph.setSampleInputState", logical_sample_input_node_id)["member_ordinal"].is_null());
    EXPECT_EQ(find_command("graph.setSampleInputState", member_sample_input_node_id)["state"], "logicalFollow");
    EXPECT_EQ(find_command("graph.setSampleInputState", member_sample_input_node_id)["member_ordinal"], 1);
    EXPECT_EQ(find_command("graph.setEventInputState", logical_event_input_node_id)["state"], "default");
    EXPECT_EQ(find_command("graph.setEventInputState", member_event_input_node_id)["state"], "disconnected");
    EXPECT_EQ(find_command("graph.setSampleOutputState", logical_sample_output_node_id)["state"], "logical");
    EXPECT_EQ(find_command("graph.setEventOutputState", member_event_output_node_id)["state"], "disconnected");
}

TEST_F(ProjectPersistenceTest, TimelineConnectivityReplayDefersConnectionUntilLanesExist)
{
    auto const workspace = mutable_module_fixture_workspace("project_timeline_connectivity_replay", "local_cmake");
    write_text(
        workspace / "iv_project.jsonl",
        Json{
            {"command", "timeline.connectLanes"},
            {"args", Json{
                {"source_lane_id", "lane-a"},
                {"target_lane_id", "lane-b"},
                {"port_domain", "realtime"},
                {"port_kind", "sample"},
                {"port_ordinal", 0},
            }},
        }.dump() + "\n");

    auto const startup = make_startup(workspace);
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    iv::ProjectPersistence persistence(workspace, startup);

    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
    iv::bind_project_persistence_bridge(persistence);

    persistence.load();

    EXPECT_TRUE(timeline.lane_connections().empty());
    ASSERT_EQ(timeline.pending_public_connections().size(), 1u);
    EXPECT_TRUE(witness.messages.empty());

    initialize_two_timeline_lanes(timeline);

    auto const connections = timeline.lane_connections();
    ASSERT_EQ(connections.size(), 1u);
    EXPECT_EQ(timeline.lane_public_id(connections.front().source).str(), "lane-a");
    EXPECT_EQ(timeline.lane_public_id(connections.front().target).str(), "lane-b");
    EXPECT_TRUE(timeline.pending_public_connections().empty());

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_timeline_execution_bridge(execution);
}

TEST_F(ProjectPersistenceTest, TimelineConnectivityReplayDefersUntilTargetExistsAndStillRestoresView)
{
    auto const workspace = mutable_module_fixture_workspace("project_timeline_missing_target", "local_cmake");
    write_text(
        workspace / "iv_project.jsonl",
        Json{
            {"command", "timeline.connectLanes"},
            {"args", Json{
                {"source_lane_id", "lane-a"},
                {"target_lane_id", "lane-b"},
                {"port_domain", "realtime"},
                {"port_kind", "sample"},
                {"port_ordinal", 0},
            }},
        }.dump() + "\n" +
        Json{
            {"command", "timeline.openLaneView"},
            {"args", Json{
                {"view_id", "view-after-target-failure"},
                {"query_source", "graph_input"},
                {"start_index", 3},
                {"visible_lane_count", 5},
                {"first_sample_index", 0},
                {"last_sample_index", 16},
                {"display_sample_count", 16},
            }},
        }.dump() + "\n");

    auto const startup = make_startup(workspace);
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    iv::LaneViews lane_views;
    iv::ProjectPersistence persistence(workspace, startup);

    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
    iv::bind_runtime_project_lane_views_bridge(lane_views);
    iv::bind_project_persistence_bridge(persistence);

    persistence.load();

    EXPECT_TRUE(timeline.lane_connections().empty());
    ASSERT_EQ(timeline.pending_public_connections().size(), 1u);
    ASSERT_EQ(lane_views.active_view_requests().size(), 1u);
    EXPECT_EQ(lane_views.active_view_requests().front().view_id.str(), "view-after-target-failure");
    EXPECT_TRUE(witness.messages.empty());

    initialize_two_timeline_lanes(timeline);
    ASSERT_EQ(timeline.lane_connections().size(), 1u);
    EXPECT_TRUE(timeline.pending_public_connections().empty());

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_lane_views_bridge(lane_views);
    iv::unbind_runtime_project_timeline_execution_bridge(execution);
}

TEST_F(ProjectPersistenceTest, TimelineLaneSampleChannelTypeFailureDoesNotStopLaterCommands)
{
    auto const workspace = mutable_module_fixture_workspace("project_timeline_bad_channel_target", "local_cmake");
    write_text(
        workspace / "iv_project.jsonl",
        Json{
            {"command", "timeline.setLaneSampleChannelType"},
            {"args", Json{
                {"lane_id", "event-lane"},
                {"sample_channel_type", "stereo"},
            }},
        }.dump() + "\n" +
        Json{
            {"command", "timeline.openLaneView"},
            {"args", Json{
                {"view_id", "view-after-channel-failure"},
                {"query_source", "graph_input"},
                {"start_index", 0},
                {"visible_lane_count", 2},
                {"first_sample_index", 0},
                {"last_sample_index", 4},
                {"display_sample_count", 4},
            }},
        }.dump() + "\n");

    auto const startup = make_startup(workspace);
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    iv::LaneViews lane_views;
    iv::ProjectPersistence persistence(workspace, startup);
    timeline.apply_lane_batch(timeline_batch_with_event_lane());

    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
    iv::bind_runtime_project_lane_views_bridge(lane_views);
    iv::bind_project_persistence_bridge(persistence);

    persistence.load();

    ASSERT_EQ(lane_views.active_view_requests().size(), 1u);
    EXPECT_EQ(lane_views.active_view_requests().front().view_id.str(), "view-after-channel-failure");
    ASSERT_EQ(count_messages_with_level(witness.messages, "error"), 1u);
    EXPECT_TRUE(witness.messages.front().message.contains("does not produce samples"));

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_lane_views_bridge(lane_views);
    iv::unbind_runtime_project_timeline_execution_bridge(execution);
}

TEST_F(ProjectPersistenceTest, ProjectSaveUnboundReturnsError)
{
    iv::SocketRpcAckResponseBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_save_project_event,
        iv::SaveProjectRequest{},
        builder);

    auto const response = parse_json_line(builder.build(1));
    EXPECT_EQ(response["error"]["message"], "runtime project event was not handled");
}

TEST_F(ProjectPersistenceTest, ProjectSaveWithNoContributorsWritesEmptyFile)
{
    auto const workspace = mutable_module_fixture_workspace("project_save_empty", "local_cmake");
    auto const startup = make_startup(workspace);
    iv::ProjectPersistence persistence(workspace, startup);
    iv::bind_project_persistence_bridge(persistence);

    iv::SocketRpcAckResponseBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_save_project_event,
        iv::SaveProjectRequest{},
        builder);

    auto const response = parse_json_line(builder.build(1));
    EXPECT_EQ(response["result"]["ok"], true);
    EXPECT_EQ(read_text(workspace / "iv_project.jsonl"), "");

    iv::unbind_project_persistence_bridge(persistence);
}

TEST_F(ProjectPersistenceTest, ProjectSavePersistsCurrentMutatedRuntimeState)
{
    auto const workspace = mutable_module_fixture_workspace("project_save_current_runtime_state", "local_cmake");
    auto const startup = make_startup(workspace);
    iv::IvModuleInstances instances;
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    iv::AudioDeviceLanes audio_device_lanes(48000, 8, make_audio_backend());
    iv::LaneViews lane_views;
    iv::GraphInputLanes graph_input_lanes;
    iv::ProjectPersistence persistence(workspace, startup);

    initialize_two_timeline_lanes(timeline);
    instances.create_instance(workspace, "instance-save");
    execution.set_compiled_sample_cache_chunk_size_multiplier(8);
    (void)audio_device_lanes.set_selected_devices("out-1", "in-1");
    audio_device_lanes.set_lane_external_ids(intern("audio-out-save"), intern("audio-in-save"));
    (void)lane_views.open_view(iv::LaneViewRequest{
        .view_id = intern("view-save"),
        .query = iv::LaneQuery{.filter = iv::LaneQueryFilter{.source = "graph_input"}},
        .start_index = 5,
        .visible_lane_count = 7,
        .first_sample_index = 10,
        .last_sample_index = 30,
        .display_sample_count = 20,
    });
    auto graph_instance = make_instance_with_member_ports();
    auto const graph_runtime_node_id = runtime_node_id(graph_instance.instance_id, "node-1");
    iv::GraphBuilder graph_builder;
    auto graph_node = iv::_annotate_node_source_info(
        graph_builder.node<iv::Sum<1>>().node_ref(),
        "node-1");
    (void)graph_node;
    graph_input_lanes.handle_iv_module_instance_builders_changed(iv::IvModuleInstanceBuildersChanged{
        .created = {iv::IvModuleInstanceBuilderRef{.instance = &graph_instance, .builder = &graph_builder}},
    });
    graph_input_lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 0});
    graph_input_lanes.set_sample_input_value(iv::ProjectSetSampleInputValueRequest{
        .node_id = graph_runtime_node_id,
        .member_ordinal = std::nullopt,
        .input_ordinal = 0,
        .value = iv::Sample{0.25f},
    });

    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
    iv::bind_runtime_project_iv_module_instances_bridge(instances);
    iv::bind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
    iv::bind_runtime_project_graph_input_lanes_bridge(graph_input_lanes);
    iv::bind_runtime_project_lane_views_bridge(lane_views);
    iv::bind_project_persistence_bridge(persistence);

    iv::SocketRpcAckResponseBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_save_project_event,
        iv::SaveProjectRequest{},
        builder);

    auto const response = parse_json_line(builder.build(1));
    EXPECT_EQ(response["result"]["ok"], true);
    auto const commands = parse_project_file(workspace / "iv_project.jsonl");

    auto contains_command = [&](std::string_view command_name) {
        return std::ranges::any_of(commands, [&](auto const &command) {
            return command["command"] == command_name;
        });
    };

    EXPECT_TRUE(contains_command("project.overrideSettings"));
    EXPECT_TRUE(contains_command("audioDevices.setLaneIds"));
    EXPECT_TRUE(contains_command("ivModuleInstances.create"));
    EXPECT_TRUE(contains_command("graph.setSampleInputValue"));
    EXPECT_TRUE(contains_command("timeline.openLaneView"));
    EXPECT_TRUE(std::ranges::any_of(commands, [&](auto const &command) {
        return command["command"] == "graph.setSampleInputValue"
            && command["args"]["node_id"] == graph_runtime_node_id;
    }));

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_lane_views_bridge(lane_views);
    iv::unbind_runtime_project_graph_input_lanes_bridge(graph_input_lanes);
    iv::unbind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
    iv::unbind_runtime_project_iv_module_instances_bridge(instances);
    iv::unbind_runtime_project_timeline_execution_bridge(execution);
}

TEST_F(ProjectPersistenceTest, ProjectSaveContributorExceptionReturnsError)
{
    auto const workspace = mutable_module_fixture_workspace("project_save_contributor_failure", "local_cmake");
    auto const startup = make_startup(workspace);
    iv::ProjectPersistence persistence(workspace, startup);
    iv::bind_project_persistence_bridge(persistence);
    g_throw_on_collect_state = true;

    iv::SocketRpcAckResponseBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_save_project_event,
        iv::SaveProjectRequest{},
        builder);

    auto const response = parse_json_line(builder.build(1));
    EXPECT_EQ(response["error"]["message"], "test contributor failure");

    iv::unbind_project_persistence_bridge(persistence);
}

TEST_F(ProjectPersistenceTest, ProjectSaveWriteFailureReturnsError)
{
    auto const workspace = mutable_module_fixture_workspace("project_save_write_failure", "local_cmake");
    std::filesystem::remove(workspace / "iv_project.jsonl");
    std::filesystem::create_directory(workspace / "iv_project.jsonl");
    auto const startup = make_startup(workspace);
    iv::ProjectPersistence persistence(workspace, startup);
    iv::bind_project_persistence_bridge(persistence);

    iv::SocketRpcAckResponseBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_save_project_event,
        iv::SaveProjectRequest{},
        builder);

    auto const response = parse_json_line(builder.build(1));
    EXPECT_TRUE(
        response["error"]["message"].get<std::string>().contains("failed to replace"));

    iv::unbind_project_persistence_bridge(persistence);
}

TEST_F(ProjectPersistenceTest, RepeatedProjectSaveIsIdempotent)
{
    auto const workspace = mutable_module_fixture_workspace("project_save_idempotent", "local_cmake");
    auto const startup = make_startup(workspace);
    iv::IvModuleInstances instances;
    instances.create_instance(workspace, "instance-a");
    iv::ProjectPersistence persistence(workspace, startup);

    iv::bind_runtime_project_iv_module_instances_bridge(instances);
    iv::bind_project_persistence_bridge(persistence);

    iv::SocketRpcAckResponseBuilder builder1;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_save_project_event,
        iv::SaveProjectRequest{},
        builder1);
    auto const first = read_text(workspace / "iv_project.jsonl");

    iv::SocketRpcAckResponseBuilder builder2;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_save_project_event,
        iv::SaveProjectRequest{},
        builder2);
    EXPECT_EQ(read_text(workspace / "iv_project.jsonl"), first);

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_iv_module_instances_bridge(instances);
}

TEST(Uuid, InterningAndUuidGenerationBehavior)
{
    auto const a = iv::InternedString::from_view("alpha");
    auto const a2 = iv::InternedString::from_string("alpha");
    auto const b = iv::InternedString::from_view("beta");
    EXPECT_EQ(a, a2);
    EXPECT_EQ(a.c_str(), a2.c_str());
    EXPECT_NE(a, b);

    std::regex uuid_re(
        "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    std::set<std::string> values;
    for (size_t i = 0; i < 256; ++i) {
        auto const uuid = iv::generate_uuid_v4().str();
        EXPECT_TRUE(std::regex_match(uuid, uuid_re));
        values.insert(uuid);
    }
    EXPECT_EQ(values.size(), 256u);
}

TEST_F(ProjectPersistenceTest, SavedIdsAreReusedAcrossLoadAndResave)
{
    auto const workspace = mutable_module_fixture_workspace("project_saved_ids_reused", "local_cmake");
    write_text(
        workspace / "iv_project.jsonl",
        Json{
            {"command", "audioDevices.setLaneIds"},
            {"args", Json{
                {"output_lane_id", "audio-out-fixed"},
                {"input_lane_id", "audio-in-fixed"},
            }},
        }.dump() + "\n" +
        Json{
            {"command", "timeline.openLaneView"},
            {"args", Json{
                {"view_id", "view-fixed"},
                {"query_source", "graph_input"},
                {"start_index", 1},
                {"visible_lane_count", 2},
                {"first_sample_index", 0},
                {"last_sample_index", 8},
                {"display_sample_count", 8},
            }},
        }.dump() + "\n");

    auto const startup = make_startup(workspace);
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    iv::AudioDeviceLanes audio_device_lanes(48000, 8, make_audio_backend());
    iv::LaneViews lane_views;
    iv::ProjectPersistence persistence(workspace, startup);
    initialize_two_timeline_lanes(timeline);

    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
    iv::bind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
    iv::bind_runtime_project_lane_views_bridge(lane_views);
    iv::bind_project_persistence_bridge(persistence);

    persistence.load();
    persistence.save();

    auto const commands = parse_project_file(workspace / "iv_project.jsonl");
    auto const lane_id_command = std::ranges::find_if(commands, [](auto const &command) {
        return command["command"] == "audioDevices.setLaneIds";
    });
    ASSERT_NE(lane_id_command, commands.end());
    EXPECT_EQ((*lane_id_command)["args"]["output_lane_id"], "audio-out-fixed");
    EXPECT_EQ((*lane_id_command)["args"]["input_lane_id"], "audio-in-fixed");

    auto const view_command = std::ranges::find_if(commands, [](auto const &command) {
        return command["command"] == "timeline.openLaneView";
    });
    ASSERT_NE(view_command, commands.end());
    EXPECT_EQ((*view_command)["args"]["view_id"], "view-fixed");

    EXPECT_EQ(audio_device_lanes.output_lane_external_id().str(), "audio-out-fixed");
    EXPECT_EQ(audio_device_lanes.input_lane_external_id().str(), "audio-in-fixed");
    ASSERT_EQ(lane_views.active_view_requests().size(), 1u);
    EXPECT_EQ(lane_views.active_view_requests().front().view_id.str(), "view-fixed");

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_lane_views_bridge(lane_views);
    iv::unbind_runtime_project_audio_device_lanes_bridge(audio_device_lanes);
    iv::unbind_runtime_project_timeline_execution_bridge(execution);
}
