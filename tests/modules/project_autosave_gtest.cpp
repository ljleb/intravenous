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
    EXPECT_FALSE(autosave.take_due_save(start + 1499ms));
    EXPECT_TRUE(autosave.take_due_save(start + 1500ms));

    autosave.project_state_changed(start + 1500ms);
    autosave.save_succeeded(start + 1500ms);
    EXPECT_FALSE(autosave.take_due_save(start + 2499ms));
    EXPECT_TRUE(autosave.take_due_save(start + 2500ms));
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
