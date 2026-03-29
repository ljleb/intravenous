#include "module_test_utils.h"

int main()
{
    iv::test::install_crash_handlers();

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

    iv::AudioDevice audio_device({}, false);
    iv::ModuleLoader loader(repo, { runtime_root });

    auto graph_a = loader.load_root(
        reload_dst,
        iv::ModuleRenderConfig{
            .sample_rate = audio_device.config().sample_rate,
            .num_channels = audio_device.config().num_channels,
            .max_block_frames = audio_device.config().max_block_frames,
        },
        &audio_device.sample_period()
    );
    auto processor_a = std::make_unique<iv::NodeExecutor>(
        iv::NodeExecutor::create(
            std::move(graph_a.root),
            iv::test::make_resource_context(audio_device),
            iv::ExecutionTargets(iv::test::make_audio_device_provider(audio_device)),
            std::move(graph_a.module_refs)
        )
    );
    iv::test::run_processor_ticks(*processor_a);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto module_cpp = voice_dst / "module.cpp";
    auto source = iv::test::read_text(module_cpp);
    auto needle = std::string("warper[\"anti_aliased\"] * amplitude");
    auto replacement = std::string("warper[\"anti_aliased\"] * amplitude/* reload marker*/");
    iv::test::require(source.contains(needle), "reload fixture did not contain expected marker");
    source.replace(source.find(needle), needle.size(), replacement);
    iv::test::write_text(module_cpp, source);

    auto graph_b = loader.load_root(
        reload_dst,
        iv::ModuleRenderConfig{
            .sample_rate = audio_device.config().sample_rate,
            .num_channels = audio_device.config().num_channels,
            .max_block_frames = audio_device.config().max_block_frames,
        },
        &audio_device.sample_period()
    );
    auto dependency_count = graph_b.dependencies.size();
    auto processor_b = std::make_unique<iv::NodeExecutor>(
        iv::NodeExecutor::create(
            std::move(graph_b.root),
            iv::test::make_resource_context(audio_device),
            iv::ExecutionTargets(iv::test::make_audio_device_provider(audio_device)),
            std::move(graph_b.module_refs)
        )
    );
    iv::test::run_processor_ticks(*processor_b);
    iv::test::require(processor_a->num_module_refs() != 0, "old processor should still own module refs");
    iv::test::require(processor_b->num_module_refs() != 0, "new processor should own module refs");
    iv::test::require(dependency_count >= 2, "dependency graph should include nested module");
    processor_b.reset();
    processor_a.reset();

    return 0;
}
