#include "module_test_utils.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {
    template<typename Fn>
    void expect_failure_contains(Fn&& fn, std::string_view needle)
    {
        try {
            fn();
        } catch (std::exception const& e) {
            EXPECT_NE(std::string_view(e.what()).find(needle), std::string_view::npos);
            return;
        }

        FAIL() << "expected exception containing: " << needle;
    }
}

TEST(ModuleLoaderFailures, MissingModuleCppFails)
{
    auto const runtime_root = iv::test::runtime_modules_root();
    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);

    iv::test::FakeAudioDevice audio_device;
    auto loader = iv::test::make_loader();
    auto missing_dir = runtime_root / "missing_entry";
    std::filesystem::create_directories(missing_dir);

    expect_failure_contains(
        [&] { (void)loader.load_root(missing_dir, iv::test::module_render_config(audio_device), &audio_device.sample_period()); },
        "module.cpp"
    );
}

TEST(ModuleLoaderFailures, MissingExportSymbolFails)
{
    auto const fixtures = iv::test::test_modules_root();
    iv::test::FakeAudioDevice audio_device;
    auto loader = iv::test::make_loader();

    expect_failure_contains(
        [&] { (void)loader.load_root(fixtures / "missing_export", iv::test::module_render_config(audio_device), &audio_device.sample_period()); },
        "does not declare IV_EXPORT_MODULE"
    );
}

TEST(ModuleLoaderFailures, BuildFailurePropagates)
{
    auto const fixtures = iv::test::test_modules_root();
    iv::test::FakeAudioDevice audio_device;
    auto loader = iv::test::make_loader();

    expect_failure_contains(
        [&] { (void)loader.load_root(fixtures / "build_failure", iv::test::module_render_config(audio_device), &audio_device.sample_period()); },
        "command failed"
    );
}

TEST(ModuleLoaderFailures, MissingDependencyFails)
{
    auto const fixtures = iv::test::test_modules_root();
    iv::test::FakeAudioDevice audio_device;
    auto loader = iv::test::make_loader();

    expect_failure_contains(
        [&] { (void)loader.load_root(fixtures / "missing_dependency", iv::test::module_render_config(audio_device), &audio_device.sample_period()); },
        "unknown module id"
    );
}

TEST(ModuleLoaderFailures, DuplicateModuleIdFails)
{
    auto const fixtures = iv::test::test_modules_root();
    iv::test::FakeAudioDevice audio_device;
    auto loader = iv::test::make_loader({ fixtures, iv::test::duplicate_modules_root() });

    expect_failure_contains(
        [&] { (void)loader.load_root(fixtures / "nested_loader_project", iv::test::module_render_config(audio_device), &audio_device.sample_period()); },
        "duplicate module id"
    );
}
