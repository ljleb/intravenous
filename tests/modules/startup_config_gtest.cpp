#include "../module_test_utils.h"

#include <intravenous/runtime/startup_config.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>

namespace {
using iv::test_support::ScopedEnvVar;
using iv::test_support::configured_program_or_find;
using iv::test_support::mutable_module_fixture_workspace;
using Json = nlohmann::ordered_json;
}

TEST(StartupConfig, MissingProjectFileUsesBuiltinDefaults)
{
    auto const workspace = mutable_module_fixture_workspace("startup_config_missing_project_file", "local_cmake");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const initialized = startup_config.initialize();

    EXPECT_EQ(initialized.workspace_root, std::filesystem::weakly_canonical(workspace));
    EXPECT_EQ(initialized.execution.block_size, 256u);
    EXPECT_EQ(initialized.execution.sample_rate, 48000u);
    EXPECT_EQ(initialized.output_device_id, std::optional<std::string>("default"));
    EXPECT_EQ(initialized.input_device_id, std::optional<std::string>("default"));
}

TEST(StartupConfig, ProjectFileDoesNotAffectStartupDefaults)
{
    auto const workspace = mutable_module_fixture_workspace("startup_config_project_file_ignored", "local_cmake");
    iv::test_support::write_text(
        workspace / "iv_project.jsonl",
        Json{
            {"command", "project.overrideSettings"},
            {"args", Json{
                {"c_compiler", "/tmp/not-used"},
                {"block_size", 512},
                {"sample_rate", 44100},
            }},
        }.dump() + "\n");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const initialized = startup_config.initialize();

    EXPECT_EQ(initialized.execution.block_size, 256u);
    EXPECT_EQ(initialized.execution.sample_rate, 48000u);
    EXPECT_FALSE(initialized.toolchain.c_compiler.has_value());
}

TEST(StartupConfig, IntravenousDefaultsOverrideToolchain)
{
    auto const workspace = mutable_module_fixture_workspace("startup_config_toolchain_override", "local_cmake");
    auto const install_dir = workspace / "install";
    std::filesystem::create_directories(install_dir);
    std::filesystem::remove_all(
        iv::test::runtime_module_workspace_root(workspace));

    auto const c_compiler = configured_program_or_find("clang", IV_CONFIGURED_C_COMPILER);
    auto const cxx_compiler = configured_program_or_find("clang++", IV_CONFIGURED_CXX_COMPILER);
    ASSERT_FALSE(c_compiler.empty());
    ASSERT_FALSE(cxx_compiler.empty());

    iv::test_support::write_text(
        install_dir / ".intravenous_defaults",
        "c_compiler=" + c_compiler + "\n"
        "cxx_compiler=" + cxx_compiler + "\n");

    ScopedEnvVar env("INTRAVENOUS_DIR", install_dir.string());
    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const initialized = startup_config.initialize();

    ASSERT_TRUE(initialized.toolchain.c_compiler.has_value());
    ASSERT_TRUE(initialized.toolchain.cxx_compiler.has_value());
    EXPECT_EQ(initialized.toolchain.c_compiler->string(), c_compiler);
    EXPECT_EQ(initialized.toolchain.cxx_compiler->string(), cxx_compiler);
}

TEST(StartupConfig, IntravenousDefaultsOverrideExecutionConfig)
{
    auto const workspace = mutable_module_fixture_workspace("startup_config_execution_override", "local_cmake");
    auto const install_dir = workspace / "install";
    std::filesystem::create_directories(install_dir);

    iv::test_support::write_text(
        install_dir / ".intravenous_defaults",
        "block_size=512\n"
        "sample_rate=44100\n"
        "output_device_id=out-1\n"
        "input_device_id=\n");

    ScopedEnvVar env("INTRAVENOUS_DIR", install_dir.string());
    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const initialized = startup_config.initialize();

    EXPECT_EQ(initialized.execution.block_size, 512u);
    EXPECT_EQ(initialized.execution.sample_rate, 44100u);
    EXPECT_EQ(initialized.output_device_id, std::optional<std::string>("out-1"));
    EXPECT_EQ(initialized.input_device_id, std::nullopt);
}

TEST(StartupConfig, IntravenousDefaultsAddOrdinaryPackageSearchRoots)
{
    auto const workspace = mutable_module_fixture_workspace(
        "startup_config_default_package_roots", "local_cmake");
    auto const install_dir = workspace / "install";
    auto const first_package_root = install_dir / "packages-a";
    auto const second_package_root = install_dir / "packages-b";
    std::filesystem::create_directories(install_dir);

    iv::test_support::write_text(
        install_dir / ".intravenous_defaults",
        "iv_package_search_root=packages-a\n"
        "iv_package_search_root=" + second_package_root.string() + "\n");

    ScopedEnvVar env("INTRAVENOUS_DIR", install_dir.string());
    ScopedEnvVar module_search_path("IV_MODULE_SEARCH_PATH", "");
    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const initialized = startup_config.initialize();

    ASSERT_EQ(initialized.search_roots.size(), 2u);
    EXPECT_EQ(initialized.search_roots[0], std::filesystem::weakly_canonical(first_package_root));
    EXPECT_EQ(initialized.search_roots[1], std::filesystem::weakly_canonical(second_package_root));
}
