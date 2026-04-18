#include "module_test_utils.h"

#if !defined(_WIN32)
#include <cstdlib>
#endif

int main()
{
    iv::test::install_crash_handlers();

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
    iv::test::require(
        !std::filesystem::exists(project_workspace / "build" / "_deps" / "juce" / "JUCEConfig.cmake"),
        "juce-enabled runtime module should reuse the core runtime's JUCE support instead of configuring its own _deps/juce tree"
    );
#endif

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto project_source = iv::test::read_text(project_dst / "module.cpp");
    auto project_needle = std::string("SamplePortRef first_output;");
    auto project_replacement = std::string("SamplePortRef first_output;\n    // behavior source marker");
    iv::test::require(project_source.contains(project_needle), "project fixture missing source marker");
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

    iv::test::require(iv::test::write_time(project_cache) == project_cache_before, "source edit should not reconfigure root module");
    iv::test::require(iv::test::write_time(voice_cache) == voice_cache_before, "source edit should not reconfigure dependency module");

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto voice_source = iv::test::read_text(voice_dst / "module.cpp");
    auto voice_needle = std::string("auto const amplitude = g.input(\"amplitude\", 0.1);");
    auto voice_replacement = std::string("auto const amplitude = g.input(\"amplitude\", 0.1);/* behavior dependency marker*/");
    iv::test::require(voice_source.contains(voice_needle), "voice fixture missing source marker");
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

    iv::test::require(iv::test::write_time(project_cache) == project_cache_mid, "nested source edit should not reconfigure root module");
    iv::test::require(iv::test::write_time(voice_cache) == voice_cache_mid, "nested source edit should not reconfigure dependency module");

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

    iv::test::require(
        iv::test::write_time(local_configure_signature) > local_configure_before,
        "local CMake edit should force reconfigure"
    );

    auto const expected_generator = iv::test::configured_build_generator();
    auto const project_generator = iv::test::read_text(project_workspace / "generator.txt");
    auto const local_generator = iv::test::read_text(local_workspace / "generator.txt");
    iv::test::require(project_generator == expected_generator, "project workspace should preserve top-level generator");
    iv::test::require(local_generator == expected_generator, "local workspace should preserve top-level generator");
    if (project_generator == "Ninja") {
        iv::test::require(std::filesystem::exists(project_workspace / "build" / "build.ninja"), "ninja workspace should contain build.ninja");
        iv::test::require(std::filesystem::exists(local_workspace / "build" / "build.ninja"), "local ninja workspace should contain build.ninja");
    }

    return 0;
}
