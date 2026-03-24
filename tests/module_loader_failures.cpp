#include "module_test_utils.h"

int main()
{
    iv::test::install_crash_handlers();

    auto const fixtures = iv::test::test_modules_root();
    auto const runtime_root = iv::test::runtime_modules_root();

    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);

    {
        iv::System system({}, false, false);
        auto loader = iv::test::make_loader();
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
        auto loader = iv::test::make_loader();

        iv::test::expect_failure(
            [&] { (void)loader.load_root(fixtures / "missing_export", system); },
            "does not declare IV_EXPORT_MODULE",
            "missing export symbol should fail"
        );
    }

    {
        iv::System system({}, false, false);
        auto loader = iv::test::make_loader();

        iv::test::expect_failure(
            [&] { (void)loader.load_root(fixtures / "build_failure", system); },
            "command failed",
            "build failure should propagate"
        );
    }

    {
        iv::System system({}, false, false);
        auto loader = iv::test::make_loader();

        iv::test::expect_failure(
            [&] { (void)loader.load_root(fixtures / "missing_dependency", system); },
            "unknown module id",
            "missing dependency id should fail"
        );
    }

    {
        iv::System system({}, false, false);
        auto loader = iv::test::make_loader({ fixtures, iv::test::duplicate_modules_root() });

        iv::test::expect_failure(
            [&] { (void)loader.load_root(fixtures / "noisy_saw_project", system); },
            "duplicate module id",
            "duplicate module id should fail"
        );
    }

    return 0;
}
