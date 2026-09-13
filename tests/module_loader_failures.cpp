#include "module_test_utils.h"

int main()
{
    iv::test::install_crash_handlers();

    auto const fixtures = iv::test::test_modules_root();
    auto const runtime_root = iv::test::runtime_modules_root();

    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);

    {
        auto loader = iv::test::make_loader();
        auto missing_dir = runtime_root / "missing_entry";
        std::filesystem::create_directories(missing_dir);
        iv::test::expect_failure(
            [&] { (void)loader.load_package_definitions(missing_dir); },
            "iv_package.json",
            "missing manifest should fail");
    }

    {
        auto loader = iv::test::make_loader({fixtures / "missing_export"});
        iv::test::expect_failure(
            [&] { (void)loader.load_package_definitions(fixtures / "missing_export"); },
            "iv_package.json",
            "source without manifest should fail");
    }

    {
        auto loader = iv::test::make_loader({fixtures / "build_failure"});
        iv::test::expect_failure(
            [&] { (void)loader.load_package_definitions(fixtures / "build_failure"); },
            "command failed",
            "build failure should propagate");
    }

    {
        auto loader = iv::test::make_loader({fixtures / "missing_dependency"});
        iv::test::expect_failure(
            [&] { (void)loader.load_package_definitions(fixtures / "missing_dependency"); },
            "imports missing",
            "missing dependency id should fail");
    }

    {
        auto const duplicates = iv::test::duplicate_modules_root();
        auto loader = iv::test::make_loader({
            fixtures / "nested_loader_project",
            fixtures / "nested_loader_voice",
            duplicates / "one",
            duplicates / "two",
        });
        iv::test::expect_failure(
            [&] { (void)loader.load_package_definitions(fixtures / "nested_loader_project"); },
            "duplicate stable IV definition ID",
            "duplicate module id should fail");
    }

    return 0;
}
