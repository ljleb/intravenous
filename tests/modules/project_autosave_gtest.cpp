#include <intravenous/runtime/project_autosave.h>

#include <gtest/gtest.h>

#include <chrono>

namespace {
using namespace std::chrono_literals;

TEST(ProjectAutosaveTest, IgnoresMutationsUntilProjectLoadCompletes)
{
    iv::ProjectAutosave autosave;
    auto const start = iv::ProjectAutosave::TimePoint{};

    autosave.project_state_changed(start);
    EXPECT_FALSE(autosave.take_due_save(start + 10s));

    autosave.project_loaded(start + 10s);
    EXPECT_FALSE(autosave.take_due_save(start + 20s));
}

TEST(ProjectAutosaveTest, CoalescesMutationsAndPreservesChangesDuringSave)
{
    iv::ProjectAutosave autosave;
    auto const start = iv::ProjectAutosave::TimePoint{};
    autosave.project_loaded(start);

    autosave.project_state_changed(start);
    autosave.project_state_changed(start + 500ms);
    EXPECT_FALSE(autosave.take_due_save(start + 3499ms));
    EXPECT_TRUE(autosave.take_due_save(start + 3500ms));

    autosave.project_state_changed(start + 3600ms);
    autosave.save_succeeded(start + 3700ms);
    EXPECT_FALSE(autosave.take_due_save(start + 6599ms));
    EXPECT_TRUE(autosave.take_due_save(start + 6600ms));
}

TEST(ProjectAutosaveTest, ContinuousEditsReachDeadlineAndIdleSaveClearsIt)
{
    iv::ProjectAutosave autosave;
    auto const start = iv::ProjectAutosave::TimePoint{};
    autosave.project_loaded(start);
    autosave.project_state_changed(start);
    for (int second = 1; second < 10; ++second) {
        autosave.project_state_changed(start + second * 1s);
        EXPECT_FALSE(autosave.take_due_save(start + second * 1s));
    }
    EXPECT_TRUE(autosave.take_due_save(start + 10s));
    autosave.save_succeeded(start + 10s);
    autosave.project_state_changed(start + 11s);
    EXPECT_TRUE(autosave.take_due_save(start + 14s));
    autosave.save_succeeded(start + 14s);
    EXPECT_FALSE(autosave.take_due_save(start + 21s));
    EXPECT_FALSE(autosave.take_pending_save());

    for (int second = 22; second < 32; ++second) {
        autosave.project_state_changed(start + second * 1s);
        EXPECT_FALSE(autosave.take_due_save(start + second * 1s));
    }
    EXPECT_TRUE(autosave.take_due_save(start + 32s));
}

TEST(ProjectAutosaveTest, FailedSaveRetriesAndKeepsPendingEdits)
{
    iv::ProjectAutosave autosave;
    auto const start = iv::ProjectAutosave::TimePoint{};
    autosave.project_loaded(start);
    autosave.project_state_changed(start);
    EXPECT_TRUE(autosave.take_due_save(start + 3s));
    autosave.save_failed(start + 3s);
    autosave.project_state_changed(start + 4s);
    EXPECT_FALSE(autosave.take_due_save(start + 7s));
    EXPECT_TRUE(autosave.take_due_save(start + 8s));
    autosave.save_succeeded(start + 8s);
    EXPECT_FALSE(autosave.take_due_save(start + 30s));
}

TEST(ProjectAutosaveTest, DefersPendingSaveWhileDisabled)
{
    iv::ProjectAutosave autosave;
    auto const start = iv::ProjectAutosave::TimePoint{};
    autosave.project_loaded(start);
    autosave.project_state_changed(start);
    autosave.set_enabled(false, start + 500ms);

    EXPECT_FALSE(autosave.take_due_save(start + 10s));

    autosave.set_enabled(true, start + 10s);
    EXPECT_TRUE(autosave.take_due_save(start + 10s));
}
} // namespace
