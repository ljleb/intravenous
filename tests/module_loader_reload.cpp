#include "module_test_utils.h"

int main()
{
    auto const repo = iv::test::repo_root();
    auto const fixtures = iv::test::test_modules_root();
    auto const runtime_root = iv::test::runtime_modules_root();
    auto const reload_src = fixtures / "noisy_saw_project";
    auto const reload_dst = runtime_root / "reload_target";

    std::filesystem::remove_all(reload_dst);
    iv::test::copy_directory(reload_src, reload_dst);

    iv::System system({}, false, false);
    iv::ModuleLoader loader(repo);

    auto graph_a = loader.load_root(reload_dst, system);
    auto stamp_a = graph_a.source_stamp;
    system.activate_root(graph_a.sink_count != 0);
    auto processor_a = std::make_unique<iv::NodeProcessor>(
        system.wrap_root(std::move(graph_a.root), graph_a.sink_count != 0),
        std::move(graph_a.module_refs)
    );
    iv::test::run_processor_ticks(*processor_a);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto module_cpp = reload_dst / "module.cpp";
    auto source = iv::test::read_text(module_cpp);
    auto needle = std::string("auto const level_knob = 0.1;");
    auto replacement = std::string("auto const level_knob = 0.2;");
    iv::test::require(source.contains(needle), "reload fixture did not contain expected marker");
    source.replace(source.find(needle), needle.size(), replacement);
    iv::test::write_text(module_cpp, source);

    iv::test::require(loader.source_changed(reload_dst, stamp_a), "module change was not detected");

    auto graph_b = loader.load_root(reload_dst, system);
    system.activate_root(graph_b.sink_count != 0);
    auto processor_b = std::make_unique<iv::NodeProcessor>(
        system.wrap_root(std::move(graph_b.root), graph_b.sink_count != 0),
        std::move(graph_b.module_refs)
    );
    iv::test::run_processor_ticks(*processor_b);
    iv::test::require(processor_a->num_module_refs() != 0, "old processor should still own module refs");
    iv::test::require(processor_b->num_module_refs() != 0, "new processor should own module refs");

    return 0;
}
