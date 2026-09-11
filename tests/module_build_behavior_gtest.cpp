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

    auto const module_pch = iv::test::read_text(
        iv::test::repo_root()
        / "src/intravenous/module/template/module_pch.h");
    EXPECT_NE(
        module_pch.find("<intravenous/basic_nodes/polyphonic.h>"),
        std::string::npos);
    EXPECT_NE(
        module_pch.find("<intravenous/juce/vst_wrapper.h>"),
        std::string::npos);

    {
        auto definitions = loader.load_package_definitions(project_dst);
        auto const definition = std::ranges::find(
            definitions,
            "iv.test.behavior_project",
            &iv::ModuleLoader::LoadedDefinition::module_id);
        ASSERT_NE(definition, definitions.end());
        // A source artifact owns and watches only its implementation package.
        // behavior_voice is an independently built provider selected through
        // the package definition table, not a recursive C++ build dependency of
        // behavior_project.
        ASSERT_EQ(definition->dependencies.size(), 1u);
        EXPECT_EQ(
            definition->dependencies.front().module_dir,
            std::filesystem::weakly_canonical(project_dst));

        auto executor = iv::BlockNodeExecutor::create(
            iv::TypeErasedNode(definition->root), 8);
        auto const has_structural_saw_state = std::ranges::any_of(
            executor.layout().nodes,
            [](iv::NodeLayout::NodeRecord const& record) {
                if (!record.node_state_structure) return false;
                return std::ranges::any_of(
                    record.node_state_structure->fields,
                    [](iv::NodeStateFieldStructure const& field) {
                        return field.name == "phase" && !field.type_name.empty();
                    });
            });
        // SawOscillator::State is configured in behavior_voice. Its field type
        // reaches this host layout only through the Clang plugin, exact
        // NodeCodeKey binding in the finalizer, and the binary archive.
        EXPECT_TRUE(has_structural_saw_state);
    }

    auto const project_workspace = iv::test::runtime_module_workspace(project_dst);
    auto const project_cache = project_workspace / "cmake-build" / "CMakeCache.txt";
    EXPECT_TRUE(std::filesystem::exists(project_cache));

    std::vector<std::filesystem::path> project_package_llvm;
    for (std::filesystem::recursive_directory_iterator it(project_workspace / "out"), end;
         it != end;
         ++it) {
        if (it->is_regular_file() && it->path().filename().string().ends_with(".ivpkg.bc")) {
            project_package_llvm.push_back(it->path());
        }
        EXPECT_FALSE(
            it->is_regular_file()
            && (it->path().extension() == ".so"
                || it->path().extension() == ".dylib"
                || it->path().extension() == ".dll"));
    }
    ASSERT_EQ(project_package_llvm.size(), 1u);

    // Generic g.node<Id>() configuration has no generated provider-header
    // bootstrap. Source packages stay separate C++ targets and join only in
    // the host configuration generation.
    EXPECT_FALSE(std::filesystem::exists(
        runtime_root / "build" / "iv" / "imports" / "iv" / "nodes"));

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

    (void)loader.load_package_definitions(project_dst);

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

    (void)loader.load_package_definitions(project_dst);

    {
        auto definition = loader.load_package_definitions(local_dst).front();
        EXPECT_EQ(definition.module_id, "iv.test.local_cmake");
    }

    auto local_cmake = iv::test::read_text(local_dst / "CMakeLists.txt");
    local_cmake +=
        "\n# behavior cmake marker\n"
        "set(IV_TEST_CUSTOM_CMAKE_MARKER ON CACHE BOOL \"test marker\")\n";
    iv::test::write_text_advancing_timestamp(local_dst / "CMakeLists.txt", local_cmake);

    (void)loader.load_package_definitions(local_dst);

    auto const local_workspace = iv::test::runtime_module_workspace(local_dst);
    auto const local_cache = local_workspace / "cmake-build" / "CMakeCache.txt";
    ASSERT_TRUE(std::filesystem::exists(local_cache));
    EXPECT_NE(
        iv::test::read_text(local_cache).find("IV_TEST_CUSTOM_CMAKE_MARKER:BOOL=ON"),
        std::string::npos);

    auto const local_compile_database =
        iv::test::read_text(local_workspace / "cmake-build" / "compile_commands.json");
    EXPECT_NE(local_compile_database.find("-include "), std::string::npos);
    EXPECT_NE(local_compile_database.find("cmake_pch.hxx"), std::string::npos);

    auto const finalizer_timings =
        local_workspace / "cmake-build" / "iv-package-finalizer-timings.txt";
    ASSERT_TRUE(std::filesystem::exists(finalizer_timings));
    auto const finalizer_timings_text = iv::test::read_text(finalizer_timings);
    EXPECT_TRUE(finalizer_timings_text.starts_with("version=1\n"));
    EXPECT_NE(
        finalizer_timings_text.find("bitcode_parse_link_us="),
        std::string::npos);
    EXPECT_NE(
        finalizer_timings_text.find("package_metadata_validate_us="),
        std::string::npos);
    EXPECT_NE(
        finalizer_timings_text.find("package_metadata_inject_us="),
        std::string::npos);
    EXPECT_NE(
        finalizer_timings_text.find("package_bitcode_write_us="),
        std::string::npos);
    EXPECT_EQ(finalizer_timings_text.find("native_link_us="), std::string::npos);

    bool has_precompiled_header = false;
    for (std::filesystem::recursive_directory_iterator it(local_workspace / "cmake-build"), end;
         it != end;
         ++it) {
        auto const filename = it->path().filename();
        if (it->is_regular_file()
            && (filename == "cmake_pch.hxx.gch" || filename == "cmake_pch.hxx.pch")) {
            has_precompiled_header = true;
            break;
        }
    }
    EXPECT_TRUE(has_precompiled_header);

    auto const expected_generator = iv::test::configured_build_generator();
    if (expected_generator == "Ninja") {
        EXPECT_TRUE(std::filesystem::exists(project_workspace / "cmake-build" / "build.ninja"));
        EXPECT_TRUE(std::filesystem::exists(local_workspace / "cmake-build" / "build.ninja"));
        ASSERT_TRUE(std::filesystem::exists(
            local_workspace / "cmake-build" / "CMakeFiles" / "rules.ninja"));

        auto const local_cache_text = iv::test::read_text(local_cache);
        auto cache_path = [&](std::string const& name) {
            auto const prefix = name + ":UNINITIALIZED=";
            auto const begin = local_cache_text.find(prefix);
            if (begin == std::string::npos) return std::string{};
            auto const value_begin = begin + prefix.size();
            auto const end = local_cache_text.find('\n', value_begin);
            return local_cache_text.substr(
                value_begin,
                end == std::string::npos ? std::string::npos : end - value_begin);
        };
        auto const plugin_path = cache_path("IV_CLANG_SOURCE_INTROSPECTION_PLUGIN");
        auto const finalizer_path = cache_path("IV_PACKAGE_FINALIZER");
        ASSERT_FALSE(plugin_path.empty());
        ASSERT_FALSE(finalizer_path.empty());

        auto const local_ninja = iv::test::read_text(
            local_workspace / "cmake-build" / "build.ninja");
        // Package source is compiled to full-LTO LLVM objects. The finalizer is
        // a direct custom command that combines those objects into .ivpkg.bc;
        // no native shared-library link rule exists for the package.
        EXPECT_NE(local_ninja.find(plugin_path), std::string::npos);
        EXPECT_NE(local_ninja.find(finalizer_path), std::string::npos);
        EXPECT_NE(local_ninja.find("--output="), std::string::npos);
        EXPECT_NE(local_ninja.find(".ivpkg.bc"), std::string::npos);
        EXPECT_NE(local_compile_database.find("-flto=full"), std::string::npos);
        EXPECT_NE(local_compile_database.find("-O0"), std::string::npos);
        EXPECT_EQ(local_ninja.find("CXX_SHARED_LIBRARY_LINKER__iv_package"), std::string::npos);
    }

    iv::ModuleLoader time_trace_loader(
        iv::test::repo_root(), {},
        iv::ModuleLoaderToolchainConfig{.clang_time_trace = true});
    (void)time_trace_loader.load_package_definitions(local_dst);

    auto const traced_compile_database = iv::test::read_text(
        local_workspace / "cmake-build" / "compile_commands.json");
    EXPECT_NE(traced_compile_database.find("-ftime-trace"), std::string::npos);

    bool has_clang_time_trace = false;
    for (std::filesystem::recursive_directory_iterator it(local_workspace / "cmake-build"), end;
         it != end;
         ++it) {
        if (it->is_regular_file()
            && it->path().extension() == ".json"
            && it->path().string().contains("CMakeFiles")) {
            has_clang_time_trace = true;
            break;
        }
    }
    EXPECT_TRUE(has_clang_time_trace);
}
