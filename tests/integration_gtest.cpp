#include "runtime_test_harness.h"

#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes_timeline_bridge.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_instances_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_reload_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_iv_module_definitions_bridge.h>
#include <intravenous/runtime/iv_module_instances_graph_input_lanes_bridge.h>
#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/iv_module_reload_events.h>
#include <intravenous/runtime/iv_module_reload_iv_module_definitions_bridge.h>
#include <intravenous/runtime/lane_filters.h>
#include <intravenous/runtime/lane_filters_lane_views_bridge.h>
#include <intravenous/runtime/lane_views.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/startup_config.h>
#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_execution.h>
#include <intravenous/runtime/timeline_execution_task_runner_bridge.h>
#include <intravenous/runtime/timeline_lane_filters_bridge.h>
#include <intravenous/runtime/timeline_timeline_execution_bridge.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <numbers>
#include <thread>

namespace {
using iv::test_support::read_only_module_fixture_workspace;
using iv::test_support::shared_inline_module_workspace;
using namespace std::chrono_literals;

iv::InternedString intern(std::string_view value)
{
    return iv::InternedString::from_view(value);
}

struct IntegrationReloadWitness {
    std::optional<iv::IvModuleReloadResults> results {};
};

IntegrationReloadWitness *g_integration_reload_witness = nullptr;
struct IntegrationPassFinishedAction {
    std::mutex mutex;
    std::function<void(iv::TasksRunnerAfterPass const &)> fn {};
};

IntegrationPassFinishedAction *g_integration_pass_finished_action = nullptr;

IV_SUBSCRIBE_LINKER_EVENT(
    iv::IvModuleReloadResultsEvent,
    iv_runtime_iv_module_reload_results_event,
    +[](iv::IvModuleReloadResults const &results) {
        if (g_integration_reload_witness != nullptr) {
            g_integration_reload_witness->results = results;
        }
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::TasksRunnerAfterPassEvent,
    iv_runtime_task_runner_after_pass_event,
    +[](iv::TasksRunnerAfterPass const &finished) {
        if (g_integration_pass_finished_action == nullptr) {
            return;
        }
        std::function<void(iv::TasksRunnerAfterPass const &)> fn;
        {
            std::scoped_lock lock(g_integration_pass_finished_action->mutex);
            fn = std::move(g_integration_pass_finished_action->fn);
        }
        if (fn) {
            fn(finished);
        }
    });

bool wait_until(std::function<bool()> const &predicate, std::chrono::milliseconds timeout = 2s)
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

struct AudioBufferCaptureState {
    mutable std::mutex mutex;
    std::vector<iv::Sample::storage> latest {};
    std::atomic<size_t> captures {0};

    void store(std::vector<iv::Sample::storage> values)
    {
        {
            std::scoped_lock lock(mutex);
            latest = std::move(values);
        }
        captures.fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<iv::Sample::storage> snapshot() const
    {
        std::scoped_lock lock(mutex);
        return latest;
    }
};

struct TestRealtimeSineLaneState {
    double phase = 0.0;
};

struct TestRealtimeSineLaneNode {
    std::shared_ptr<TestRealtimeSineLaneState> state {};
    double frequency_hz = 440.0;
    double sample_rate_hz = 48000.0;

    iv::RealtimeSampleLaneOutputConfig output() const
    {
        return {.name = "samples"};
    }

    void tick_block_realtime(iv::RealtimeLaneTickContext<TestRealtimeSineLaneNode> &ctx)
    {
        auto &shared = *state;
        auto const phase_step = std::numbers::pi * 2.0 * frequency_hz / sample_rate_hz;
        auto out = ctx.out().block_view();
        for (size_t i = 0; i < ctx.sample_count(); ++i) {
            out.set(i, 0, static_cast<iv::Sample>(std::sin(shared.phase)));
            shared.phase += phase_step;
            if (shared.phase >= std::numbers::pi * 2.0) {
                shared.phase -= std::numbers::pi * 2.0;
            }
        }
    }
};

struct TestAudioBufferSinkLaneNode {
    std::shared_ptr<AudioBufferCaptureState> capture {};

    std::array<iv::RealtimeSampleLaneInputConfig, 1> realtime_sample_inputs() const
    {
        return {iv::RealtimeSampleLaneInputConfig{.name = "source"}};
    }

    iv::RealtimeSampleLaneOutputConfig output() const
    {
        return {.name = "samples"};
    }

    void tick_block_realtime(iv::RealtimeLaneTickContext<TestAudioBufferSinkLaneNode> &ctx)
    {
        std::vector<iv::Sample::storage> latest;
        latest.reserve(ctx.sample_count());
        auto const connected = !ctx.realtime_sample_inputs().empty() && ctx.realtime_sample_input(0).connected();
        auto const block = connected
            ? ctx.realtime_sample_input(0).block_view()
            : iv::SampleBlockView<iv::Sample const>{};
        auto out = ctx.out().block_view();

        for (size_t i = 0; i < ctx.sample_count(); ++i) {
            auto const sample = i < block.frames() ? block.get(i, 0) : iv::Sample{};
            out.set(i, 0, sample);
            latest.push_back(sample.value);
        }
        capture->store(std::move(latest));
    }
};

void apply_timeline_batch_to_execution_and_runner(
    iv::Timeline &timeline,
    iv::TimelineExecution &execution,
    iv::TasksRunner &runner,
    iv::TimelineLaneBatchUpdate const &batch)
{
    std::vector<iv::LaneId> created_lanes;
    std::vector<iv::LaneId> changed_lanes;
    std::vector<iv::LaneId> removed_lanes = batch.removals;

    for (auto const &upsert : batch.upserts) {
        if (timeline.contains_lane(upsert.lane)) {
            changed_lanes.push_back(upsert.lane);
        } else {
            created_lanes.push_back(upsert.lane);
        }
    }
    for (auto const &connection : batch.connections_to_add) {
        changed_lanes.push_back(connection.target);
    }
    for (auto const &connection : batch.connections_to_remove) {
        changed_lanes.push_back(connection.target);
    }

    std::ranges::sort(changed_lanes, [](auto lhs, auto rhs) {
        return lhs.value < rhs.value;
    });
    changed_lanes.erase(
        std::unique(changed_lanes.begin(), changed_lanes.end()),
        changed_lanes.end());
    changed_lanes.erase(
        std::remove_if(
            changed_lanes.begin(),
            changed_lanes.end(),
            [&](iv::LaneId lane) {
                return std::ranges::find(created_lanes, lane) != created_lanes.end();
            }),
        changed_lanes.end());

    timeline.apply_lane_batch(batch);
    runner.update_tasks(execution.handle_timeline_lanes_changed(
        iv::TimelineLanesChanged{
            .visit_lanes = [&](std::vector<iv::LaneId> const &lanes, iv::TimelineLaneVisitFn const &visit) {
                timeline.with_graph([&](iv::LaneGraph const &graph) {
                    for (auto const lane : lanes) {
                        if (!graph.contains(lane)) {
                            continue;
                        }
                        auto const &record = graph.lane(lane);
                        visit(
                            lane,
                            record.node,
                            record.output,
                            record.sample_channel_type,
                            graph.inputs_for(lane),
                            record.external_task_dependencies);
                    }
                });
            },
            .created_lanes = std::move(created_lanes),
            .removed_lanes = std::move(removed_lanes),
            .changed_lanes = std::move(changed_lanes),
        }));
}
}

TEST(Integration, StartupConfigDefinitionsAndIvModuleSourceIntrospectionInitializeAndShutdown)
{
    auto const workspace =
        read_only_module_fixture_workspace("local_cmake");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvModuleDefinitions definitions;
    iv::IvModuleSourceIntrospection introspection;
    iv::bind_iv_module_definitions_iv_module_source_introspection_bridge(introspection);

    auto const loaded = iv::test_support::BoundIvModuleSourceIntrospection::load_definition(
        startup,
        std::filesystem::weakly_canonical(workspace));
    auto const module_id = loaded.module_id;
    definitions.seed_loaded_definition(std::move(loaded));
    auto const initialized = introspection.initialize();

    EXPECT_EQ(initialized.module_id, module_id);

    iv::unbind_iv_module_definitions_iv_module_source_introspection_bridge(introspection);
}

TEST(Integration, InstancesDefinitionsReloadAndGraphInputLanesInitializeAndShutdown)
{
    auto const workspace = shared_inline_module_workspace(
        "runtime_integration_graph_input_lanes_initialize",
        R"(#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>

namespace {
    void graph_input_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const dt = g.node<ValueSource>(&context.sample_period());
        auto const voices = iv::polyphonic<2>(g, [&](auto m) {
            auto const saw = g.node<SawOscillator>();
            saw(
                "phase_offset"_P = 0.0,
                "frequency"_P = 440.0,
                "dt"_P = dt
            );
            return saw * ("amplitude"_P << m);
        });
        g.outputs(channels::mono = voices);
    }
}

IV_EXPORT_MODULE("iv.test.graph_input_module", graph_input_module);
)");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::Timeline timeline;
    iv::IvModuleInstances instances;
    iv::IvModuleDefinitions definitions;
    iv::IvModuleReload reload(startup);
    iv::GraphInputLanes graph_input_lanes;
    iv::LaneFilters lane_filters;
    iv::LaneViews lane_views;

    iv::bind_graph_input_lanes_timeline_bridge(graph_input_lanes, timeline);
    iv::bind_timeline_lane_filters_bridge(lane_filters);
    iv::bind_lane_filters_lane_views_bridge(lane_filters, lane_views);
    iv::bind_iv_module_instances_iv_module_definitions_bridge(definitions);
    iv::bind_iv_module_definitions_iv_module_instances_bridge(instances);
    iv::bind_iv_module_definitions_iv_module_reload_bridge(reload);
    iv::bind_iv_module_reload_iv_module_definitions_bridge(definitions);
    iv::bind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);

