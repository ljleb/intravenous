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
#include <vector>

namespace {
    struct TaskState {
        std::vector<std::string> depends_on {};
        iv::TaskCallback callback {};
    };

    class TaskGraphHarness {
        std::unordered_map<std::string, TaskState> tasks_ {};

    public:
        void apply(iv::VersionedTaskGraphUpdate const& update)
        {
            for (auto const& id : update.update.to_delete) {
                tasks_.erase(id);
            }
            for (auto const& task : update.update.to_create) {
                tasks_[task.id] = TaskState {
                    .depends_on = task.depends_on,
                    .callback = task.callback,
                };
            }
            for (auto const& task : update.update.to_update) {
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

    std::vector<iv::Sample> sample_values(iv::BorrowedSampleBlock const& block)
    {
        auto const view = block.view();
        std::vector<iv::Sample> values;
        values.reserve(view.frames() * view.channels());
        for (size_t channel = 0; channel < view.channels(); ++channel) {
            for (size_t frame = 0; frame < view.frames(); ++frame) {
                values.push_back(view.get(frame, channel));
            }
        }
        return values;
    }

    std::vector<iv::Sample> sample_values(iv::OwnedSampleBlock const& block)
    {
        auto const view = block.view();
        std::vector<iv::Sample> values;
        values.reserve(view.frames() * view.channels());
        for (size_t channel = 0; channel < view.channels(); ++channel) {
            for (size_t frame = 0; frame < view.frames(); ++frame) {
                values.push_back(view.get(frame, channel));
            }
        }
        return values;
    }

    std::vector<iv::Sample> raw_samples(iv::BorrowedSampleBlock const& block)
    {
        return std::vector<iv::Sample>(block.samples.begin(), block.samples.end());
    }

    std::vector<iv::Sample> raw_samples(iv::OwnedSampleBlock const& block)
    {
        return block.samples;
    }

    std::vector<std::vector<iv::Sample>> per_channel_values(iv::BorrowedSampleBlock const& block)
    {
        auto const view = block.view();
        std::vector<std::vector<iv::Sample>> values(view.channels());
        for (size_t channel = 0; channel < view.channels(); ++channel) {
            values[channel].reserve(view.frames());
            for (size_t frame = 0; frame < view.frames(); ++frame) {
                values[channel].push_back(view.get(frame, channel));
            }
        }
        return values;
    }

    std::vector<std::vector<iv::Sample>> per_channel_values(iv::OwnedSampleBlock const& block)
    {
        auto const view = block.view();
        std::vector<std::vector<iv::Sample>> values(view.channels());
        for (size_t channel = 0; channel < view.channels(); ++channel) {
            values[channel].reserve(view.frames());
            for (size_t frame = 0; frame < view.frames(); ++frame) {
                values[channel].push_back(view.get(frame, channel));
            }
        }
        return values;
    }

    struct TestPatternRealtimeSampleLaneNode {
        iv::SampleStreamLayout layout = iv::SampleStreamLayout::planar;
        std::vector<iv::Sample> left { 1.0f, 2.0f, 3.0f, 4.0f };
        std::vector<iv::Sample> right { 10.0f, 20.0f, 30.0f, 40.0f };

        iv::RealtimeSampleLaneOutputConfig output() const
        {
            return {
                .name = "samples",
                .sample_layout = layout,
            };
        }

        void tick_block_realtime(iv::RealtimeLaneTickContext<TestPatternRealtimeSampleLaneNode>& ctx) const
        {
            auto const out = ctx.out().block_view();
            for (size_t frame = 0; frame < out.frames(); ++frame) {
                out.set(frame, 0, left[frame % left.size()]);
                if (out.channels() > 1) {
                    out.set(frame, 1, right[frame % right.size()]);
                }
            }
        }
    };

    struct TestPassthroughRealtimeSampleLaneNode {
        iv::SampleStreamLayout input_layout = iv::SampleStreamLayout::planar;
        iv::SampleStreamLayout output_layout = iv::SampleStreamLayout::planar;

        std::array<iv::RealtimeSampleLaneInputConfig, 1> realtime_sample_inputs() const
        {
            return {{
                {
                    .name = "samples",
                    .sample_layout = input_layout,
                }
            }};
        }

        iv::RealtimeSampleLaneOutputConfig output() const
        {
            return {
                .name = "samples",
                .sample_layout = output_layout,
            };
        }

        void tick_block_realtime(iv::RealtimeLaneTickContext<TestPassthroughRealtimeSampleLaneNode>& ctx) const
        {
            ctx.out().write_block(ctx.realtime_sample_input(0).block_view());
        }
    };

    struct TestPatternCompiledSampleLaneNode {
        iv::SampleStreamLayout layout = iv::SampleStreamLayout::planar;
        size_t support_start = 0;
        size_t support_end = 4096;
        int* tick_count = nullptr;
        iv::Sample right_bias = 100.0f;

        iv::CompiledSampleLaneOutputConfig output() const
        {
            return {
                .name = "samples",
                .sample_layout = layout,
            };
        }

        std::vector<iv::CompiledSupportRange> compiled_support_ranges(
            iv::CompiledSupportContext<TestPatternCompiledSampleLaneNode>&) const
        {
            return { iv::CompiledSupportRange{
                .start_index = support_start,
                .end_index = support_end,
            }};
        }

        void tick_block_compiled(iv::CompiledLaneTickContext<TestPatternCompiledSampleLaneNode>& ctx)
        {
            if (tick_count) {
                *tick_count += 1;
            }
            auto const frames = ctx.sample_count();
            auto const layout_value = ctx.out().channel_layout;
            std::vector<iv::Sample> samples(iv::sample_storage_size(layout_value, frames), iv::Sample{});
            auto const block = iv::SampleBlockView<iv::Sample>(samples, layout_value, frames);
            for (size_t frame = 0; frame < frames; ++frame) {
                block.set(frame, 0, static_cast<iv::Sample>(ctx.start_index() + frame));
                if (block.channels() > 1) {
                    block.set(
                        frame,
                        1,
                        static_cast<iv::Sample>(right_bias + ctx.start_index() + frame));
                }
            }
            ctx.out().write_block(
                ctx.start_index(),
                iv::SampleBlockView<iv::Sample const>(samples, layout_value, frames));
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
            ctx.out().write_block(
                ctx.start_index(),
                iv::SampleBlockView<iv::Sample const>(samples, ctx.out().channel_layout, samples.size()));
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
            ctx.out().write_block(
                ctx.start_index(),
                iv::SampleBlockView<iv::Sample const>(samples, ctx.out().channel_layout, samples.size()));
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
            ctx.out().write_block(
                ctx.start_index(),
                iv::SampleBlockView<iv::Sample const>(samples, ctx.out().channel_layout, samples.size()));
        }
    };
}

TEST(TimelineExecution, SynchronizeTracksRealtimeSampleOutputLanes)
{
    iv::Timeline timeline;
    auto const source = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode {
            .value = 440.0f,
        }), {}, iv::ChannelTypeId::mono);
    });
    auto const target = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(iv::GraphSampleInputLaneNode {
            .default_value = 0.0f,
        }), {}, iv::ChannelTypeId::mono);
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
        }), {}, iv::ChannelTypeId::mono);
    });
    auto const concrete = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode {
            .value = 220.0f,
        }), {}, iv::ChannelTypeId::mono);
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
    ASSERT_EQ(logical_block.frame_count, 4u);
    ASSERT_EQ(concrete_block.frame_count, 4u);
    EXPECT_EQ(sample_values(logical_block),
        (std::vector<iv::Sample> { 440.0f, 440.0f, 440.0f, 440.0f }));
    EXPECT_EQ(sample_values(concrete_block),
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
        }), {}, iv::ChannelTypeId::mono);
        realtime_target = graph.add_lane(iv::TypeErasedLaneNode(iv::GraphSampleInputLaneNode {
            .default_value = -1.0f,
        }), {}, iv::ChannelTypeId::mono);
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
    ASSERT_EQ(target_block.frame_count, 4u);
    EXPECT_EQ(sample_values(target_block),
        (std::vector<iv::Sample> { 8.0f, 9.0f, 10.0f, 11.0f }));

    auto const compiled_block = execution.compiled_sample_block(compiled_source, 8);
    ASSERT_EQ(compiled_block.frame_count, 4u);
    EXPECT_EQ(sample_values(compiled_block),
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

TEST(TimelineExecution, PausePinsRealtimeStartIndexUntilResume)
{
    iv::Timeline timeline;
    int compiled_ticks = 0;
    iv::LaneId compiled_source {};
    iv::LaneId realtime_target {};
    timeline.with_graph([&](iv::LaneGraph& graph) {
        compiled_source = graph.add_lane(iv::TypeErasedLaneNode(TestCompiledSampleSourceLaneNode {
            .tick_count = &compiled_ticks,
        }), {}, iv::ChannelTypeId::mono);
        realtime_target = graph.add_lane(iv::TypeErasedLaneNode(iv::GraphSampleInputLaneNode {
            .default_value = -1.0f,
        }), {}, iv::ChannelTypeId::mono);
        graph.connect(compiled_source, realtime_target, iv::realtime_sample_input());
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });

    execution.set_realtime_start_index(8);
    harness.run_once();
    EXPECT_EQ(sample_values(execution.realtime_sample_block(realtime_target)),
        (std::vector<iv::Sample>{8.0f, 9.0f, 10.0f, 11.0f}));

    execution.pause();
    EXPECT_TRUE(execution.is_paused());
    execution.set_realtime_start_index(32);
    harness.run_once();
    EXPECT_EQ(execution.realtime_start_index(), 8u);
    EXPECT_EQ(sample_values(execution.realtime_sample_block(realtime_target)),
        (std::vector<iv::Sample>{8.0f, 9.0f, 10.0f, 11.0f}));

    execution.resume(32);
    EXPECT_FALSE(execution.is_paused());
    harness.run_once();
    EXPECT_EQ(execution.realtime_start_index(), 32u);
    EXPECT_EQ(sample_values(execution.realtime_sample_block(realtime_target)),
        (std::vector<iv::Sample>{32.0f, 33.0f, 34.0f, 35.0f}));
    EXPECT_EQ(compiled_ticks, 1);
}

TEST(TimelineExecution, LaneChangeInvalidatesCompiledCache)
{
    iv::Timeline timeline;
    auto const compiled_source = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(
            iv::TypeErasedLaneNode(TestCompiledSampleSourceLaneNode {}),
            {},
            iv::ChannelTypeId::mono);
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });

    execution.set_realtime_start_index(16);
    harness.run_once();
    auto const first_block = execution.compiled_sample_block(compiled_source, 16);
    ASSERT_EQ(first_block.frame_count, 4u);
    EXPECT_EQ(sample_values(first_block)[0], 16.0f);

    timeline.apply_lane_batch(iv::TimelineLaneBatchUpdate {
        .upserts = {
            iv::TimelineLaneUpsert {
                .lane = compiled_source,
                .make_node = [] {
                    return iv::TypeErasedLaneNode(TestCompiledSampleSourceLaneNode {});
                },
                .sample_channel_type = iv::ChannelTypeId::mono,
            },
        },
    });

    iv::TimelineLanesChanged change {
        .lane_set_changed = false,
        .visit_lanes = [&timeline](std::vector<iv::LaneId> const& lanes, iv::TimelineLaneVisitFn const& visit) {
            timeline.with_graph([&](iv::LaneGraph const& graph) {
                for (auto const lane : lanes) {
                    auto const& record = graph.lane(lane);
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
        .changed_lanes = { compiled_source },
    };
    harness.apply(execution.handle_timeline_lanes_changed(change));

    execution.set_realtime_start_index(20);
    harness.run_once();
    auto const second_block = execution.compiled_sample_block(compiled_source, 20);
    ASSERT_EQ(second_block.frame_count, 4u);
    EXPECT_EQ(sample_values(second_block)[0], 20.0f);
}

TEST(TimelineExecution, CompiledSupportReturnsDefaultsOutsideSupport)
{
    iv::Timeline timeline;
    int compiled_ticks = 0;
    auto const compiled_source = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(TestSparseCompiledSampleLaneNode {
            .tick_count = &compiled_ticks,
        }), {}, iv::ChannelTypeId::mono);
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });

    auto const outside_support = execution.compiled_sample_block(compiled_source, 0);
    ASSERT_EQ(outside_support.frame_count, 4u);
    EXPECT_EQ(
        sample_values(outside_support),
        (std::vector<iv::Sample> { 0.0f, 0.0f, 0.0f, 0.0f }));
    EXPECT_EQ(compiled_ticks, 1);

    auto const inside_support = execution.compiled_sample_block(compiled_source, 8);
    ASSERT_EQ(inside_support.frame_count, 4u);
    EXPECT_EQ(
        sample_values(inside_support),
        (std::vector<iv::Sample> { 7.0f, 7.0f, 7.0f, 7.0f }));
    EXPECT_EQ(compiled_ticks, 1);
}

