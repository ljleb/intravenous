#include <intravenous/basic_lane_nodes/controls.h>
#include <intravenous/basic_lane_nodes/type_erased.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_execution.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>

namespace {
    struct TaskState {
        std::vector<std::string> depends_on {};
        iv::TaskCallback callback {};
    };

    class TaskGraphHarness {
        std::unordered_map<std::string, TaskState> tasks_ {};

    public:
        void apply(iv::TaskGraphUpdate const& update)
        {
            for (auto const& id : update.to_delete) {
                tasks_.erase(id);
            }
            for (auto const& task : update.to_create) {
                tasks_[task.id] = TaskState {
                    .depends_on = task.depends_on,
                    .callback = task.callback,
                };
            }
            for (auto const& task : update.to_update) {
                auto& stored = tasks_[task.id];
                if (task.depends_on) {
                    stored.depends_on = *task.depends_on;
                }
                if (task.callback) {
                    stored.callback = *task.callback;
                }
            }
        }

        void run_once() const
        {
            std::unordered_map<std::string, std::size_t> indegree {};
            std::unordered_map<std::string, std::vector<std::string>> users {};
            for (auto const& [id, task] : tasks_) {
                indegree[id] = 0;
            }
            for (auto const& [id, task] : tasks_) {
                for (auto const& dependency : task.depends_on) {
                    if (!tasks_.contains(dependency)) {
                        continue;
                    }
                    ++indegree[id];
                    users[dependency].push_back(id);
                }
            }

            std::priority_queue<
                std::string,
                std::vector<std::string>,
                std::greater<>
            > ready;
            for (auto const& [id, degree] : indegree) {
                if (degree == 0) {
                    ready.push(id);
                }
            }

            while (!ready.empty()) {
                auto id = ready.top();
                ready.pop();
                auto const& task = tasks_.at(id);
                task.callback.invoke(task.callback.context);
                if (auto user_it = users.find(id); user_it != users.end()) {
                    for (auto const& user : user_it->second) {
                        auto& degree = indegree[user];
                        if (--degree == 0) {
                            ready.push(user);
                        }
                    }
                }
            }
        }
    };

    struct TestCompiledSampleSourceLaneNode {
        int* tick_count = nullptr;

        iv::CompiledSampleLaneOutputConfig output() const
        {
            return {
                .name = "samples",
            };
        }

        std::vector<iv::CompiledSupportRange> compiled_support_ranges(
            iv::CompiledSupportContext<TestCompiledSampleSourceLaneNode>&) const
        {
            return { iv::CompiledSupportRange { .start_index = 0, .end_index = 4096 } };
        }

        void tick_block_compiled(iv::CompiledLaneTickContext<TestCompiledSampleSourceLaneNode>& ctx)
        {
            if (tick_count) {
                *tick_count += 1;
            }
            std::vector<iv::Sample> samples(ctx.sample_count());
            for (size_t i = 0; i < samples.size(); ++i) {
                samples[i] = static_cast<iv::Sample>(ctx.start_index() + i);
            }
            ctx.out().write(ctx.start_index(), samples);
        }
    };

    struct TestCompiledEventSourceLaneNode {
        iv::CompiledEventLaneOutputConfig output() const
        {
            return {
                .name = "events",
                .event_type = iv::EventTypeId::trigger,
            };
        }

        std::vector<iv::CompiledSupportRange> compiled_support_ranges(
            iv::CompiledSupportContext<TestCompiledEventSourceLaneNode>&) const
        {
            return { iv::CompiledSupportRange { .start_index = 0, .end_index = 4096 } };
        }

        void tick_block_compiled(iv::CompiledLaneTickContext<TestCompiledEventSourceLaneNode>& ctx)
        {
            ctx.out().push(iv::TimedEvent {
                .time = ctx.start_index(),
                .value = iv::TriggerEvent {},
            });
        }
    };

    struct TestSparseCompiledSampleLaneNode {
        int* tick_count = nullptr;

        iv::CompiledSampleLaneOutputConfig output() const
        {
            return {
                .name = "samples",
            };
        }

        std::vector<iv::CompiledSupportRange> compiled_support_ranges(
            iv::CompiledSupportContext<TestSparseCompiledSampleLaneNode>&) const
        {
            return { iv::CompiledSupportRange { .start_index = 8, .end_index = 12 } };
        }

        void tick_block_compiled(iv::CompiledLaneTickContext<TestSparseCompiledSampleLaneNode>& ctx)
        {
            if (tick_count) {
                *tick_count += 1;
            }
            std::vector<iv::Sample> samples(ctx.sample_count(), 7.0f);
            ctx.out().write(ctx.start_index(), samples);
        }
    };

    struct TestChunkedCompiledSampleLaneNode {
        int* tick_count = nullptr;

        iv::CompiledSampleLaneOutputConfig output() const
        {
            return {
                .name = "samples",
            };
        }

