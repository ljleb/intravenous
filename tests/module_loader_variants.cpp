#include "module_test_utils.h"

int main()
{
    iv::test::install_crash_handlers();

    auto const fixtures = iv::test::test_modules_root();

    {
        iv::AudioDevice audio_device({}, false);
        auto loader = iv::test::make_loader();
        iv::NodeExecutor executor = iv::test::make_executor(
            loader,
            audio_device,
            fixtures / "noisy_saw_project" / "module.cpp"
        );
        iv::test::run_processor_ticks(executor);
    }

    {
        iv::AudioDevice audio_device({}, false);
        auto loader = iv::test::make_loader();
        iv::NodeExecutor executor = iv::test::make_executor(
            loader,
            audio_device,
            fixtures / "local_cmake"
        );
        iv::test::run_processor_ticks(executor);
    }

    {
        auto moved_root = iv::test::runtime_modules_root() / "moved_modules";
        std::filesystem::remove_all(moved_root);
        iv::test::copy_directory(fixtures / "noisy_saw_project", moved_root / "project");
        iv::test::copy_directory(fixtures / "noisy_saw_voice", moved_root / "voice");

        iv::AudioDevice audio_device({}, false);
        auto loader = iv::test::make_loader({ moved_root });
        iv::NodeExecutor executor = iv::test::make_executor(
            loader,
            audio_device,
            moved_root / "project"
        );
        iv::test::run_processor_ticks(executor);
    }

    return 0;
}
