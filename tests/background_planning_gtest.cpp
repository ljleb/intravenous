#include <intravenous/execution/background/background_planning_scheduler.h>

#include <gtest/gtest.h>

#include <array>
#include <coroutine>
#include <vector>

namespace {
using Scheduler = iv::BackgroundPlanningScheduler;

iv::CoverageTask collect(iv::CoverageRequestAccess access,
    std::array<iv::CoverageRequestId, 3> ids, std::vector<int>& values)
{
    iv::CoverageRequest<int> first{access, ids[0]};
    iv::CoverageRequest<int> second{access, ids[1]};
    iv::CoverageRequest<int> unused{access, ids[2]};
    (void)unused;
    co_await iv::when_all_coverage(first, second);
    values.push_back(first.get());
    values.push_back(second.get());
}

iv::CoverageTask publish_then_wait(iv::CoverageRequestAccess access,
    iv::CoverageRequestId id, bool& published)
{
    published = true;
    (void)co_await iv::CoverageRequest<int>{access, id};
}

iv::CoverageTask wait_then_publish(iv::CoverageRequestAccess access,
    iv::CoverageRequestId id, bool& published)
{
    (void)co_await iv::CoverageRequest<int>{access, id};
    published = true;
}

iv::CoverageTask unnamed_suspension()
{
    co_await std::suspend_always{};
}

struct OwnedContext {
    std::vector<int>& destruction;
    explicit OwnedContext(std::vector<int>& values) : destruction(values) {}
    ~OwnedContext() { destruction.push_back(2); }
};

iv::CoverageTask retaining_context(iv::CoverageRequest<int> request,
    OwnedContext& context)
{
    struct FrameLocal {
        std::vector<int>& destruction;
        ~FrameLocal() { destruction.push_back(1); }
    } local{context.destruction};
    (void)co_await request;
}

TEST(BackgroundPlanningScheduler, GroupedWaitsDoNotBlockOnAnUnusedRequest)
{
    Scheduler scheduler;
    int group = 0;
    std::array<iv::CoverageRequestId, 3> ids{};
    std::vector<int> values;
    auto& operation = scheduler.add_operation(&group, 0, false,
        [&](auto ready) {
            EXPECT_EQ(ready.size(), 1u);
            scheduler.submit(*ready[0], collect(scheduler.access(), ids, values).release());
        });
    int first = 10;
    int second = 20;
    int resolves = 0;
    ids[0] = scheduler.request(operation, [&] { ++resolves; return true; }, [&] { return &first; });
    ids[1] = scheduler.request(operation, [&] { ++resolves; return true; }, [&] { return &second; });
    ids[2] = scheduler.request(operation, []() -> bool {
        ADD_FAILURE() << "an unawaited request became a scheduling prerequisite";
        return false;
    }, []() -> void const* { return nullptr; });
    scheduler.schedule(operation);
    scheduler.run_until([&] { return operation.complete; });
    EXPECT_EQ(values, (std::vector<int>{10, 20}));
    EXPECT_EQ(resolves, 2);
    EXPECT_EQ(operation.reads, (std::vector<iv::CoverageRequestId>{ids[0], ids[1]}));
    EXPECT_FALSE(scheduler.ready(ids[2]));
}

TEST(BackgroundPlanningScheduler, IndependentPublishedInformationBreaksAFalseOperationCycle)
{
    Scheduler scheduler;
    int first_group = 0;
    int second_group = 0;
    bool first_output = false;
    bool second_output = false;
    int value = 42;
    iv::CoverageRequestId first_request = 0;
    iv::CoverageRequestId second_request = 0;
    auto& first = scheduler.add_operation(&first_group, 0, false, [&](auto ready) {
        scheduler.submit(*ready[0], publish_then_wait(
            scheduler.access(), first_request, first_output).release());
    });
    auto& second = scheduler.add_operation(&second_group, 1, true, [&](auto ready) {
        scheduler.submit(*ready[0], wait_then_publish(
            scheduler.access(), second_request, second_output).release());
    });
    first_request = scheduler.request(first, [&] { return second_output; }, [&] { return &value; });
    second_request = scheduler.request(second, [&] { return first_output; }, [&] { return &value; });
    scheduler.schedule(first);
    scheduler.schedule(second);
    scheduler.run_until([&] { return first.complete && second.complete; });
    EXPECT_TRUE(first_output);
    EXPECT_TRUE(second_output);
    EXPECT_EQ(first.reads.size(), 1u);
    EXPECT_EQ(second.reads.size(), 1u);
}

TEST(BackgroundPlanningScheduler, RejectsATrueUnresolvedInformationCycle)
{
    Scheduler scheduler;
    int first_group = 0;
    int second_group = 0;
    bool first_output = false;
    bool second_output = false;
    int value = 42;
    iv::CoverageRequestId first_request = 0;
    iv::CoverageRequestId second_request = 0;
    auto& first = scheduler.add_operation(&first_group, 0, false, [&](auto ready) {
        scheduler.submit(*ready[0], wait_then_publish(
            scheduler.access(), first_request, first_output).release());
    });
    auto& second = scheduler.add_operation(&second_group, 1, true, [&](auto ready) {
        scheduler.submit(*ready[0], wait_then_publish(
            scheduler.access(), second_request, second_output).release());
    });
    first_request = scheduler.request(first, [&] { return second_output; }, [&] { return &value; });
    second_request = scheduler.request(second, [&] { return first_output; }, [&] { return &value; });
    scheduler.schedule(first);
    scheduler.schedule(second);
    EXPECT_THROW(scheduler.run_until([&] { return first.complete && second.complete; }), std::runtime_error);
    EXPECT_TRUE(first.reads.empty());
    EXPECT_TRUE(second.reads.empty());
}

TEST(BackgroundPlanningScheduler, RejectsSuspensionWithoutNamedPrerequisites)
{
    Scheduler scheduler;
    int group = 0;
    auto& operation = scheduler.add_operation(&group, 0, false, [&](auto ready) {
        scheduler.submit(*ready[0], unnamed_suspension().release());
    });
    scheduler.schedule(operation);
    EXPECT_THROW(scheduler.run_until([&] { return operation.complete; }), std::logic_error);
}

TEST(BackgroundPlanningScheduler, CancellationDestroysFramesBeforeTheirOwnedContexts)
{
    std::vector<int> destruction;
    Scheduler scheduler;
    int group = 0;
    iv::CoverageRequestId id = 0;
    auto& operation = scheduler.add_operation(&group, 0, false, [&](auto ready) {
        auto context = std::make_unique<OwnedContext>(destruction);
        auto task = retaining_context(iv::CoverageRequest<int>{scheduler.access(), id}, *context);
        task.own_context(context.release());
        scheduler.submit(*ready[0], task.release());
    });
    id = scheduler.request(operation, [] { return false; }, []() -> void const* { return nullptr; });
    scheduler.schedule(operation);
    EXPECT_THROW(scheduler.run_until([&] { return operation.complete; }), std::runtime_error);
    EXPECT_TRUE(destruction.empty());
    scheduler.cancel();
    EXPECT_EQ(destruction, (std::vector<int>{1, 2}));
}

TEST(BackgroundPlanningScheduler, ReadySynchronousOperationsKeepTheirBatchDispatch)
{
    Scheduler scheduler;
    int group = 0;
    int calls = 0;
    auto dispatch = [&](std::span<Scheduler::Operation* const> ready) {
        ++calls;
        EXPECT_EQ(ready.size(), 2u);
    };
    auto& first = scheduler.add_operation(&group, 0, false, dispatch);
    auto& second = scheduler.add_operation(&group, 1, false, dispatch);
    scheduler.schedule(first);
    scheduler.schedule(second);
    scheduler.run_until([&] { return first.complete && second.complete; });
    EXPECT_EQ(calls, 1);
}

} // namespace
