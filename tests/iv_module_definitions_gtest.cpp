#include "runtime_test_harness.h"

#include "runtime/iv_module_definitions.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace {
using iv::test_support::BoundRuntimeProjectIntrospection;
using iv::test_support::ScopedEnvVar;
using iv::test_support::configured_program_or_find;
using iv::test_support::copy_fixture_workspace;
using iv::test_support::make_workspace;
}

TEST(RuntimeIvModuleDefinitions, EmptyIntravenousMarkerUsesWorkspaceRoot)
{
    auto const workspace = copy_fixture_workspace("iv_module_definitions_empty_marker", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    BoundRuntimeProjectIntrospection app(workspace, iv::test::repo_root(), {});
    auto const initialized = app.initialize();

    EXPECT_EQ(initialized.module_root, std::filesystem::weakly_canonical(workspace));
    EXPECT_FALSE(initialized.module_id.empty());
}

TEST(RuntimeIvModuleDefinitions, RelativeRootModulePathResolvesAgainstWorkspaceRoot)
{
    auto const workspace = make_workspace("iv_module_definitions_relative_root");
    auto const module_root = workspace / "module";
    iv::test::copy_directory(iv::test::test_modules_root() / "local_cmake", module_root);
    iv::test_support::write_text(workspace / ".intravenous", "rootModulePath=module\n");

    BoundRuntimeProjectIntrospection app(workspace, iv::test::repo_root(), {});
    auto const initialized = app.initialize();

    EXPECT_EQ(initialized.module_root, std::filesystem::weakly_canonical(module_root));
}

TEST(RuntimeIvModuleDefinitions, MissingMarkerFailsInitialization)
{
    auto const workspace = copy_fixture_workspace("iv_module_definitions_missing_marker", "local_cmake");

    BoundRuntimeProjectIntrospection app(workspace, iv::test::repo_root(), {});
    EXPECT_THROW(
        {
            try {
                (void)app.initialize();
            } catch (std::exception const &e) {
                EXPECT_TRUE(std::string(e.what()).contains(".intravenous"));
                throw;
            }
        },
        std::exception);
}

TEST(RuntimeIvModuleDefinitions, ProjectConfigOverridesIntravenousDefaultsToolchain)
{
    auto const workspace = copy_fixture_workspace("iv_module_definitions_toolchain_override", "local_cmake");
    auto const install_dir = workspace / "install";
    std::filesystem::create_directories(install_dir);
    std::filesystem::remove_all(
        iv::test::runtime_module_workspace_root("iv.test.local_cmake", workspace));

    iv::test_support::write_text(
        install_dir / ".intravenous_defaults",
        "cCompiler=/definitely/missing-clang\n"
        "cxxCompiler=/definitely/missing-clangxx\n");

    auto const c_compiler = configured_program_or_find("clang", IV_CONFIGURED_C_COMPILER);
    auto const cxx_compiler = configured_program_or_find("clang++", IV_CONFIGURED_CXX_COMPILER);
    ASSERT_FALSE(c_compiler.empty());
    ASSERT_FALSE(cxx_compiler.empty());

    iv::test_support::write_text(
        workspace / ".intravenous",
        "cCompiler=" + c_compiler + "\n"
        "cxxCompiler=" + cxx_compiler + "\n");

    ScopedEnvVar env("INTRAVENOUS_DIR", install_dir.string());
    BoundRuntimeProjectIntrospection app(workspace, iv::test::repo_root(), {});
    auto const initialized = app.initialize();

    EXPECT_EQ(initialized.module_root, std::filesystem::weakly_canonical(workspace));
}

TEST(RuntimeIvModuleDefinitions, InitializesAndShutsDownWithoutBridges)
{
    auto const workspace =
        copy_fixture_workspace("iv_module_definitions_without_bridges", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    iv::RuntimeIvModuleDefinitions definitions(workspace, iv::test::repo_root(), {}, {});
    auto const definition = definitions.initialize();

    EXPECT_FALSE(definition.definition_id.empty());
    EXPECT_FALSE(definition.module_id.empty());
    EXPECT_FALSE(definition.module_root.empty());
    EXPECT_NE(definition.canonical_builder, nullptr);

    definitions.request_shutdown();
}
