#include "module_test_utils.h"

int main()
{
    auto const repo = iv::test::repo_root();
    auto const fixtures = iv::test::test_modules_root();
    auto const runtime_root = iv::test::runtime_modules_root();
    auto const reload_src = fixtures / "noisy_saw_project";
    auto const reload_dst = runtime_root / "reload_target";
    auto const voice_src = fixtures / "noisy_saw_voice";
    auto const voice_dst = runtime_root / "reload_voice";

    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);
    iv::test::copy_directory(reload_src, reload_dst);
    iv::test::copy_directory(voice_src, voice_dst);

    iv::System system({}, false, false);
    iv::ModuleLoader loader(repo, { runtime_root });

    auto graph_a = loader.load_root(reload_dst, system);
    system.activate_root(graph_a.sink_count != 0);
    auto processor_a = std::make_unique<iv::NodeProcessor>(
        system.wrap_root(std::move(graph_a.root), graph_a.sink_count != 0),
        std::move(graph_a.module_refs)
    );
    iv::test::run_processor_ticks(*processor_a);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto module_cpp = voice_dst / "module.cpp";
    auto source = iv::test::read_text(module_cpp);
    auto needle = std::string("return iv::modules::noisy_saw();");
    auto replacement = std::string("return iv::modules::noisy_saw();\n        // reload marker");
    iv::test::require(source.contains(needle), "reload fixture did not contain expected marker");
    source.replace(source.find(needle), needle.size(), replacement);
    iv::test::write_text(module_cpp, source);

    auto graph_b = loader.load_root(reload_dst, system);
    auto dependency_count = graph_b.dependencies.size();
    system.activate_root(graph_b.sink_count != 0);
    auto processor_b = std::make_unique<iv::NodeProcessor>(
        system.wrap_root(std::move(graph_b.root), graph_b.sink_count != 0),
        std::move(graph_b.module_refs)
    );
    iv::test::run_processor_ticks(*processor_b);
    iv::test::require(processor_a->num_module_refs() != 0, "old processor should still own module refs");
    iv::test::require(processor_b->num_module_refs() != 0, "new processor should own module refs");
    iv::test::require(dependency_count >= 2, "dependency graph should include nested module");

    return 0;
}
