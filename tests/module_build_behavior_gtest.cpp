#include "module_test_utils.h"

#include <gtest/gtest.h>

TEST(ModuleBuildBehavior, SourceAndCmakeEditsTriggerExpectedRebuildBehavior)
{
    auto const fixtures = iv::test::test_modules_root();
    auto const runtime_root = iv::test::runtime_modules_root();
    auto const project_dst = runtime_root / "behavior_project";
    auto const voice_dst = runtime_root / "behavior_voice";
    auto const local_dst = runtime_root / "behavior_local";

    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);
    iv::test::write_text(runtime_root / "iv_project.jsonl", "");
    iv::test::copy_directory(fixtures / "behavior_project", project_dst);
    iv::test::copy_directory(fixtures / "behavior_voice", voice_dst);
    iv::test::copy_directory(fixtures / "local_cmake", local_dst);

    iv::ModuleLoader loader(iv::test::repo_root(), {});

    {
        auto definition = loader.load_root_definition(project_dst);
        EXPECT_EQ(definition.module_id, "iv.test.behavior_project");
        ASSERT_EQ(definition.dependencies.size(), 2u);
    }

    auto const project_workspace =
        iv::test::runtime_module_workspace("iv.test.behavior_project", project_dst);
    auto const project_cache = project_workspace / "cmake-build" / "CMakeCache.txt";
    EXPECT_TRUE(std::filesystem::exists(project_cache));

    auto const import_root = runtime_root / "build" / "iv" / "imports" / "iv" / "modules";
    auto const project_import = import_root / "iv.test.behavior_project";
    auto const voice_import = import_root / "iv.test.behavior_voice";
    EXPECT_TRUE(std::filesystem::exists(project_import));
    EXPECT_TRUE(std::filesystem::exists(voice_import));
    EXPECT_NE(iv::test::read_text(project_import).find(project_dst.generic_string()), std::string::npos);
    EXPECT_NE(iv::test::read_text(voice_import).find(voice_dst.generic_string()), std::string::npos);

    auto project_source = iv::test::read_text(project_dst / "module.cpp");
    auto const project_needle = std::string("    using namespace iv;");
    auto const project_replacement =
        std::string("    using namespace iv;\n    // behavior source marker");
    ASSERT_NE(project_source.find(project_needle), std::string::npos);
    project_source.replace(
        project_source.find(project_needle),
        project_needle.size(),
        project_replacement);
    iv::test::write_text_advancing_timestamp(project_dst / "module.cpp", project_source);

    (void)loader.load_root_definition(project_dst);

    auto voice_source = iv::test::read_text(voice_dst / "module.cpp");
    auto const voice_needle =
        std::string("auto const amplitude = g.input<\"amplitude\">(0.1);");
    auto const voice_replacement =
        std::string("auto const amplitude = g.input<\"amplitude\">(0.1);/* behavior dependency marker*/");
    ASSERT_NE(voice_source.find(voice_needle), std::string::npos);
    voice_source.replace(
        voice_source.find(voice_needle),
        voice_needle.size(),
        voice_replacement);
    iv::test::write_text_advancing_timestamp(voice_dst / "module.cpp", voice_source);

    (void)loader.load_root_definition(project_dst);

    {
        auto definition = loader.load_root_definition(local_dst);
        EXPECT_EQ(definition.module_id, "iv.test.local_cmake");
    }

    auto local_cmake = iv::test::read_text(local_dst / "CMakeLists.txt");
    local_cmake +=
        "\n# behavior cmake marker\n"
        "set(IV_TEST_CUSTOM_CMAKE_MARKER ON CACHE BOOL \"test marker\")\n";
    iv::test::write_text_advancing_timestamp(local_dst / "CMakeLists.txt", local_cmake);

    (void)loader.load_root_definition(local_dst);

    auto const local_workspace =
        iv::test::runtime_module_workspace("iv.test.local_cmake", local_dst);
    auto const local_cache = local_workspace / "cmake-build" / "CMakeCache.txt";
    ASSERT_TRUE(std::filesystem::exists(local_cache));
    EXPECT_NE(
        iv::test::read_text(local_cache).find("IV_TEST_CUSTOM_CMAKE_MARKER:BOOL=ON"),
        std::string::npos);

    auto const expected_generator = iv::test::configured_build_generator();
    if (expected_generator == "Ninja") {
        EXPECT_TRUE(std::filesystem::exists(project_workspace / "cmake-build" / "build.ninja"));
        EXPECT_TRUE(std::filesystem::exists(local_workspace / "cmake-build" / "build.ninja"));
    }
}
