#include "module_test_utils.h"
#include "module/watcher.h"

#include <gtest/gtest.h>

TEST(ModuleWatcher, ObservesDependencyEdits)
{
    auto const fixtures = iv::test::test_modules_root();
    auto const runtime_root = iv::test::runtime_modules_root() / "module_watcher";
    auto const project_src = fixtures / "noisy_saw_project";
    auto const project_dst = runtime_root / "watch_target";
    auto const voice_src = fixtures / "noisy_saw_voice";
    auto const voice_dst = runtime_root / "watch_voice";

    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);
    iv::test::copy_directory(project_src, project_dst);
    iv::test::copy_directory(voice_src, voice_dst);

    iv::test::FakeAudioDevice audio_device;
    auto loader = iv::test::make_loader({ runtime_root });
    auto graph = loader.load_root(
        project_dst,
        iv::test::module_render_config(audio_device),
        &audio_device.sample_period()
    );

    auto watcher = iv::make_dependency_watcher();
    watcher->update(graph.dependencies);
    EXPECT_FALSE(watcher->has_changes());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto module_cpp = voice_dst / "module.cpp";
    auto source = iv::test::read_text(module_cpp);
    auto needle = std::string("warper[\"anti_aliased\"] * amplitude");
    auto replacement = std::string("warper[\"anti_aliased\"] * amplitude/* watcher marker*/");
    ASSERT_NE(source.find(needle), std::string::npos);
    source.replace(source.find(needle), needle.size(), replacement);
    iv::test::write_text(module_cpp, source);

    bool saw_change = false;
    for (int i = 0; i < 40; ++i) {
        if (watcher->has_changes()) {
            saw_change = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(saw_change);
}
