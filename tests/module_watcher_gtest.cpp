#include "module_test_utils.h"
#include <intravenous/module/watcher.h>

#include <gtest/gtest.h>
#include <poll.h>

TEST(ModuleWatcher, ObservesDependencyEdits)
{
    auto const fixtures = iv::test::test_modules_root();
    auto const runtime_root = iv::test::runtime_modules_root() / "module_watcher";
    auto const project_src = fixtures / "reload_project";
    auto const project_dst = runtime_root / "watch_target";
    auto const voice_src = fixtures / "reload_voice";
    auto const voice_dst = runtime_root / "watch_voice";

    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);
    iv::test::write_text(runtime_root / "iv_project.jsonl", "");
    iv::test::copy_directory(project_src, project_dst);
    iv::test::copy_directory(voice_src, voice_dst);

    auto loader = iv::test::make_loader({});
    ASSERT_NO_THROW((void)loader.load_package_definitions(voice_dst));
    auto graph = loader.load_package_definitions(project_dst).front();

    auto watcher = iv::make_dependency_watcher();
    watcher.update(graph.dependencies);
    EXPECT_FALSE(watcher.has_changes());

    iv::test::write_text_advancing_timestamp(project_dst / "compile_commands.json", "[]\n");
    EXPECT_FALSE(watcher.has_changes());

    // Opening a package source in clangd can populate a persistent background
    // index below .cache without modifying package source. That editor cache
    // must not make the package dirty.
    auto const clangd_cache = project_dst / ".cache" / "clangd" / "index";
    std::filesystem::create_directories(clangd_cache);
    iv::test::write_text_advancing_timestamp(clangd_cache / "module.cpp.fake.idx", "index\n");
    EXPECT_FALSE(watcher.has_changes());

    // The configured graph watches its own source and every provider it
    // resolves through g.node<Id>().
    auto module_cpp = project_dst / "module.cpp";
    auto source = iv::test::read_text(module_cpp);
    auto needle = std::string("using namespace iv;");
    auto replacement = std::string("using namespace iv; /* watcher marker */");
    ASSERT_NE(source.find(needle), std::string::npos);
    source.replace(source.find(needle), needle.size(), replacement);
    iv::test::write_text_advancing_timestamp(module_cpp, source);

    bool saw_change = false;
    for (int i = 0; i < 40; ++i) {
        if (watcher.has_changes()) {
            saw_change = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(saw_change);
}

TEST(ModuleWatcher, MissingDependencyDirectoryIsReportedAsChangeWithoutThrowing)
{
    auto const fixtures = iv::test::test_modules_root();
    auto const runtime_root = iv::test::runtime_modules_root() / "module_watcher_missing_dependency";
    auto const project_src = fixtures / "reload_project";
    auto const project_dst = runtime_root / "watch_target";
    auto const voice_src = fixtures / "reload_voice";
    auto const voice_dst = runtime_root / "watch_voice";

    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);
    iv::test::write_text(runtime_root / "iv_project.jsonl", "");
    iv::test::copy_directory(project_src, project_dst);
    iv::test::copy_directory(voice_src, voice_dst);

    auto loader = iv::test::make_loader({});
    ASSERT_NO_THROW((void)loader.load_package_definitions(voice_dst));
    auto graph = loader.load_package_definitions(project_dst).front();

    auto watcher = iv::make_dependency_watcher();
    watcher.update(graph.dependencies);
    EXPECT_FALSE(watcher.has_changes());

    std::filesystem::rename(project_dst, runtime_root / "watch_target_renamed");

    EXPECT_TRUE(watcher.has_changes());
}

