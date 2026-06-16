#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/task_runner_events.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <functional>
#include <latch>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
    using namespace std::chrono_literals;

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

    struct LogState {
        mutable std::mutex mutex;
        std::vector<std::string> entries;

        void push(std::string entry)
        {
            std::scoped_lock lock(mutex);
            entries.push_back(std::move(entry));
        }

        std::vector<std::string> snapshot() const
        {
            std::scoped_lock lock(mutex);
            return entries;
        }
    };

    struct RecordingContext {
        std::string name;
        LogState *log = nullptr;
        std::atomic<int> invocations = 0;
    };

    void record_callback(void *raw)
    {
        auto &ctx = *static_cast<RecordingContext *>(raw);
        ctx.invocations.fetch_add(1, std::memory_order_relaxed);
        ctx.log->push(ctx.name);
    }

    struct BlockingContext {
        std::string name;
        LogState *log = nullptr;
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false;
        bool release = false;
        std::atomic<int> invocations = 0;
    };

    void blocking_callback(void *raw)
    {
        auto &ctx = *static_cast<BlockingContext *>(raw);
        ctx.invocations.fetch_add(1, std::memory_order_relaxed);
        ctx.log->push(ctx.name);

        std::unique_lock lock(ctx.mutex);
        ctx.entered = true;
        ctx.cv.notify_all();
        ctx.cv.wait(lock, [&] { return ctx.release; });
    }

    struct OverlapState {
        std::mutex mutex;
        std::condition_variable cv;
        int started = 0;
        bool release = false;
        std::atomic<int> running = 0;
        std::atomic<int> max_running = 0;
    };

    struct OverlapContext {
        OverlapState *state = nullptr;
        std::atomic<int> invocations = 0;
    };

    void overlap_callback(void *raw)
    {
        auto &ctx = *static_cast<OverlapContext *>(raw);
        auto invocation = ctx.invocations.fetch_add(1, std::memory_order_relaxed);
        if (invocation > 0) {
            return;
        }

        auto &state = *ctx.state;
        auto running = state.running.fetch_add(1, std::memory_order_relaxed) + 1;
        auto observed = state.max_running.load(std::memory_order_relaxed);
        while (observed < running
               && !state.max_running.compare_exchange_weak(
                   observed,
                   running,
                   std::memory_order_relaxed)) {
        }

        std::unique_lock lock(state.mutex);
        state.started += 1;
        state.cv.notify_all();
        state.cv.wait(lock, [&] { return state.release; });
        lock.unlock();

        state.running.fetch_sub(1, std::memory_order_relaxed);
    }

    struct DeletingContext {
        std::string name;
        LogState *log = nullptr;
        std::atomic<int> invocations = 0;
    };

    struct PassFinishedWitness {
        mutable std::mutex mutex;
        std::vector<std::uint64_t> revisions;

        void push(std::uint64_t revision)
        {
            std::scoped_lock lock(mutex);
            revisions.push_back(revision);
        }

        std::vector<std::uint64_t> snapshot() const
        {
            std::scoped_lock lock(mutex);
            return revisions;
        }
    };

    PassFinishedWitness *g_pass_finished_witness = nullptr;

    IV_SUBSCRIBE_LINKER_EVENT(
        iv::TaskRunnerPassFinishedEvent,
        iv_runtime_task_runner_pass_finished_event,
        +[](iv::TaskRunnerPassFinished const &finished) {
            if (g_pass_finished_witness != nullptr) {
                g_pass_finished_witness->push(finished.graph_revision);
            }
        });

    class TaskRunnerTest : public ::testing::Test {
    protected:
        static iv::TaskRecord task(
            std::string id,
            std::vector<std::string> depends_on,
            void (*invoke)(void *),
            void *context)
        {
            return iv::TaskRecord{
                .id = std::move(id),
                .depends_on = std::move(depends_on),
                .callback = iv::TaskCallback{
                    .invoke = invoke,
                    .context = context,
                },
            };
        }

        static iv::VersionedTaskGraphUpdate versioned(
            std::uint64_t version_index,
            iv::TaskGraphUpdate update)
        {
            return iv::VersionedTaskGraphUpdate{
                .version_index = version_index,
                .update = std::move(update),
            };
        }
    };

    TEST_F(TaskRunnerTest, UpdateRejectsDuplicateTaskIdsAcrossCreateUpdateDelete)
    {
        LogState log;
        RecordingContext ctx{ .name = "a", .log = &log };
        iv::TaskRunner runner(1);
        EXPECT_THROW(
            runner.update_tasks(iv::TaskGraphUpdate{
                .to_create = { task("a", {}, &record_callback, &ctx) },
                .to_update = { iv::TaskUpdateRecord{ .id = "a" } },
            }),
            std::runtime_error);
    }

    TEST_F(TaskRunnerTest, CreateExistingTaskThrows)
    {
        LogState log;
        RecordingContext ctx{ .name = "a", .log = &log };
        iv::TaskRunner runner(1);
        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = { task("a", {}, &record_callback, &ctx) },
        });

        EXPECT_THROW(
            runner.update_tasks(iv::TaskGraphUpdate{
                .to_create = { task("a", {}, &record_callback, &ctx) },
            }),
            std::runtime_error);
    }

    TEST_F(TaskRunnerTest, UpdateMissingTaskThrows)
    {
        iv::TaskRunner runner(1);
        EXPECT_THROW(
            runner.update_tasks(iv::TaskGraphUpdate{
                .to_update = { iv::TaskUpdateRecord{
                    .id = "missing",
                    .depends_on = std::vector<std::string>{},
                } },
            }),
            std::runtime_error);
    }

    TEST_F(TaskRunnerTest, DeleteMissingTaskThrows)
    {
        iv::TaskRunner runner(1);
        EXPECT_THROW(
            runner.update_tasks(iv::TaskGraphUpdate{
                .to_delete = { "missing" },
            }),
            std::runtime_error);
    }

    TEST_F(TaskRunnerTest, UnresolvedDependencyOpensIncompletePendingGraph)
    {
        LogState log;
        RecordingContext ctx{ .name = "a", .log = &log };
        iv::TaskRunner runner(1);
        runner.update_tasks(versioned(1, iv::TaskGraphUpdate{
                .to_create = { task("a", { "missing" }, &record_callback, &ctx) },
            }));
        EXPECT_EQ(runner.active_graph_revision(), 0u);
        ASSERT_TRUE(runner.pending_graph_revision().has_value());
        EXPECT_EQ(*runner.pending_graph_revision(), 1u);
        std::this_thread::sleep_for(20ms);
        EXPECT_EQ(ctx.invocations.load(), 0);
    }

    TEST_F(TaskRunnerTest, CycleThrows)
    {
        LogState log;
        RecordingContext a{ .name = "a", .log = &log };
        RecordingContext b{ .name = "b", .log = &log };
        iv::TaskRunner runner(1);
        EXPECT_THROW(
            runner.update_tasks(iv::TaskGraphUpdate{
                .to_create = {
                    task("a", { "b" }, &record_callback, &a),
                    task("b", { "a" }, &record_callback, &b),
                },
            }),
            std::runtime_error);
    }

    TEST_F(TaskRunnerTest, MatchingVersionCompletesIncompletePendingGraph)
    {
        LogState log;
        RecordingContext a{ .name = "a", .log = &log };
        RecordingContext b{ .name = "b", .log = &log };
        iv::TaskRunner runner(1);

        runner.update_tasks(versioned(1, iv::TaskGraphUpdate{
            .to_create = { task("a", { "b" }, &record_callback, &a) },
        }));
        runner.update_tasks(versioned(1, iv::TaskGraphUpdate{
            .to_create = { task("b", {}, &record_callback, &b) },
        }));

        ASSERT_TRUE(wait_until([&] {
            return runner.active_graph_revision() == 2
                && a.invocations.load() > 0
                && b.invocations.load() > 0;
        }));
    }

    TEST_F(TaskRunnerTest, DifferentVersionWhilePendingIncompleteThrows)
    {
        LogState log;
        RecordingContext a{ .name = "a", .log = &log };
        RecordingContext b{ .name = "b", .log = &log };
        iv::TaskRunner runner(1);

        runner.update_tasks(versioned(1, iv::TaskGraphUpdate{
            .to_create = { task("a", { "b" }, &record_callback, &a) },
        }));

        EXPECT_THROW(
            runner.update_tasks(versioned(2, iv::TaskGraphUpdate{
                .to_create = { task("b", {}, &record_callback, &b) },
            })),
            std::runtime_error);
    }

    TEST_F(TaskRunnerTest, DeleteAutomaticallyRemovesDependencyReferencesFromSuccessorGraph)
    {
        LogState log;
        RecordingContext a{ .name = "a", .log = &log };
        RecordingContext b{ .name = "b", .log = &log };
        iv::TaskRunner runner(1);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = {
                task("a", {}, &record_callback, &a),
                task("b", { "a" }, &record_callback, &b),
            },
        });

        ASSERT_TRUE(wait_until([&] { return a.invocations.load() > 0 && b.invocations.load() > 0; }));

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_delete = { "a" },
        });

        ASSERT_TRUE(wait_until([&] {
            auto ids = runner.active_task_ids();
            return ids.size() == 1 && ids.front() == "b";
        }));
        ASSERT_TRUE(wait_until([&] { return b.invocations.load() >= 2; }));
    }

    TEST_F(TaskRunnerTest, UpdateCanReplaceDependenciesAndCallback)
    {
        LogState log;
        RecordingContext a{ .name = "a", .log = &log };
        RecordingContext b{ .name = "b", .log = &log };
        RecordingContext c{ .name = "c", .log = &log };
        RecordingContext d{ .name = "d", .log = &log };
        iv::TaskRunner runner(1);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = {
                task("a", {}, &record_callback, &a),
                task("b", { "a" }, &record_callback, &b),
            },
        });
        ASSERT_TRUE(wait_until([&] { return b.invocations.load() > 0; }));

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = { task("c", {}, &record_callback, &c) },
            .to_update = {
                iv::TaskUpdateRecord{
                    .id = "b",
                    .depends_on = std::vector<std::string>{ "c" },
                    .callback = iv::TaskCallback{
                        .invoke = &record_callback,
                        .context = &c,
                    },
                },
            },
        });

        ASSERT_TRUE(wait_until([&] { return runner.active_graph_revision() >= 2; }));
        ASSERT_TRUE(wait_until([&] { return c.invocations.load() >= 2; }));
    }

    TEST_F(TaskRunnerTest, UpdateBuildsFromLatestPendingVersionWhenMultipleUpdatesArriveDuringPass)
    {
        LogState log;
        BlockingContext a;
        a.name = "a";
        a.log = &log;
        RecordingContext b{ .name = "b", .log = &log };
        RecordingContext c{ .name = "c", .log = &log };
        iv::TaskRunner runner(1);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = { task("a", {}, &blocking_callback, &a) },
        });

        {
            std::unique_lock lock(a.mutex);
            ASSERT_TRUE(a.cv.wait_for(lock, 2s, [&] { return a.entered; }));
        }

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = { task("b", {}, &record_callback, &b) },
        });
        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = { task("c", { "b" }, &record_callback, &c) },
        });

        EXPECT_EQ(runner.pending_graph_revision(), std::optional<std::uint64_t>(3));

        {
            std::scoped_lock lock(a.mutex);
            a.release = true;
        }
        a.cv.notify_all();

        ASSERT_TRUE(wait_until([&] {
            auto ids = runner.active_task_ids();
            return runner.active_graph_revision() == 3
                && ids == std::vector<std::string>({ "a", "b", "c" });
        }));
    }

    TEST_F(TaskRunnerTest, ExecutesTasksInDependencyOrder)
    {
        LogState log;
        RecordingContext a{ .name = "a", .log = &log };
        RecordingContext b{ .name = "b", .log = &log };
        RecordingContext c{ .name = "c", .log = &log };
        iv::TaskRunner runner(2);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = {
                task("a", {}, &record_callback, &a),
                task("b", { "a" }, &record_callback, &b),
                task("c", { "b" }, &record_callback, &c),
            },
        });

        ASSERT_TRUE(wait_until([&] { return c.invocations.load() > 0; }));
        auto entries = log.snapshot();
        ASSERT_GE(entries.size(), 3u);
        EXPECT_EQ(entries[0], "a");
        EXPECT_EQ(entries[1], "b");
        EXPECT_EQ(entries[2], "c");
    }

    TEST_F(TaskRunnerTest, ReadyTasksUseHigherUserCountAsTieBreaker)
    {
        LogState log;
        RecordingContext a{ .name = "a", .log = &log };
        RecordingContext b{ .name = "b", .log = &log };
        RecordingContext c{ .name = "c", .log = &log };
        RecordingContext d{ .name = "d", .log = &log };
        RecordingContext e{ .name = "e", .log = &log };
        iv::TaskRunner runner(1);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = {
                task("a", {}, &record_callback, &a),
                task("b", {}, &record_callback, &b),
                task("c", { "a" }, &record_callback, &c),
                task("d", { "b" }, &record_callback, &d),
                task("e", { "b" }, &record_callback, &e),
            },
        });

        ASSERT_TRUE(wait_until([&] { return d.invocations.load() > 0 && e.invocations.load() > 0; }));
        auto entries = log.snapshot();
        ASSERT_GE(entries.size(), 2u);
        EXPECT_EQ(entries[0], "b");
        EXPECT_EQ(entries[1], "a");
    }

    TEST_F(TaskRunnerTest, RunsIndependentTasksInParallel)
    {
        OverlapState state;
        OverlapContext a{ .state = &state };
        OverlapContext b{ .state = &state };
        iv::TaskRunner runner(2);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = {
                task("a", {}, &overlap_callback, &a),
                task("b", {}, &overlap_callback, &b),
            },
        });

        {
            std::unique_lock lock(state.mutex);
            ASSERT_TRUE(state.cv.wait_for(lock, 2s, [&] { return state.started == 2; }));
            state.release = true;
        }
        state.cv.notify_all();

        ASSERT_TRUE(wait_until([&] { return state.max_running.load() >= 2; }));
    }

    TEST_F(TaskRunnerTest, RunsEveryTaskOncePerPass)
    {
        LogState log;
        RecordingContext a{ .name = "a", .log = &log };
        RecordingContext b{ .name = "b", .log = &log };
        iv::TaskRunner runner(1);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = {
                task("a", {}, &record_callback, &a),
                task("b", { "a" }, &record_callback, &b),
            },
        });

        ASSERT_TRUE(wait_until([&] { return b.invocations.load() >= 2; }));
        EXPECT_GE(a.invocations.load(), 2);
        EXPECT_EQ(a.invocations.load(), b.invocations.load());
    }

    TEST_F(TaskRunnerTest, SwapsToLatestReadyGraphOnlyAtPassBoundary)
    {
        LogState log;
        BlockingContext a;
        a.name = "a";
        a.log = &log;
        RecordingContext b{ .name = "b", .log = &log };
        iv::TaskRunner runner(1);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = { task("a", {}, &blocking_callback, &a) },
        });

        {
            std::unique_lock lock(a.mutex);
            ASSERT_TRUE(a.cv.wait_for(lock, 2s, [&] { return a.entered; }));
        }

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = { task("b", {}, &record_callback, &b) },
        });

        EXPECT_EQ(runner.active_graph_revision(), 1u);
        EXPECT_EQ(runner.pending_graph_revision(), std::optional<std::uint64_t>(2));

        {
            std::scoped_lock lock(a.mutex);
            a.release = true;
        }
        a.cv.notify_all();

        ASSERT_TRUE(wait_until([&] { return runner.active_graph_revision() == 2; }));
    }

    TEST_F(TaskRunnerTest, RunningTaskMayFinishBeforeDeletedTaskDisappears)
    {
        LogState log;
        BlockingContext a;
        a.name = "a";
        a.log = &log;
        iv::TaskRunner runner(1);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = { task("a", {}, &blocking_callback, &a) },
        });

        {
            std::unique_lock lock(a.mutex);
            ASSERT_TRUE(a.cv.wait_for(lock, 2s, [&] { return a.entered; }));
        }

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_delete = { "a" },
        });

        EXPECT_EQ(runner.active_task_ids(), std::vector<std::string>({ "a" }));

        {
            std::scoped_lock lock(a.mutex);
            a.release = true;
        }
        a.cv.notify_all();

        ASSERT_TRUE(wait_until([&] { return runner.active_task_ids().empty(); }));
    }

    TEST_F(TaskRunnerTest, EmptyGraphWorkersSleepUntilUpdateArrives)
    {
        LogState log;
        RecordingContext a{ .name = "a", .log = &log };
        iv::TaskRunner runner(2);

        std::this_thread::sleep_for(20ms);
        EXPECT_EQ(a.invocations.load(), 0);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = { task("a", {}, &record_callback, &a) },
        });

        ASSERT_TRUE(wait_until([&] { return a.invocations.load() > 0; }));
    }

    TEST_F(TaskRunnerTest, MergesStrictLinearChainIntoSingleExecutionGroup)
    {
        LogState log;
        RecordingContext a{ .name = "a", .log = &log };
        RecordingContext b{ .name = "b", .log = &log };
        RecordingContext c{ .name = "c", .log = &log };
        iv::TaskRunner runner(1);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = {
                task("a", {}, &record_callback, &a),
                task("b", { "a" }, &record_callback, &b),
                task("c", { "b" }, &record_callback, &c),
            },
        });

        ASSERT_TRUE(wait_until([&] { return runner.active_task_ids().size() == 3; }));
        EXPECT_EQ(runner.active_execution_group_count(), 1u);
    }

    TEST_F(TaskRunnerTest, DoesNotMergeAcrossFanOut)
    {
        LogState log;
        RecordingContext a{ .name = "a", .log = &log };
        RecordingContext b{ .name = "b", .log = &log };
        RecordingContext c{ .name = "c", .log = &log };
        iv::TaskRunner runner(1);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = {
                task("a", {}, &record_callback, &a),
                task("b", { "a" }, &record_callback, &b),
                task("c", { "a" }, &record_callback, &c),
            },
        });

        ASSERT_TRUE(wait_until([&] { return runner.active_task_ids().size() == 3; }));
        EXPECT_EQ(runner.active_execution_group_count(), 3u);
    }

    TEST_F(TaskRunnerTest, DoesNotMergeAcrossFanIn)
    {
        LogState log;
        RecordingContext a{ .name = "a", .log = &log };
        RecordingContext b{ .name = "b", .log = &log };
        RecordingContext c{ .name = "c", .log = &log };
        iv::TaskRunner runner(1);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = {
                task("a", {}, &record_callback, &a),
                task("b", {}, &record_callback, &b),
                task("c", { "a", "b" }, &record_callback, &c),
            },
        });

        ASSERT_TRUE(wait_until([&] { return runner.active_task_ids().size() == 3; }));
        EXPECT_EQ(runner.active_execution_group_count(), 3u);
    }

    TEST_F(TaskRunnerTest, DoesNotMergeWhenSuccessorHasMultipleDependencies)
    {
        LogState log;
        RecordingContext a{ .name = "a", .log = &log };
        RecordingContext x{ .name = "x", .log = &log };
        RecordingContext b{ .name = "b", .log = &log };
        RecordingContext c{ .name = "c", .log = &log };
        RecordingContext d{ .name = "d", .log = &log };
        iv::TaskRunner runner(1);

        runner.update_tasks(iv::TaskGraphUpdate{
            .to_create = {
                task("a", {}, &record_callback, &a),
                task("x", {}, &record_callback, &x),
                task("b", { "a", "x" }, &record_callback, &b),
                task("c", { "b" }, &record_callback, &c),
                task("d", { "b" }, &record_callback, &d),
            },
        });

        ASSERT_TRUE(wait_until([&] { return runner.active_task_ids().size() == 5; }));
        EXPECT_EQ(runner.active_execution_group_count(), 5u);
    }

    TEST_F(TaskRunnerTest, DestructorWaitsForCurrentPassToFinish)
    {
        auto runner = std::make_unique<iv::TaskRunner>(1);
        auto log = std::make_unique<LogState>();
        auto ctx = std::make_unique<BlockingContext>();
        ctx->name = "a";
        ctx->log = log.get();

        runner->update_tasks(iv::TaskGraphUpdate{
            .to_create = { task("a", {}, &blocking_callback, ctx.get()) },
        });

        {
            std::unique_lock lock(ctx->mutex);
            ASSERT_TRUE(ctx->cv.wait_for(lock, 2s, [&] { return ctx->entered; }));
        }

        auto destroy_future = std::async(std::launch::async, [&] {
            runner.reset();
        });
        EXPECT_EQ(destroy_future.wait_for(50ms), std::future_status::timeout);

        {
            std::scoped_lock lock(ctx->mutex);
            ctx->release = true;
        }
        ctx->cv.notify_all();

        EXPECT_EQ(destroy_future.wait_for(2s), std::future_status::ready);
    }

    TEST_F(TaskRunnerTest, DestructorRequestsShutdownAndJoinsWorkers)
    {
        auto log = std::make_unique<LogState>();
        auto *runner = new iv::TaskRunner(1);
        auto *ctx = new BlockingContext();
        ctx->name = "a";
        ctx->log = log.get();

        runner->update_tasks(iv::TaskGraphUpdate{
            .to_create = { task("a", {}, &blocking_callback, ctx) },
        });

        {
            std::unique_lock lock(ctx->mutex);
            ASSERT_TRUE(ctx->cv.wait_for(lock, 2s, [&] { return ctx->entered; }));
        }

        auto destroy_future = std::async(std::launch::async, [&] {
            delete runner;
            delete ctx;
        });
        EXPECT_EQ(destroy_future.wait_for(50ms), std::future_status::timeout);

        {
            std::scoped_lock lock(ctx->mutex);
            ctx->release = true;
        }
        ctx->cv.notify_all();

        EXPECT_EQ(destroy_future.wait_for(2s), std::future_status::ready);
    }

    TEST_F(TaskRunnerTest, EmitsPassFinishedAfterCompletedPass)
    {
        LogState log;
        RecordingContext ctx{ .name = "a", .log = &log };
        PassFinishedWitness witness;
        g_pass_finished_witness = &witness;

        {
            iv::TaskRunner runner(1);
            runner.update_tasks(iv::TaskGraphUpdate{
                .to_create = { task("a", {}, &record_callback, &ctx) },
            });

            ASSERT_TRUE(wait_until([&] { return ctx.invocations.load() >= 1; }));
            ASSERT_TRUE(wait_until([&] { return !witness.snapshot().empty(); }));
            EXPECT_EQ(witness.snapshot().front(), runner.active_graph_revision());
        }

        g_pass_finished_witness = nullptr;
    }

} // namespace
