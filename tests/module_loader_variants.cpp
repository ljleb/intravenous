#include "module_test_utils.h"

int main()
{
    auto const repo = iv::test::repo_root();
    auto const fixtures = iv::test::test_modules_root();

    {
        iv::System system({}, false, false);
        iv::ModuleLoader loader(repo);
        iv::NodeProcessor processor = iv::test::make_processor(
            loader,
            system,
            fixtures / "noisy_saw_project" / "module.cpp"
        );
        iv::test::run_processor_ticks(processor);
    }

    {
        iv::System system({}, false, false);
        iv::ModuleLoader loader(repo);
        iv::NodeProcessor processor = iv::test::make_processor(
            loader,
            system,
            fixtures / "local_cmake"
        );
        iv::test::run_processor_ticks(processor);
    }

    return 0;
}
