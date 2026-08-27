#include "module_test_utils.h"

int main()
{
    iv::test::install_crash_handlers();

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

    auto definition = loader.load_root_definition(project_dst);
    iv::test::require(
        definition.module_id == "iv.test.behavior_project",
        "behavior project should load");
    iv::test::require(
        definition.dependencies.size() == 2,
        "behavior project should depend on exactly its root and voice closure");

    auto const project_workspace =
        iv::test::runtime_module_workspace("iv.test.behavior_project", project_dst);
    auto const project_cache = project_workspace / "cmake-build" / "CMakeCache.txt";
    iv::test::require(
        std::filesystem::exists(project_cache),
        "root module should configure in the project-local build/iv tree");

    auto const import_root = runtime_root / "build" / "iv" / "imports" / "iv" / "modules";
    auto const project_import = import_root / "iv.test.behavior_project";
    auto const voice_import = import_root / "iv.test.behavior_voice";
    iv::test::require(std::filesystem::exists(project_import), "root import should exist");
    iv::test::require(std::filesystem::exists(voice_import), "dependency import should exist");
    iv::test::require(
        iv::test::read_text(project_import).contains(project_dst.generic_string()),
        "root import should forward to its authored entry");
    iv::test::require(
        iv::test::read_text(voice_import).contains(voice_dst.generic_string()),
        "dependency import should forward to its authored entry");

    auto project_source = iv::test::read_text(project_dst / "module.cpp");
    auto const project_needle = std::string("    using namespace iv;");
    auto const project_replacement =
        std::string("    using namespace iv;\n    // behavior source marker");
    iv::test::require(project_source.contains(project_needle), "project fixture missing source marker");
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
    iv::test::require(voice_source.contains(voice_needle), "voice fixture missing source marker");
    voice_source.replace(
        voice_source.find(voice_needle),
        voice_needle.size(),
        voice_replacement);
    iv::test::write_text_advancing_timestamp(voice_dst / "module.cpp", voice_source);

    (void)loader.load_root_definition(project_dst);

    (void)loader.load_root_definition(local_dst);
    auto local_cmake = iv::test::read_text(local_dst / "CMakeLists.txt");
    local_cmake +=
        "\n# behavior cmake marker\n"
        "set(IV_TEST_CUSTOM_CMAKE_MARKER ON CACHE BOOL \"test marker\")\n";
    iv::test::write_text_advancing_timestamp(local_dst / "CMakeLists.txt", local_cmake);
    (void)loader.load_root_definition(local_dst);

    auto const local_workspace =
        iv::test::runtime_module_workspace("iv.test.local_cmake", local_dst);
    auto const local_cache = local_workspace / "cmake-build" / "CMakeCache.txt";
    iv::test::require(
        iv::test::read_text(local_cache).contains("IV_TEST_CUSTOM_CMAKE_MARKER:BOOL=ON"),
        "custom module CMake should remain authoritative");

    auto const expected_generator = iv::test::configured_build_generator();
    if (expected_generator == "Ninja") {
        iv::test::require(
            std::filesystem::exists(project_workspace / "cmake-build" / "build.ninja"),
            "project Ninja workspace should contain build.ninja");
        iv::test::require(
            std::filesystem::exists(local_workspace / "cmake-build" / "build.ninja"),
            "custom Ninja workspace should contain build.ninja");
    }

    return 0;
}
