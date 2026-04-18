#include "module_test_utils.h"

#include <gtest/gtest.h>

#if !defined(_WIN32)
#include <cstdlib>
#endif

TEST(ModuleBuildBehavior, SourceAndCmakeEditsTriggerExpectedRebuildBehavior)
{
    auto const fixtures = iv::test::test_modules_root();
    auto const runtime_root = iv::test::runtime_modules_root();
    auto const project_src = fixtures / "behavior_project";
    auto const project_dst = runtime_root / "behavior_project";
    auto const voice_src = fixtures / "behavior_voice";
    auto const voice_dst = runtime_root / "behavior_voice";
    auto const local_src = fixtures / "local_cmake";
    auto const local_dst = runtime_root / "behavior_local";

    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);
    iv::test::copy_directory(project_src, project_dst);
    iv::test::copy_directory(voice_src, voice_dst);
    iv::test::copy_directory(local_src, local_dst);

    std::filesystem::remove_all(iv::test::runtime_module_workspace_root("iv.test.behavior_project", project_dst));
    std::filesystem::remove_all(iv::test::runtime_module_workspace_root("iv.test.behavior_voice", voice_dst));
    std::filesystem::remove_all(iv::test::runtime_module_workspace_root("iv.test.local_cmake", local_dst));

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

    auto const project_workspace = iv::test::runtime_module_workspace("iv.test.behavior_project", project_dst);
    auto const voice_workspace = iv::test::runtime_module_workspace("iv.test.behavior_voice", voice_dst);

    auto const project_cache = project_workspace / "build" / "CMakeCache.txt";
    auto const voice_cache = voice_workspace / "build" / "CMakeCache.txt";
    auto const project_cache_before = iv::test::write_time(project_cache);
    auto const voice_cache_before = iv::test::write_time(voice_cache);

#if IV_ENABLE_JUCE_VST
    EXPECT_FALSE(std::filesystem::exists(project_workspace / "build" / "_deps" / "juce" / "JUCEConfig.cmake"));
#endif

    auto project_source = iv::test::read_text(project_dst / "module.cpp");
    auto project_needle = std::string("SamplePortRef first_output;");
    auto project_replacement = std::string("SamplePortRef first_output;\n    // behavior source marker");
    ASSERT_NE(project_source.find(project_needle), std::string::npos);
    project_source.replace(project_source.find(project_needle), project_needle.size(), project_replacement);
    iv::test::write_text_advancing_timestamp(project_dst / "module.cpp", project_source);

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

    auto voice_source = iv::test::read_text(voice_dst / "module.cpp");
    auto voice_needle = std::string("auto const amplitude = g.input(\"amplitude\", 0.1);");
    auto voice_replacement = std::string("auto const amplitude = g.input(\"amplitude\", 0.1);/* behavior dependency marker*/");
    ASSERT_NE(voice_source.find(voice_needle), std::string::npos);
    voice_source.replace(voice_source.find(voice_needle), voice_needle.size(), voice_replacement);
    iv::test::write_text_advancing_timestamp(voice_dst / "module.cpp", voice_source);

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

    auto const local_workspace = iv::test::runtime_module_workspace("iv.test.local_cmake", local_dst);
    auto const local_configure_signature = local_workspace / "configure.signature";
    auto const local_configure_before = iv::test::write_time(local_configure_signature);

    auto local_cmake = iv::test::read_text(local_dst / "CMakeLists.txt");
    local_cmake += "\n# behavior cmake marker\n";
    iv::test::write_text_advancing_timestamp(local_dst / "CMakeLists.txt", local_cmake);

    {
        auto graph = loader.load_root(
            local_dst,
            iv::test::module_render_config(audio_device),
            &audio_device.sample_period()
        );
        (void)graph;
    }

    EXPECT_GT(iv::test::write_time(local_configure_signature), local_configure_before);

    auto const expected_generator = iv::test::configured_build_generator();
    auto const project_generator = iv::test::read_text(project_workspace / "generator.txt");
    auto const local_generator = iv::test::read_text(local_workspace / "generator.txt");
    EXPECT_EQ(project_generator, expected_generator);
    EXPECT_EQ(local_generator, expected_generator);
    if (project_generator == "Ninja") {
        EXPECT_TRUE(std::filesystem::exists(project_workspace / "build" / "build.ninja"));
        EXPECT_TRUE(std::filesystem::exists(local_workspace / "build" / "build.ninja"));
    }
}
