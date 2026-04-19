#include "module_test_utils.h"

#include <gtest/gtest.h>

TEST(ModuleLoaderReload, OldAndNewProcessorsRetainModuleRefs)
{
    auto const repo = iv::test::repo_root();
    auto const fixtures = iv::test::test_modules_root();
    auto const runtime_root = iv::test::runtime_modules_root();
    auto const reload_src = fixtures / "reload_project";
    auto const reload_dst = runtime_root / "reload_target";
    auto const voice_src = fixtures / "reload_voice";
    auto const voice_dst = runtime_root / "reload_voice";

    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);
    iv::test::copy_directory(reload_src, reload_dst);
    iv::test::copy_directory(voice_src, voice_dst);

    iv::test::FakeAudioDevice audio_device;
    iv::Timeline timeline;
    iv::ModuleLoader loader(timeline, repo, { runtime_root });

    auto graph_a = loader.load_root(
        reload_dst,
        iv::ModuleRenderConfig{
            .sample_rate = audio_device.config().sample_rate,
            .num_channels = audio_device.config().num_channels,
            .max_block_frames = audio_device.config().max_block_frames,
        },
        &audio_device.sample_period()
    );
    iv::ExecutionTargetRegistry execution_targets(iv::test::make_audio_device_provider(audio_device));
    auto processor_a = std::make_unique<iv::NodeExecutor>(
        iv::NodeExecutor::create(
            std::move(graph_a.root),
            iv::test::make_resource_context(audio_device),
            std::move(execution_targets).to_builder(),
            std::move(graph_a.module_refs)
        )
    );
    iv::test::run_processor_ticks(audio_device, *processor_a);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto module_cpp = voice_dst / "module.cpp";
    auto source = iv::test::read_text(module_cpp);
    auto needle = std::string("auto const amplitude = g.input(\"amplitude\", 0.1);");
    auto replacement = std::string("auto const amplitude = g.input(\"amplitude\", 0.1);/* reload marker*/");
    ASSERT_NE(source.find(needle), std::string::npos);
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
    iv::ExecutionTargetRegistry execution_targets_b(iv::test::make_audio_device_provider(audio_device));
    auto processor_b = std::make_unique<iv::NodeExecutor>(
        iv::NodeExecutor::create(
            std::move(graph_b.root),
            iv::test::make_resource_context(audio_device),
            std::move(execution_targets_b).to_builder(),
            std::move(graph_b.module_refs)
        )
    );
    iv::test::run_processor_ticks(audio_device, *processor_b);
    EXPECT_NE(processor_a->num_module_refs(), 0u);
    EXPECT_NE(processor_b->num_module_refs(), 0u);
    EXPECT_GE(dependency_count, 2u);
}