        std::vector<iv::CompiledSupportRange> compiled_support_ranges(
            iv::CompiledSupportContext<TestChunkedCompiledSampleLaneNode>&) const
        {
            return { iv::CompiledSupportRange { .start_index = 8, .end_index = 24 } };
        }

        void tick_block_compiled(iv::CompiledLaneTickContext<TestChunkedCompiledSampleLaneNode>& ctx)
        {
            if (tick_count) {
                *tick_count += 1;
            }
            std::vector<iv::Sample> samples(ctx.sample_count());
            for (size_t i = 0; i < samples.size(); ++i) {
                samples[i] = static_cast<iv::Sample>(ctx.start_index() + i);
            }
            ctx.out().write(ctx.start_index(), samples);
        }
    };
}

TEST(TimelineExecution, SynchronizeTracksRealtimeSampleOutputLanes)
{
    iv::Timeline timeline;
    auto const source = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode {
            .value = 440.0f,
        }));
    });
    auto const target = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(iv::GraphSampleInputLaneNode {
            .default_value = 0.0f,
        }));
    });
    timeline.apply_lane_batch(iv::TimelineLaneBatchUpdate {
        .connections_to_add = {
            iv::LaneGraphConnection {
                .source = source,
                .target = target,
                .input = iv::realtime_sample_input(),
            },
        },
    });

    iv::TimelineExecution execution(8);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });

    auto lanes = execution.realtime_sample_output_lanes();
    std::sort(lanes.begin(), lanes.end(), [](iv::LaneId lhs, iv::LaneId rhs) {
        return lhs.value < rhs.value;
    });
    ASSERT_EQ(lanes.size(), 2u);
    EXPECT_EQ(lanes[0], source);
    EXPECT_EQ(lanes[1], target);
}

TEST(TimelineExecution, ExecutesRealtimeSampleKnobChainForward)
{
    iv::Timeline timeline;
    auto const logical = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode {
            .value = 440.0f,
        }));
    });
    auto const concrete = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode {
            .value = 220.0f,
        }));
    });
    timeline.apply_lane_batch(iv::TimelineLaneBatchUpdate {
        .connections_to_add = {
            iv::LaneGraphConnection {
                .source = logical,
                .target = concrete,
                .input = iv::realtime_sample_input(),
            },
        },
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });
    execution.set_realtime_start_index(64);
    harness.run_once();

    auto const logical_block = execution.realtime_sample_block(logical);
    auto const concrete_block = execution.realtime_sample_block(concrete);
    ASSERT_EQ(logical_block.size(), 4u);
    ASSERT_EQ(concrete_block.size(), 4u);
    EXPECT_EQ(std::vector<iv::Sample>(logical_block.begin(), logical_block.end()),
        (std::vector<iv::Sample> { 440.0f, 440.0f, 440.0f, 440.0f }));
    EXPECT_EQ(std::vector<iv::Sample>(concrete_block.begin(), concrete_block.end()),
        (std::vector<iv::Sample> { 440.0f, 440.0f, 440.0f, 440.0f }));
}

TEST(TimelineExecution, ReadsCompiledSampleStorageIntoRealtimeSampleInputs)
{
    iv::Timeline timeline;
    int compiled_ticks = 0;
    iv::LaneId compiled_source {};
    iv::LaneId realtime_target {};
    timeline.with_graph([&](iv::LaneGraph& graph) {
        compiled_source = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledSampleSourceLaneNode {
            .tick_count = &compiled_ticks,
        }));
        realtime_target = graph.add_lane(iv::TypeErasedLaneNode(iv::GraphSampleInputLaneNode {
            .default_value = -1.0f,
        }));
        graph.connect(compiled_source, realtime_target, iv::realtime_sample_input());
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });
    execution.set_realtime_start_index(8);
    harness.run_once();

    auto const target_block = execution.realtime_sample_block(realtime_target);
    ASSERT_EQ(target_block.size(), 4u);
    EXPECT_EQ(std::vector<iv::Sample>(target_block.begin(), target_block.end()),
        (std::vector<iv::Sample> { 8.0f, 9.0f, 10.0f, 11.0f }));

    auto const compiled_block = execution.compiled_sample_block(compiled_source, 8);
    ASSERT_EQ(compiled_block.size(), 4u);
    EXPECT_EQ(compiled_block,
        (std::vector<iv::Sample> { 8.0f, 9.0f, 10.0f, 11.0f }));
    EXPECT_EQ(compiled_ticks, 1);

    execution.set_realtime_start_index(8);
    harness.run_once();
    EXPECT_EQ(compiled_ticks, 1);
}

