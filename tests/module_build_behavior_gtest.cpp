#include "module_test_utils.h"

#include <gtest/gtest.h>

#if !defined(_WIN32)
#include <cstdlib>
#endif

TEST(ModuleBuildBehavior, SourceAndCmakeEditsTriggerExpectedRebuildBehavior)
{
    auto const fixtures = iv::test::test_modules_root();
    auto const runtime_root = iv::test::runtime_modules_root();
    auto const project_src = fixtures / "noisy_saw_project";
    auto const project_dst = runtime_root / "behavior_project";
    auto const voice_src = fixtures / "noisy_saw_voice";
    auto const voice_dst = runtime_root / "behavior_voice";
    auto const local_src = fixtures / "local_cmake";
    auto const local_dst = runtime_root / "behavior_local";

    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);
    iv::test::copy_directory(project_src, project_dst);
    iv::test::copy_directory(voice_src, voice_dst);
    iv::test::copy_directory(local_src, local_dst);

    std::filesystem::remove_all(iv::test::runtime_module_workspace("iv.test.noisy_saw_project", project_dst));
    std::filesystem::remove_all(iv::test::runtime_module_workspace("iv.test.noisy_saw_voice", voice_dst));
    std::filesystem::remove_all(iv::test::runtime_module_workspace("iv.test.local_cmake", local_dst));

    iv::test::FakeAudioDevice audio_device;
    iv::ModuleLoader loader(iv::test::repo_root(), { runtime_root });

    {
        auto graph = loader.load_root(
            project_dst,
            iv::test::module_render_config(audio_device),
            &audio_device.sample_period()
        );
        (void)graph;
    }

    auto const project_workspace = iv::test::runtime_module_workspace("iv.test.noisy_saw_project", project_dst);
    auto const voice_workspace = iv::test::runtime_module_workspace("iv.test.noisy_saw_voice", voice_dst);
    auto const local_workspace = iv::test::runtime_module_workspace("iv.test.local_cmake", local_dst);

    auto const project_cache = project_workspace / "build" / "CMakeCache.txt";
    auto const voice_cache = voice_workspace / "build" / "CMakeCache.txt";
    auto const project_cache_before = iv::test::write_time(project_cache);
    auto const voice_cache_before = iv::test::write_time(voice_cache);

#if IV_ENABLE_JUCE_VST
    EXPECT_FALSE(std::filesystem::exists(project_workspace / "build" / "_deps" / "juce" / "JUCEConfig.cmake"));
#endif

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto project_source = iv::test::read_text(project_dst / "module.cpp");
    auto project_needle = std::string("SignalRef first_noise;");
    auto project_replacement = std::string("SignalRef first_noise;\n        // behavior source marker");
    ASSERT_NE(project_source.find(project_needle), std::string::npos);
    project_source.replace(project_source.find(project_needle), project_needle.size(), project_replacement);
    iv::test::write_text(project_dst / "module.cpp", project_source);

    {
        auto graph = loader.load_root(
            project_dst,
            iv::test::module_render_config(audio_device),
            &audio_device.sample_period()
        );
        (void)graph;
    }

    EXPECT_EQ(iv::test::write_time(project_cache), project_cache_before);
    EXPECT_EQ(iv::test::write_time(voice_cache), voice_cache_before);

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto voice_source = iv::test::read_text(voice_dst / "module.cpp");
    auto voice_needle = std::string("\"amplitude\"_P = amplitude,");
    auto voice_replacement = std::string("\"amplitude\"_P = amplitude,/* behavior dependency marker*/");
    ASSERT_NE(voice_source.find(voice_needle), std::string::npos);
    voice_source.replace(voice_source.find(voice_needle), voice_needle.size(), voice_replacement);
    iv::test::write_text(voice_dst / "module.cpp", voice_source);

    auto const project_cache_mid = iv::test::write_time(project_cache);
    auto const voice_cache_mid = iv::test::write_time(voice_cache);

    {
        auto graph = loader.load_root(
            project_dst,
            iv::test::module_render_config(audio_device),
            &audio_device.sample_period()
        );
        (void)graph;
    }

    EXPECT_EQ(iv::test::write_time(project_cache), project_cache_mid);
    EXPECT_EQ(iv::test::write_time(voice_cache), voice_cache_mid);

    {
        auto graph = loader.load_root(
            local_dst,
            iv::test::module_render_config(audio_device),
            &audio_device.sample_period()
        );
        (void)graph;
    }

    auto const local_configure_signature = local_workspace / "configure.signature";
    auto const local_configure_before = iv::test::write_time(local_configure_signature);

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto local_cmake = iv::test::read_text(local_dst / "CMakeLists.txt");
    local_cmake += "\n# behavior cmake marker\n";
    iv::test::write_text(local_dst / "CMakeLists.txt", local_cmake);

    {
        auto graph = loader.load_root(
            local_dst,
            iv::test::module_render_config(audio_device),
            &audio_device.sample_period()
        );
        (void)graph;
    }

    EXPECT_GT(iv::test::write_time(local_configure_signature), local_configure_before);

    if (auto ninja_path = std::getenv("IV_RUNTIME_MODULE_PREFER_NINJA"); ninja_path == nullptr || std::string_view(ninja_path) != "0") {
        auto configured = iv::test::read_text(project_workspace / "generator.txt");
        if (configured == "Ninja") {
            EXPECT_TRUE(std::filesystem::exists(project_workspace / "build" / "build.ninja"));
        }
    }

    std::filesystem::remove_all(iv::test::runtime_module_workspace("iv.test.local_cmake", local_dst));
#if defined(_WIN32)
    _putenv_s("IV_RUNTIME_MODULE_PREFER_NINJA", "0");
#else
    setenv("IV_RUNTIME_MODULE_PREFER_NINJA", "0", 1);
#endif
    {
        iv::ModuleLoader fallback_loader(iv::test::repo_root(), { runtime_root });
        auto graph = fallback_loader.load_root(
            local_dst,
            iv::test::module_render_config(audio_device),
            &audio_device.sample_period()
        );
        (void)graph;
    }
    auto fallback_generator = iv::test::read_text(local_workspace / "generator.txt");
    EXPECT_NE(fallback_generator, "Ninja");
#if defined(_WIN32)
    _putenv_s("IV_RUNTIME_MODULE_PREFER_NINJA", "");
#else
    unsetenv("IV_RUNTIME_MODULE_PREFER_NINJA");
#endif
}
