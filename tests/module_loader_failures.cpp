#include "module_test_utils.h"

int main()
{
    iv::test::install_crash_handlers();

    auto const fixtures = iv::test::test_modules_root();
    auto const runtime_root = iv::test::runtime_modules_root();

    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);

    {
        iv::test::FakeAudioDevice audio_device;
        auto loader = iv::test::make_loader();
        auto missing_dir = runtime_root / "missing_entry";
        std::filesystem::create_directories(missing_dir);

        iv::test::expect_failure(
            [&] { (void)loader.load_root(missing_dir, iv::test::module_render_config(audio_device), &audio_device.sample_period()); },
            "module.cpp",
            "missing module.cpp should fail"
        );
    }

    {
        iv::test::FakeAudioDevice audio_device;
        auto loader = iv::test::make_loader();

        iv::test::expect_failure(
            [&] { (void)loader.load_root(fixtures / "missing_export", iv::test::module_render_config(audio_device), &audio_device.sample_period()); },
            "does not declare IV_EXPORT_MODULE",
            "missing export symbol should fail"
        );
    }

    {
        iv::test::FakeAudioDevice audio_device;
        auto loader = iv::test::make_loader();

        iv::test::expect_failure(
            [&] { (void)loader.load_root(fixtures / "build_failure", iv::test::module_render_config(audio_device), &audio_device.sample_period()); },
            "command failed",
            "build failure should propagate"
        );
    }

    {
        iv::test::FakeAudioDevice audio_device;
        auto loader = iv::test::make_loader();

        iv::test::expect_failure(
            [&] { (void)loader.load_root(fixtures / "missing_dependency", iv::test::module_render_config(audio_device), &audio_device.sample_period()); },
            "unknown module id",
            "missing dependency id should fail"
        );
    }

    {
        iv::test::FakeAudioDevice audio_device;
        auto loader = iv::test::make_loader({ fixtures, iv::test::duplicate_modules_root() });

        iv::test::expect_failure(
            [&] { (void)loader.load_root(fixtures / "noisy_saw_project", iv::test::module_render_config(audio_device), &audio_device.sample_period()); },
            "duplicate module id",
            "duplicate module id should fail"
        );
    }

    return 0;
}
