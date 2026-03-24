#include "module_test_utils.h"

int main()
{
    iv::test::install_crash_handlers();

    auto const fixtures = iv::test::test_modules_root();

    {
        iv::System system({}, false, false);
        auto loader = iv::test::make_loader();
        iv::NodeProcessor processor = iv::test::make_processor(
            loader,
            system,
            fixtures / "noisy_saw_project" / "module.cpp"
        );
        iv::test::run_processor_ticks(processor);
    }

    {
        iv::System system({}, false, false);
        auto loader = iv::test::make_loader();
        iv::NodeProcessor processor = iv::test::make_processor(
            loader,
            system,
            fixtures / "local_cmake"
        );
        iv::test::run_processor_ticks(processor);
    }

    {
        auto moved_root = iv::test::runtime_modules_root() / "moved_modules";
        std::filesystem::remove_all(moved_root);
        iv::test::copy_directory(fixtures / "noisy_saw_project", moved_root / "project");
        iv::test::copy_directory(fixtures / "noisy_saw_voice", moved_root / "voice");

        iv::System system({}, false, false);
        auto loader = iv::test::make_loader({ moved_root });
        iv::NodeProcessor processor = iv::test::make_processor(
            loader,
            system,
            moved_root / "project"
        );
        iv::test::run_processor_ticks(processor);
    }

    return 0;
}