TEST(TimelineExecution, CompiledSupportFillsUnsupportedRemainderWithDefaults)
{
    iv::Timeline timeline;
    int compiled_ticks = 0;
    auto const compiled_source = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(TestSparseCompiledSampleLaneNode {
            .tick_count = &compiled_ticks,
        }), {}, iv::ChannelTypeId::mono);
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });

    auto const partially_supported = execution.compiled_sample_block(compiled_source, 6);
    ASSERT_EQ(partially_supported.frame_count, 4u);
    EXPECT_EQ(
        sample_values(partially_supported),
        (std::vector<iv::Sample> { 0.0f, 0.0f, 7.0f, 7.0f }));
    EXPECT_EQ(compiled_ticks, 1);
}

TEST(TimelineExecution, ReusesCompiledSampleChunksWithinChunkBoundaries)
{
    iv::Timeline timeline;
    int compiled_ticks = 0;
    auto const compiled_source = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(TestChunkedCompiledSampleLaneNode {
            .tick_count = &compiled_ticks,
        }), {}, iv::ChannelTypeId::mono);
    });

    iv::TimelineExecution execution(4, 2);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });

    auto const first_block = execution.compiled_sample_block(compiled_source, 8);
    ASSERT_EQ(first_block.frame_count, 4u);
    EXPECT_EQ(
        sample_values(first_block),
        (std::vector<iv::Sample> { 8.0f, 9.0f, 10.0f, 11.0f }));
    EXPECT_EQ(compiled_ticks, 1);

    auto const same_chunk_block = execution.compiled_sample_block(compiled_source, 12);
    ASSERT_EQ(same_chunk_block.frame_count, 4u);
    EXPECT_EQ(
        sample_values(same_chunk_block),
        (std::vector<iv::Sample> { 12.0f, 13.0f, 14.0f, 15.0f }));
    EXPECT_EQ(compiled_ticks, 1);

    auto const next_chunk_block = execution.compiled_sample_block(compiled_source, 16);
    ASSERT_EQ(next_chunk_block.frame_count, 4u);
    EXPECT_EQ(
        sample_values(next_chunk_block),
        (std::vector<iv::Sample> { 16.0f, 17.0f, 18.0f, 19.0f }));
    EXPECT_EQ(compiled_ticks, 2);
}