    IntegrationReloadWitness reload_witness;
    g_integration_reload_witness = &reload_witness;
    (void)instances.create_instance(std::filesystem::weakly_canonical(workspace));
    g_integration_reload_witness = nullptr;

    ASSERT_TRUE(reload_witness.results.has_value());
    ASSERT_TRUE(reload_witness.results->failed.empty())
        << reload_witness.results->failed.front().message;

    auto const loaded_definitions = definitions.loaded_definitions();
    ASSERT_EQ(loaded_definitions.size(), 1u);
    EXPECT_FALSE(loaded_definitions.front().introspection.logical_nodes.empty());

    auto const logical_node_it = std::find_if(
        loaded_definitions.front().introspection.logical_nodes.begin(),
        loaded_definitions.front().introspection.logical_nodes.end(),
        [](auto const &node) {
            return !node.sample_inputs.empty();
        });
    ASSERT_NE(logical_node_it, loaded_definitions.front().introspection.logical_nodes.end());

    graph_input_lanes.set_sample_input_state(iv::ProjectSetSampleInputStateRequest{
        .node_id = logical_node_it->id,
        .member_ordinal = std::nullopt,
        .input_ordinal = 0,
        .state = iv::ProjectSampleInputState::timeline_lane,
    });
    graph_input_lanes.handle_task_runner_after_pass(iv::TasksRunnerAfterPass{.graph_revision = 1});

