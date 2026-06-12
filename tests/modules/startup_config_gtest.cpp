#include "../module_test_utils.h"

#include <intravenous/runtime/startup_config.h>

#include <gtest/gtest.h>

#include <filesystem>

namespace {
using iv::test_support::ScopedEnvVar;
using iv::test_support::configured_program_or_find;
using iv::test_support::mutable_module_fixture_workspace;
using iv::test_support::make_workspace;
}

TEST(StartupConfig, EmptyIntravenousMarkerUsesWorkspaceRoot)
{
    auto const workspace = mutable_module_fixture_workspace("startup_config_empty_marker", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const initialized = startup_config.initialize();

    EXPECT_EQ(initialized.workspace_root, std::filesystem::weakly_canonical(workspace));
    EXPECT_EQ(initialized.execution.block_size, 256u);
    EXPECT_EQ(initialized.execution.sample_rate, 48000u);
    EXPECT_EQ(initialized.execution.compiled_sample_cache_chunk_size_multiplier, 16u);
}

TEST(StartupConfig, RootModulePathIsRejected)
{
    auto const workspace = make_workspace("startup_config_relative_root");
    auto const module_root = workspace / "module";
    iv::test::copy_directory(iv::test::test_modules_root() / "local_cmake", module_root);
    iv::test_support::write_text(workspace / ".intravenous", "rootModulePath=module\n");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    EXPECT_THROW(
        {
            try {
                (void)startup_config.initialize();
            } catch (std::exception const &e) {
                EXPECT_TRUE(std::string(e.what()).contains("unknown config key"));
                throw;
            }
        },
        std::exception);
}

TEST(StartupConfig, MissingMarkerFailsInitialization)
{
    auto const workspace = mutable_module_fixture_workspace("startup_config_missing_marker", "local_cmake");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    EXPECT_THROW(
        {
            try {
                (void)startup_config.initialize();
            } catch (std::exception const &e) {
                EXPECT_TRUE(std::string(e.what()).contains(".intravenous"));
                throw;
            }
        },
        std::exception);
}

TEST(StartupConfig, ProjectConfigOverridesIntravenousDefaultsToolchain)
{
    auto const workspace = mutable_module_fixture_workspace("startup_config_toolchain_override", "local_cmake");
    auto const install_dir = workspace / "install";
    std::filesystem::create_directories(install_dir);
    std::filesystem::remove_all(
        iv::test::runtime_module_workspace_root("iv.test.local_cmake", workspace));

    iv::test_support::write_text(
        install_dir / ".intravenous_defaults",
        "c_compiler=/definitely/missing-clang\n"
        "cxx_compiler=/definitely/missing-clangxx\n");

    auto const c_compiler = configured_program_or_find("clang", IV_CONFIGURED_C_COMPILER);
    auto const cxx_compiler = configured_program_or_find("clang++", IV_CONFIGURED_CXX_COMPILER);
    ASSERT_FALSE(c_compiler.empty());
    ASSERT_FALSE(cxx_compiler.empty());

    iv::test_support::write_text(
        workspace / ".intravenous",
        "c_compiler=" + c_compiler + "\n"
        "cxx_compiler=" + cxx_compiler + "\n");

    ScopedEnvVar env("INTRAVENOUS_DIR", install_dir.string());
    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const initialized = startup_config.initialize();

    EXPECT_EQ(initialized.workspace_root, std::filesystem::weakly_canonical(workspace));
}

TEST(StartupConfig, ProjectConfigOverridesExecutionConfig)
{
    auto const workspace = mutable_module_fixture_workspace("startup_config_execution_override", "local_cmake");
    iv::test_support::write_text(
        workspace / ".intravenous",
        "block_size=512\n"
        "sample_rate=44100\n"
        "compiled_sample_cache_chunk_size_multiplier=8\n");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const initialized = startup_config.initialize();

    EXPECT_EQ(initialized.execution.block_size, 512u);
    EXPECT_EQ(initialized.execution.sample_rate, 44100u);
    EXPECT_EQ(initialized.execution.compiled_sample_cache_chunk_size_multiplier, 8u);
}