TEST(TimelineExecution, RejectsZeroChunkSizeMultiplier)
{
    EXPECT_THROW(
        (void)iv::TimelineExecution(8, 0),
        std::runtime_error);
}

TEST(TimelineExecution, ConvertsMonoPlanarToStereoInterleaved)
{
    iv::Timeline timeline;
    iv::LaneId source {};
    iv::LaneId sink {};
    timeline.with_graph([&](iv::LaneGraph& graph) {
        source = graph.add_lane(
            iv::TypeErasedLaneNode(TestPatternRealtimeSampleLaneNode{
                .layout = iv::SampleStreamLayout::planar,
                .left = { 1.0f, 2.0f, 3.0f, 4.0f },
            }),
            {},
            iv::ChannelTypeId::mono);
        sink = graph.add_lane(
            iv::TypeErasedLaneNode(TestPassthroughRealtimeSampleLaneNode{
                .input_layout = iv::SampleStreamLayout::interleaved,
                .output_layout = iv::SampleStreamLayout::interleaved,
            }),
            {},
            iv::ChannelTypeId::stereo);
        graph.connect(source, sink, iv::realtime_sample_input());
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });
    harness.run_once();

    auto const sink_block = execution.realtime_sample_block(sink);
    EXPECT_EQ(sink_block.channel_layout.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(sink_block.channel_layout.sample_layout, iv::SampleStreamLayout::interleaved);
    EXPECT_EQ(
        raw_samples(sink_block),
        (std::vector<iv::Sample>{ 1.0f, 1.0f, 2.0f, 2.0f, 3.0f, 3.0f, 4.0f, 4.0f }));
}

TEST(TimelineExecution, ConvertsStereoInterleavedToMonoPlanar)
{
    iv::Timeline timeline;
    iv::LaneId source {};
    iv::LaneId sink {};
    timeline.with_graph([&](iv::LaneGraph& graph) {
        source = graph.add_lane(
            iv::TypeErasedLaneNode(TestPatternRealtimeSampleLaneNode{
                .layout = iv::SampleStreamLayout::interleaved,
                .left = { 1.0f, 2.0f, 3.0f, 4.0f },
                .right = { 9.0f, 10.0f, 11.0f, 12.0f },
            }),
            {},
            iv::ChannelTypeId::stereo);
        sink = graph.add_lane(
            iv::TypeErasedLaneNode(TestPassthroughRealtimeSampleLaneNode{
                .input_layout = iv::SampleStreamLayout::planar,
                .output_layout = iv::SampleStreamLayout::planar,
            }),
            {},
            iv::ChannelTypeId::mono);
        graph.connect(source, sink, iv::realtime_sample_input());
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });
    harness.run_once();

    auto const sink_block = execution.realtime_sample_block(sink);
    EXPECT_EQ(sink_block.channel_layout.channel_type, iv::ChannelTypeId::mono);
    EXPECT_EQ(sink_block.channel_layout.sample_layout, iv::SampleStreamLayout::planar);
    EXPECT_EQ(raw_samples(sink_block), (std::vector<iv::Sample>{ 5.0f, 6.0f, 7.0f, 8.0f }));
}

TEST(TimelineExecution, OneStereoSourceFeedsMultipleConsumerLayouts)
{
    iv::Timeline timeline;
    iv::LaneId source {};
    iv::LaneId planar_sink {};
    iv::LaneId interleaved_sink {};
    iv::LaneId mono_sink {};
    timeline.with_graph([&](iv::LaneGraph& graph) {
        source = graph.add_lane(
            iv::TypeErasedLaneNode(TestPatternRealtimeSampleLaneNode{
                .layout = iv::SampleStreamLayout::planar,
                .left = { 1.0f, 2.0f, 3.0f, 4.0f },
                .right = { 10.0f, 20.0f, 30.0f, 40.0f },
            }),
            {},
            iv::ChannelTypeId::stereo);
        planar_sink = graph.add_lane(
            iv::TypeErasedLaneNode(TestPassthroughRealtimeSampleLaneNode{
                .input_layout = iv::SampleStreamLayout::planar,
                .output_layout = iv::SampleStreamLayout::planar,
            }),
            {},
            iv::ChannelTypeId::stereo);
        interleaved_sink = graph.add_lane(
            iv::TypeErasedLaneNode(TestPassthroughRealtimeSampleLaneNode{
                .input_layout = iv::SampleStreamLayout::interleaved,
                .output_layout = iv::SampleStreamLayout::interleaved,
            }),
            {},
            iv::ChannelTypeId::stereo);
        mono_sink = graph.add_lane(
            iv::TypeErasedLaneNode(TestPassthroughRealtimeSampleLaneNode{
                .input_layout = iv::SampleStreamLayout::planar,
                .output_layout = iv::SampleStreamLayout::planar,
            }),
            {},
            iv::ChannelTypeId::mono);
        graph.connect(source, planar_sink, iv::realtime_sample_input());
        graph.connect(source, interleaved_sink, iv::realtime_sample_input());
        graph.connect(source, mono_sink, iv::realtime_sample_input());
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });
    harness.run_once();

    EXPECT_EQ(
        raw_samples(execution.realtime_sample_block(planar_sink)),
        (std::vector<iv::Sample>{ 1.0f, 2.0f, 3.0f, 4.0f, 10.0f, 20.0f, 30.0f, 40.0f }));
    EXPECT_EQ(
        raw_samples(execution.realtime_sample_block(interleaved_sink)),
        (std::vector<iv::Sample>{ 1.0f, 10.0f, 2.0f, 20.0f, 3.0f, 30.0f, 4.0f, 40.0f }));
    EXPECT_EQ(
        raw_samples(execution.realtime_sample_block(mono_sink)),
        (std::vector<iv::Sample>{ 5.5f, 11.0f, 16.5f, 22.0f }));
}

TEST(TimelineExecution, MixedLayoutFanInSumsAfterConversion)
{
    iv::Timeline timeline;
    iv::LaneId mono_source {};
    iv::LaneId stereo_source {};
    iv::LaneId sink {};
    timeline.with_graph([&](iv::LaneGraph& graph) {
        mono_source = graph.add_lane(
            iv::TypeErasedLaneNode(TestPatternRealtimeSampleLaneNode{
                .layout = iv::SampleStreamLayout::planar,
                .left = { 1.0f, 2.0f, 3.0f, 4.0f },
            }),
            {},
            iv::ChannelTypeId::mono);
        stereo_source = graph.add_lane(
            iv::TypeErasedLaneNode(TestPatternRealtimeSampleLaneNode{
                .layout = iv::SampleStreamLayout::interleaved,
                .left = { 10.0f, 20.0f, 30.0f, 40.0f },
                .right = { 100.0f, 200.0f, 300.0f, 400.0f },
            }),
            {},
            iv::ChannelTypeId::stereo);
        sink = graph.add_lane(
            iv::TypeErasedLaneNode(TestPassthroughRealtimeSampleLaneNode{
                .input_layout = iv::SampleStreamLayout::planar,
                .output_layout = iv::SampleStreamLayout::planar,
            }),
            {},
            iv::ChannelTypeId::stereo);
        graph.connect(mono_source, sink, iv::realtime_sample_input());
        graph.connect(stereo_source, sink, iv::realtime_sample_input());
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });
    harness.run_once();

    auto const sink_block = execution.realtime_sample_block(sink);
    EXPECT_EQ(
        per_channel_values(sink_block),
        (std::vector<std::vector<iv::Sample>>{
            { 11.0f, 22.0f, 33.0f, 44.0f },
            { 101.0f, 202.0f, 303.0f, 404.0f },
        }));
    EXPECT_EQ(
        raw_samples(sink_block),
        (std::vector<iv::Sample>{ 11.0f, 22.0f, 33.0f, 44.0f, 101.0f, 202.0f, 303.0f, 404.0f }));
}

TEST(TimelineExecution, ReusesStereoInterleavedCompiledChunksWithinChunkBoundaries)
{
    iv::Timeline timeline;
    int compiled_ticks = 0;
    auto const compiled_source = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(
            iv::TypeErasedLaneNode(TestPatternCompiledSampleLaneNode{
                .layout = iv::SampleStreamLayout::interleaved,
                .support_start = 8,
                .support_end = 24,
                .tick_count = &compiled_ticks,
            }),
            {},
            iv::ChannelTypeId::stereo);
    });

    iv::TimelineExecution execution(4, 2);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });

    auto const first_block = execution.compiled_sample_block(compiled_source, 8);
    EXPECT_EQ(
        raw_samples(first_block),
        (std::vector<iv::Sample>{ 8.0f, 108.0f, 9.0f, 109.0f, 10.0f, 110.0f, 11.0f, 111.0f }));
    EXPECT_EQ(compiled_ticks, 1);

    auto const same_chunk_block = execution.compiled_sample_block(compiled_source, 12);
    EXPECT_EQ(
        raw_samples(same_chunk_block),
        (std::vector<iv::Sample>{ 12.0f, 112.0f, 13.0f, 113.0f, 14.0f, 114.0f, 15.0f, 115.0f }));
    EXPECT_EQ(compiled_ticks, 1);

    auto const next_chunk_block = execution.compiled_sample_block(compiled_source, 16);
    EXPECT_EQ(
        raw_samples(next_chunk_block),
        (std::vector<iv::Sample>{ 16.0f, 116.0f, 17.0f, 117.0f, 18.0f, 118.0f, 19.0f, 119.0f }));
    EXPECT_EQ(compiled_ticks, 2);
}

