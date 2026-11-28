#include "test_workspace.h"

#include <gtest/gtest.h>
#include <cstdlib>

namespace {
class WorkspaceListener final : public testing::EmptyTestEventListener {
    void OnTestStart(testing::TestInfo const&) override
    {
        // CTest runs cases from one executable in parallel processes. Each
        // case resets only its own stable directory, including on repetition.
        std::error_code error;
        auto const workspace = iv::test::runtime_modules_root();
        std::filesystem::remove_all(workspace, error);
        ASSERT_FALSE(error) << "could not reset test workspace " << workspace
                            << ": " << error.message();
        ASSERT_EQ(::setenv("IV_BUILD_CACHE_DIR", (workspace / "build/cache").c_str(), 1), 0);
    }
};
} // namespace

int main(int argc, char** argv)
{
    iv::test::set_test_executable_name(argv[0]);
    testing::InitGoogleTest(&argc, argv);
    testing::UnitTest::GetInstance()->listeners().Append(new WorkspaceListener);
    return RUN_ALL_TESTS();
}
