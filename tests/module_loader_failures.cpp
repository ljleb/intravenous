#include "module_test_utils.h"

int main()
{
    auto const repo = iv::test::repo_root();
    auto const fixtures = iv::test::test_modules_root();
    auto const runtime_root = iv::test::runtime_modules_root();

    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);

    {
        iv::System system({}, false, false);
        iv::ModuleLoader loader(repo);
        auto missing_dir = runtime_root / "missing_entry";
        std::filesystem::create_directories(missing_dir);

        iv::test::expect_failure(
            [&] { (void)loader.load_root(missing_dir, system); },
            "module.cpp",
            "missing module.cpp should fail"
        );
    }

    {
        iv::System system({}, false, false);
        iv::ModuleLoader loader(repo);

        iv::test::expect_failure(
            [&] { (void)loader.load_root(fixtures / "missing_export", system); },
            "does not export",
            "missing export symbol should fail"
        );
    }

    {
        iv::System system({}, false, false);
        iv::ModuleLoader loader(repo);

        iv::test::expect_failure(
            [&] { (void)loader.load_root(fixtures / "build_failure", system); },
            "command failed",
            "build failure should propagate"
        );
    }

    return 0;
}