TEST(TimelineExecution, StereoSparseCompiledSupportFillsUnsupportedRemainderWithDefaults)
{
    iv::Timeline timeline;
    int compiled_ticks = 0;
    auto const compiled_source = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(
            iv::TypeErasedLaneNode(TestPatternCompiledSampleLaneNode{
                .layout = iv::SampleStreamLayout::planar,
                .support_start = 8,
                .support_end = 12,
                .tick_count = &compiled_ticks,
            }),
            {},
            iv::ChannelTypeId::stereo);
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });

    auto const partially_supported = execution.compiled_sample_block(compiled_source, 6);
    EXPECT_EQ(
        per_channel_values(partially_supported),
        (std::vector<std::vector<iv::Sample>>{
            { 0.0f, 0.0f, 8.0f, 9.0f },
            { 0.0f, 0.0f, 108.0f, 109.0f },
        }));
    EXPECT_EQ(
        raw_samples(partially_supported),
        (std::vector<iv::Sample>{ 0.0f, 0.0f, 8.0f, 9.0f, 0.0f, 0.0f, 108.0f, 109.0f }));
    EXPECT_EQ(compiled_ticks, 1);
}

TEST(TimelineExecution, StereoSparseCompiledSupportFillsUnsupportedPrefixWithDefaults)
{
    iv::Timeline timeline;
    int compiled_ticks = 0;
    auto const compiled_source = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(
            iv::TypeErasedLaneNode(TestPatternCompiledSampleLaneNode{
                .layout = iv::SampleStreamLayout::planar,
                .support_start = 10,
                .support_end = 14,
                .tick_count = &compiled_ticks,
            }),
            {},
            iv::ChannelTypeId::stereo);
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });

    auto const partially_supported = execution.compiled_sample_block(compiled_source, 8);
    EXPECT_EQ(
        per_channel_values(partially_supported),
        (std::vector<std::vector<iv::Sample>>{
            { 0.0f, 0.0f, 10.0f, 11.0f },
            { 0.0f, 0.0f, 110.0f, 111.0f },
        }));
    EXPECT_EQ(compiled_ticks, 1);
}

