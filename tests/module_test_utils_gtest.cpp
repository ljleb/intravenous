#include "module_test_utils.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
bool has_complete_fixture_contract_fixture()
{
    try {
        auto const workspace = iv::test::read_only_module_fixture_workspace("fixture_contract");
        auto const module_source = workspace / "module.cpp";
        if (!std::filesystem::is_regular_file(module_source) ||
            !std::filesystem::is_regular_file(workspace / "iv_package.json") ||
            !std::filesystem::is_regular_file(workspace / "CMakeLists.txt")) {
            return false;
        }

        std::ifstream input(module_source);
        std::string source{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        return (input.good() || input.eof()) &&
            source.contains("fixture contract source");
    } catch (...) {
        return false;
    }
}
} // namespace

TEST(ModuleTestUtils, ReadOnlyFixtureRemainsCompleteDuringConcurrentProcessAccess)
{
#if defined(_WIN32)
    GTEST_SKIP() << "process stress test currently uses POSIX fork";
#else
    constexpr int worker_count = 8;
    constexpr int iterations_per_worker = 100;
    auto const workspace =
        iv::test::shared_test_fixtures_root() / "fixture_contract";
    std::filesystem::remove_all(workspace);
    std::vector<pid_t> workers;
    workers.reserve(worker_count);

    for (int worker = 0; worker < worker_count; ++worker) {
        auto const pid = fork();
        ASSERT_NE(pid, -1);
        if (pid == 0) {
            for (int iteration = 0; iteration < iterations_per_worker; ++iteration) {
                if (!has_complete_fixture_contract_fixture()) {
                    _exit(1);
                }
            }
            _exit(0);
        }
        workers.push_back(pid);
    }

    for (auto const worker : workers) {
        int status = 0;
        ASSERT_EQ(waitpid(worker, &status, 0), worker);
        ASSERT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 0);
    }
#endif
}

TEST(ModuleTestUtils, MutableFixturesAreIsolatedFromTheirSourceAndEachOther)
{
    auto const first = iv::test::mutable_module_fixture_workspace(
        "module_test_utils_first", "local_cmake");
    auto const second = iv::test::mutable_module_fixture_workspace(
        "module_test_utils_second", "local_cmake");
    auto const source = iv::test::test_modules_root() / "local_cmake" / "module.cpp";
    auto const marker = std::string("// mutable fixture marker\n");

    iv::test::write_text(first / "module.cpp", marker);

    EXPECT_EQ(iv::test::read_text(first / "module.cpp"), marker);
    EXPECT_NE(iv::test::read_text(second / "module.cpp"), marker);
    EXPECT_NE(iv::test::read_text(source), marker);
}


TEST(ModuleTestUtils, WorkspaceFollowsExecutableSuiteAndTestName)
{
    auto const root = iv::test::runtime_modules_root();
    auto const* info = testing::UnitTest::GetInstance()->current_test_info();
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(root.filename(), iv::test::test_workspace_component(info->name()));
    EXPECT_EQ(root.parent_path().filename(), iv::test::test_workspace_component(info->test_suite_name()));
    EXPECT_EQ(root.parent_path().parent_path().filename(), iv::test::test_executable_name());
    EXPECT_EQ(root.parent_path().parent_path().parent_path(),
        iv::test::configured_build_root() / "test_runtime_modules");
}

TEST(ModuleTestUtils, WorkspaceStartsCleanAndPreservesFilesForTheCurrentRun)
{
    auto const root = iv::test::runtime_modules_root();
    auto const marker = root / "previous-run.txt";
    EXPECT_FALSE(std::filesystem::exists(marker));
    std::filesystem::create_directories(root);
    iv::test::write_text(marker, "current run");
    EXPECT_EQ(iv::test::runtime_modules_root(), root);
    EXPECT_EQ(iv::test::read_text(marker), "current run");
    // Leave the marker behind: another process or --gtest_repeat iteration
    // must reset this same test directory before entering the body again.
}

TEST(ModuleTestUtils, FixtureNamesStayStableAcrossCallSites)
{
    auto const first = iv::test::fresh_module_fixture_workspace("stable_fixture");
    iv::test::write_text(first / "stale.txt", "old contents");
    auto const second = iv::test::fresh_module_fixture_workspace("stable_fixture");
    EXPECT_EQ(first, second);
    EXPECT_EQ(second, iv::test::runtime_modules_root() / "stable_fixture");
    EXPECT_FALSE(std::filesystem::exists(second / "stale.txt"));
}

TEST(ModuleTestUtils, WorkspaceComponentsDoNotMergeNamesOrIntroduceDirectories)
{
    EXPECT_NE(iv::test::test_workspace_component("Suite/0"),
        iv::test::test_workspace_component("Suite_0"));
    EXPECT_NE(iv::test::test_workspace_component("name%2F0"),
        iv::test::test_workspace_component("name/0"));
    auto const escaped = iv::test::test_workspace_component("../sibling");
    EXPECT_FALSE(std::filesystem::path(escaped).has_parent_path());
}
