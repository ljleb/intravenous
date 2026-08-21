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
            [&] { (void)loader.load_root_definition(missing_dir); },
            "iv_module.json",
            "missing manifest should fail");
    }

    {
        auto loader = iv::test::make_loader();
        iv::test::expect_failure(
            [&] { (void)loader.load_root_definition(fixtures / "missing_export"); },
            "iv_module.json",
            "legacy source without manifest should fail");
    }

    {
        auto loader = iv::test::make_loader();
        iv::test::expect_failure(
            [&] { (void)loader.load_root_definition(fixtures / "build_failure"); },
            "command failed",
            "build failure should propagate");
    }

    {
        auto loader = iv::test::make_loader();
        iv::test::expect_failure(
            [&] { (void)loader.load_root_definition(fixtures / "missing_dependency"); },
            "imports missing",
            "missing dependency id should fail");
    }

    {
        auto loader = iv::test::make_loader({fixtures, iv::test::duplicate_modules_root()});
        iv::test::expect_failure(
            [&] { (void)loader.load_root_definition(fixtures / "nested_loader_project"); },
            "duplicate module id",
            "duplicate module id should fail");
    }

    return 0;
}