TEST(TimelineExecution, ReadsCompiledEventStorageIntoRealtimeEventInputs)
{
    iv::Timeline timeline;
    iv::LaneId compiled_source {};
    iv::LaneId realtime_target {};
    timeline.with_graph([&](iv::LaneGraph& graph) {
        compiled_source = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledEventSourceLaneNode {}));
        realtime_target = graph.add_lane(iv::TypeErasedLaneNode(iv::GraphEventInputLaneNode {}));
        graph.connect(compiled_source, realtime_target, iv::realtime_event_input());
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });
    execution.set_realtime_start_index(32);
    harness.run_once();

    auto const target_events = execution.realtime_event_block(realtime_target);
    ASSERT_EQ(target_events.size(), 1u);
    EXPECT_EQ(target_events[0].time, 32u);
    EXPECT_TRUE(std::holds_alternative<iv::TriggerEvent>(target_events[0].value));

    auto const compiled_events = execution.compiled_event_block(compiled_source, 32);
    ASSERT_EQ(compiled_events.size(), 1u);
    EXPECT_EQ(compiled_events[0].time, 32u);
}

TEST(TimelineExecution, LaneChangeInvalidatesCompiledCache)
{
    iv::Timeline timeline;
    auto const compiled_source = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(TestCompiledSampleSourceLaneNode {}));
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });

    execution.set_realtime_start_index(16);
    harness.run_once();
    auto const first_block = execution.compiled_sample_block(compiled_source, 16);
    ASSERT_EQ(first_block.size(), 4u);
    EXPECT_EQ(first_block[0], 16.0f);

    timeline.apply_lane_batch(iv::TimelineLaneBatchUpdate {
        .upserts = {
            iv::TimelineLaneUpsert {
                .lane = compiled_source,
                .make_node = [] {
                    return iv::TypeErasedLaneNode(TestCompiledSampleSourceLaneNode {});
                },
            },
        },
    });

    iv::TimelineLanesChanged change {
        .lane_set_changed = false,
        .visit_lanes = [&timeline](std::vector<iv::LaneId> const& lanes, iv::TimelineLaneVisitFn const& visit) {
            timeline.with_graph([&](iv::LaneGraph const& graph) {
                for (auto const lane : lanes) {
                    auto const& record = graph.lane(lane);
                    visit(lane, record.node, record.output, graph.inputs_for(lane));
                }
            });
        },
        .changed_lanes = { compiled_source },
    };
    harness.apply(execution.handle_timeline_lanes_changed(change));

    execution.set_realtime_start_index(20);
    harness.run_once();
    auto const second_block = execution.compiled_sample_block(compiled_source, 20);
    ASSERT_EQ(second_block.size(), 4u);
    EXPECT_EQ(second_block[0], 20.0f);
}

TEST(TimelineExecution, CompiledSupportSkipsExecutionOutsideSupport)
{
    iv::Timeline timeline;
    int compiled_ticks = 0;
    auto const compiled_source = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(TestSparseCompiledSampleLaneNode {
            .tick_count = &compiled_ticks,
        }));
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });

    auto const outside_support = execution.compiled_sample_block(compiled_source, 0);
    ASSERT_EQ(outside_support.size(), 4u);
    EXPECT_EQ(
        outside_support,
        (std::vector<iv::Sample> { 0.0f, 0.0f, 0.0f, 0.0f }));
    EXPECT_EQ(compiled_ticks, 0);

    auto const inside_support = execution.compiled_sample_block(compiled_source, 8);
    ASSERT_EQ(inside_support.size(), 4u);
    EXPECT_EQ(
        inside_support,
        (std::vector<iv::Sample> { 7.0f, 7.0f, 7.0f, 7.0f }));
    EXPECT_EQ(compiled_ticks, 1);
}

TEST(TimelineExecution, ReusesCompiledSampleChunksWithinChunkBoundaries)
{
    iv::Timeline timeline;
    int compiled_ticks = 0;
    auto const compiled_source = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(TestChunkedCompiledSampleLaneNode {
            .tick_count = &compiled_ticks,
        }));
    });

    iv::TimelineExecution execution(4, 2);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });

    auto const first_block = execution.compiled_sample_block(compiled_source, 8);
    ASSERT_EQ(first_block.size(), 4u);
    EXPECT_EQ(
        first_block,
        (std::vector<iv::Sample> { 8.0f, 9.0f, 10.0f, 11.0f }));
    EXPECT_EQ(compiled_ticks, 1);

    auto const same_chunk_block = execution.compiled_sample_block(compiled_source, 12);
    ASSERT_EQ(same_chunk_block.size(), 4u);
    EXPECT_EQ(
        same_chunk_block,
        (std::vector<iv::Sample> { 12.0f, 13.0f, 14.0f, 15.0f }));
    EXPECT_EQ(compiled_ticks, 1);

    auto const next_chunk_block = execution.compiled_sample_block(compiled_source, 16);
    ASSERT_EQ(next_chunk_block.size(), 4u);
    EXPECT_EQ(
        next_chunk_block,
        (std::vector<iv::Sample> { 16.0f, 17.0f, 18.0f, 19.0f }));
    EXPECT_EQ(compiled_ticks, 2);
}

TEST(TimelineExecution, RejectsZeroChunkSizeMultiplier)
{
    EXPECT_THROW(
        (void)iv::TimelineExecution(8, 0),
        std::runtime_error);
}