TEST(ModuleWatcher, WatchSetReplacementPreservesDescriptorAndClosesPreinstallChangeRace)
{
    auto const root = iv::test::fresh_module_fixture_workspace(
        "module_watcher_watch_set_replacement");
    auto const source = root / "module.cpp";
    iv::test::write_text(source, "int value = 1;\n");

    iv::ModuleDependency dependency{
        .id = "iv.test.watch_set_replacement",
        .module_dir = root,
        .entry_file = source,
        .package_stamp = iv::test::write_time(source),
    };

    auto watcher = iv::make_dependency_watcher();
    auto const descriptor = watcher.native_handle();
    watcher.update({dependency});
    EXPECT_EQ(watcher.native_handle(), descriptor);
    EXPECT_TRUE(watcher.changed_dependencies().empty());

    // The source changes before the watch set is replaced. update() removes the
    // old watches and drains their queued events, so correctness cannot depend
    // on that inotify notification surviving. The post-install stamp rescan is
    // what must preserve this change.
    iv::test::write_text_advancing_timestamp(source, "int value = 2;\n");
    watcher.update({dependency});
    EXPECT_EQ(watcher.native_handle(), descriptor);

    auto const changed = watcher.changed_dependencies();
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed.front().id, dependency.id);

    // Reinstalling with a current stamp must not turn IN_IGNORED from removed
    // watches into a false dependency change, and the pollable descriptor must
    // remain stable for PackageWatcherService across repeated updates.
    dependency.package_stamp = iv::test::write_time(source);
    watcher.update({dependency});
    EXPECT_EQ(watcher.native_handle(), descriptor);
    EXPECT_TRUE(watcher.changed_dependencies().empty());
}

TEST(ModuleWatcher, NewDependencySubdirectoryIsWatchedBeforeLaterFilesAppear)
{
    auto const root = iv::test::fresh_module_fixture_workspace(
        "module_watcher_new_subdirectory_coverage");
    auto const source = root / "module.cpp";
    iv::test::write_text(source, "int root_value = 1;\n");

    iv::ModuleDependency const dependency{
        .id = "iv.test.new_subdirectory_coverage",
        .module_dir = root,
        .entry_file = source,
        .package_stamp = iv::test::write_time(source),
    };

    auto watcher = iv::make_dependency_watcher();
    watcher.update({dependency});
    EXPECT_TRUE(watcher.changed_dependencies().empty());

    auto const late_directory = root / "created_after_watch_install";
    std::filesystem::create_directory(late_directory);

    pollfd topology_poll{
        .fd = watcher.native_handle(),
        .events = POLLIN,
        .revents = 0,
    };
    ASSERT_EQ(::poll(&topology_poll, 1, 1000), 1);
    ASSERT_NE(topology_poll.revents & POLLIN, 0);

    // Creating an empty directory does not advance the dependency's package
    // stamp because an existing source remains the maximum mtime. Processing
    // this event must nevertheless install recursive inotify coverage there.
    EXPECT_EQ(iv::test::write_time(source), dependency.package_stamp);
    (void)watcher.changed_dependencies();

    auto const late_source = late_directory / "late.cpp";
    iv::test::write_text(late_source, "int late_value = 2;\n");

    pollfd source_poll{
        .fd = watcher.native_handle(),
        .events = POLLIN,
        .revents = 0,
    };
    ASSERT_EQ(::poll(&source_poll, 1, 1000), 1)
        << "newly created dependency subdirectory was left unwatched";
    ASSERT_NE(source_poll.revents & POLLIN, 0);

    auto const changed = watcher.changed_dependencies();
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed.front().id, dependency.id);
}

TEST(ModuleWatcher, RelevantInotifyEventIsNotVetoedByUnchangedMaximumTimestamp)
{
    auto const root = iv::test::fresh_module_fixture_workspace(
        "module_watcher_event_beats_max_timestamp");
    auto const older_source = root / "older.cpp";
    auto const newest_source = root / "newest.cpp";
    iv::test::write_text(older_source, "int older_value = 1;\n");
    iv::test::write_text(newest_source, "int newest_value = 2;\n");

    auto const now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(older_source, now - std::chrono::hours(2));
    std::filesystem::last_write_time(newest_source, now - std::chrono::hours(1));
    auto const package_stamp = iv::test::write_time(newest_source);

    iv::ModuleDependency const dependency{
        .id = "iv.test.event_beats_max_timestamp",
        .module_dir = root,
        .entry_file = newest_source,
        .package_stamp = package_stamp,
    };

    auto watcher = iv::make_dependency_watcher();
    watcher.update({dependency});
    EXPECT_TRUE(watcher.changed_dependencies().empty());

    ASSERT_TRUE(std::filesystem::remove(older_source));
    ASSERT_EQ(iv::test::write_time(newest_source), package_stamp)
        << "test precondition failed: deleting the older file changed the max mtime";

    pollfd event_poll{
        .fd = watcher.native_handle(),
        .events = POLLIN,
        .revents = 0,
    };
    ASSERT_EQ(::poll(&event_poll, 1, 1000), 1);
    ASSERT_NE(event_poll.revents & POLLIN, 0);

    auto const changed = watcher.changed_dependencies();
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed.front().id, dependency.id);
}