TEST(TimelineExecution, ChannelTypeChangeRebuildsDownstreamConversionState)
{
    iv::Timeline timeline;
    iv::LaneId source {};
    iv::LaneId sink {};
    timeline.with_graph([&](iv::LaneGraph& graph) {
        source = graph.add_lane(
            iv::TypeErasedLaneNode(TestPatternRealtimeSampleLaneNode{
                .layout = iv::SampleStreamLayout::planar,
                .left = { 4.0f, 8.0f, 12.0f, 16.0f },
            }),
            {},
            iv::ChannelTypeId::mono);
        sink = graph.add_lane(
            iv::TypeErasedLaneNode(TestPassthroughRealtimeSampleLaneNode{
                .input_layout = iv::SampleStreamLayout::planar,
                .output_layout = iv::SampleStreamLayout::planar,
            }),
            {},
            iv::ChannelTypeId::mono);
        graph.connect(source, sink, iv::realtime_sample_input());
    });

    iv::TimelineExecution execution(4);
    TaskGraphHarness harness;
    timeline.with_graph([&](iv::LaneGraph const& graph) {
        harness.apply(execution.synchronize_from_graph(graph));
    });
    harness.run_once();
    EXPECT_EQ(
        raw_samples(execution.realtime_sample_block(sink)),
        (std::vector<iv::Sample>{ 4.0f, 8.0f, 12.0f, 16.0f }));

    timeline.apply_lane_batch(iv::TimelineLaneBatchUpdate{
        .upserts = {
            iv::TimelineLaneUpsert{
                .lane = source,
                .make_node = [] {
                    return iv::TypeErasedLaneNode(TestPatternRealtimeSampleLaneNode{
                        .layout = iv::SampleStreamLayout::planar,
                        .left = { 4.0f, 8.0f, 12.0f, 16.0f },
                        .right = { 20.0f, 24.0f, 28.0f, 32.0f },
                    });
                },
                .sample_channel_type = iv::ChannelTypeId::stereo,
            },
        },
    });

    iv::TimelineLanesChanged change {
        .lane_set_changed = false,
        .visit_lanes = [&timeline](std::vector<iv::LaneId> const& lanes, iv::TimelineLaneVisitFn const& visit) {
            timeline.with_graph([&](iv::LaneGraph const& graph) {
                for (auto const lane : lanes) {
                    auto const& record = graph.lane(lane);
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
        .changed_lanes = { source },
    };
    harness.apply(execution.handle_timeline_lanes_changed(change));
    harness.run_once();

    auto const source_block = execution.realtime_sample_block(source);
    EXPECT_EQ(source_block.channel_layout.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(
        per_channel_values(source_block),
        (std::vector<std::vector<iv::Sample>>{
            { 4.0f, 8.0f, 12.0f, 16.0f },
            { 20.0f, 24.0f, 28.0f, 32.0f },
        }));
    EXPECT_EQ(
        raw_samples(execution.realtime_sample_block(sink)),
        (std::vector<iv::Sample>{ 12.0f, 16.0f, 20.0f, 24.0f }));
}