    auto const view = lane_views.open_view(iv::LaneViewRequest{
        .view_id = intern("view"),
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.source = "dsp_graph.graph_input"},
        },
    });
    EXPECT_GT(view.lanes.total_lane_count, 0u);

    iv::unbind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
    iv::unbind_iv_module_reload_iv_module_definitions_bridge(definitions);
    iv::unbind_iv_module_definitions_iv_module_reload_bridge(reload);
    iv::unbind_iv_module_definitions_iv_module_instances_bridge(instances);
    iv::unbind_iv_module_instances_iv_module_definitions_bridge(definitions);
    iv::unbind_lane_filters_lane_views_bridge(lane_filters, lane_views);
    iv::unbind_timeline_lane_filters_bridge(lane_filters);
    iv::unbind_graph_input_lanes_timeline_bridge(graph_input_lanes, timeline);
}

TEST(Integration, SampleInputMutationsFlowThroughLiveSnapshots)
{
    auto const workspace = shared_inline_module_workspace(
        "runtime_integration_live_input_snapshots",
        R"(#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>

void polyphonic_module(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const dt = g.node<ValueSource>(&context.sample_period());
    auto const voices = iv::polyphonic<2>(g, [&](auto m) {
        auto const saw = g.node<SawOscillator>();
        saw(
            "phase_offset"_P = 0.0,
            "frequency"_P = 440.0,
            "dt"_P = dt
        );
        return saw * ("amplitude"_P << m);
    });
    g.outputs(channels::mono = voices);
}

IV_EXPORT_MODULE("iv.test.polyphonic_module", polyphonic_module);
)");

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    iv::test_support::BoundIvModuleSourceIntrospection app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 11, .column = 20}, .end = {.line = 11, .column = 20}}});
    ASSERT_EQ(result.nodes.size(), 1u);
    auto const logical_id = result.nodes.front().id;

    app.set_sample_input_value(logical_id, 1, 0.25f);
    auto const logical_override = app.get_logical_node(logical_id);
    EXPECT_FLOAT_EQ(static_cast<float>(logical_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(logical_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(logical_override.members[1].sample_inputs[1].current_value), 0.25f);

    app.set_sample_input_value(logical_id, 1, 0.75f, 1u);
    auto const concrete_override = app.get_logical_node(logical_id);
    EXPECT_TRUE(concrete_override.members[1].sample_inputs[1].has_concrete_override);
    EXPECT_FLOAT_EQ(
        static_cast<float>(concrete_override.members[1].sample_inputs[1].current_value), 0.75f);

    app.set_sample_input_state(
        logical_id,
        1,
        iv::ProjectSampleInputState::logical_follow,
        1u);
    auto const cleared_override = app.get_logical_node(logical_id);
    EXPECT_FALSE(cleared_override.members[1].sample_inputs[1].has_concrete_override);
    EXPECT_FLOAT_EQ(
        static_cast<float>(cleared_override.members[1].sample_inputs[1].current_value), 0.25f);
}

TEST(Integration, TimelineRealtimeSinewaveReachesAudioBuffer)
{
    auto sine_state = std::make_shared<TestRealtimeSineLaneState>();
    auto capture_state = std::make_shared<AudioBufferCaptureState>();

    iv::Timeline timeline;
    iv::TimelineExecution execution(64);
    iv::TasksRunner runner(1);
    iv::LaneIdAllocator lane_ids;

    auto const source_lane = lane_ids.next();
    auto const sink_lane = lane_ids.next();
    apply_timeline_batch_to_execution_and_runner(timeline, execution, runner, iv::TimelineLaneBatchUpdate{
        .upserts = {
            iv::TimelineLaneUpsert{
                .lane = source_lane,
                .make_node = [sine_state] {
                    return iv::TypeErasedLaneNode(TestRealtimeSineLaneNode{
                        .state = sine_state,
                        .frequency_hz = 440.0,
                        .sample_rate_hz = 48000.0,
                    });
                },
            },
            iv::TimelineLaneUpsert{
                .lane = sink_lane,
                .make_node = [capture_state] {
                    return iv::TypeErasedLaneNode(TestAudioBufferSinkLaneNode{
                        .capture = capture_state,
                    });
                },
            },
        },
        .connections_to_add = {
            iv::LaneGraphConnection{
                .source = source_lane,
                .target = sink_lane,
                .input = iv::realtime_sample_input(0),
            },
        },
    });

    ASSERT_TRUE(wait_until([&] {
        return capture_state->captures.load(std::memory_order_relaxed) >= 2;
    })) << "expected audio buffer sink to receive realtime blocks";

    auto const latest = capture_state->snapshot();
    ASSERT_EQ(latest.size(), 64u);
    EXPECT_TRUE(std::any_of(latest.begin(), latest.end(), [](auto sample) {
        return std::abs(sample) > 0.0001f;
    }));
    EXPECT_TRUE(std::adjacent_find(latest.begin(), latest.end(), std::not_equal_to<>{}) != latest.end());
    auto const max_it = std::max_element(latest.begin(), latest.end(), [](auto a, auto b) {
        return std::abs(a) < std::abs(b);
    });
    ASSERT_NE(max_it, latest.end());
    EXPECT_LE(std::abs(*max_it), 1.0f);
}

TEST(Integration, DisconnectingTimelineSinewaveSilencesAudioBuffer)
{
    auto sine_state = std::make_shared<TestRealtimeSineLaneState>();
    auto capture_state = std::make_shared<AudioBufferCaptureState>();

    iv::Timeline timeline;
    iv::TimelineExecution execution(64);
    iv::TasksRunner runner(1);
    iv::LaneIdAllocator lane_ids;

    auto const source_lane = lane_ids.next();
    auto const sink_lane = lane_ids.next();
    auto const connection = iv::LaneGraphConnection{
        .source = source_lane,
        .target = sink_lane,
        .input = iv::realtime_sample_input(0),
    };
    apply_timeline_batch_to_execution_and_runner(timeline, execution, runner, iv::TimelineLaneBatchUpdate{
        .upserts = {
            iv::TimelineLaneUpsert{
                .lane = source_lane,
                .make_node = [sine_state] {
                    return iv::TypeErasedLaneNode(TestRealtimeSineLaneNode{
                        .state = sine_state,
                        .frequency_hz = 440.0,
                        .sample_rate_hz = 48000.0,
                    });
                },
            },
            iv::TimelineLaneUpsert{
                .lane = sink_lane,
                .make_node = [capture_state] {
                    return iv::TypeErasedLaneNode(TestAudioBufferSinkLaneNode{
                        .capture = capture_state,
                    });
                },
            },
        },
        .connections_to_add = {connection},
    });

    ASSERT_TRUE(wait_until([&] {
        return capture_state->captures.load(std::memory_order_relaxed) >= 2;
    }));

    auto const captures_before_disconnect =
        capture_state->captures.load(std::memory_order_relaxed);
    std::atomic<bool> disconnect_applied {false};
    IntegrationPassFinishedAction pass_finished_action;
    g_integration_pass_finished_action = &pass_finished_action;
    {
        std::scoped_lock lock(pass_finished_action.mutex);
        pass_finished_action.fn =
            [&](iv::TasksRunnerAfterPass const &) {
                apply_timeline_batch_to_execution_and_runner(
                    timeline,
                    execution,
                    runner,
                    iv::TimelineLaneBatchUpdate{
                    .connections_to_remove = {connection},
                });
                disconnect_applied.store(true, std::memory_order_relaxed);
            };
    }

    ASSERT_TRUE(wait_until([&] {
        return disconnect_applied.load(std::memory_order_relaxed);
    })) << "expected disconnect to be applied on a pass boundary";

    ASSERT_TRUE(wait_until([&] {
        return capture_state->captures.load(std::memory_order_relaxed) > captures_before_disconnect;
    })) << "expected sink to receive a block after disconnect";

    auto const latest = capture_state->snapshot();
    ASSERT_EQ(latest.size(), 64u);
    EXPECT_TRUE(std::all_of(latest.begin(), latest.end(), [](auto sample) {
        return std::abs(sample) < 0.000001f;
    }));

    g_integration_pass_finished_action = nullptr;
}
