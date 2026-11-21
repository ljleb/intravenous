#include "../module_test_utils.h"

#include <intravenous/runtime/package_jit.h>
#include <intravenous/runtime/startup_config.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace {
iv::IvPackageDeclaration declaration(
    std::string package_id,
    std::filesystem::path const& package_root,
    iv::PackageProvenance provenance = iv::PackageProvenance::project_local,
    std::optional<std::filesystem::path> artifact_root = std::nullopt)
{
    return iv::IvPackageDeclaration{
        .package_id = std::move(package_id),
        .package_root = std::filesystem::weakly_canonical(package_root),
        .provenance = provenance,
        .artifact_root = std::move(artifact_root),
    };
}
} // namespace

TEST(PackageJit, MixedBatchIsolatesFailuresAndSuccessfulRevisionsAdvanceMonotonically)
{
    auto const good_root = iv::test_support::make_inline_module_workspace(
        "package_jit_batch_good",
        R"cpp(
#include <intravenous/dsl.h>

void package_jit_batch_good(iv::GraphBuilder& g)
{
    g.outputs();
}
)cpp");
    auto const bad_root = iv::test_support::read_only_module_fixture_workspace("build_failure");
    auto const good = declaration(
        "iv.test.jit.good",
        good_root,
        iv::PackageProvenance::common,
        good_root.parent_path() / "package-jit-common-cache");
    auto const bad = declaration("iv.test.jit.bad", bad_root);

    iv::StartupConfig startup_config(good_root, iv::test::repo_root(), {});
    iv::PackageJit jit(startup_config.initialize());

    iv::PackageJitBatchRequest first_request{
        .declarations = {good, bad},
    };
    jit.handle_build_request(first_request);

    ASSERT_EQ(first_request.result.revisions.size(), 1u);
    ASSERT_EQ(first_request.result.failed.size(), 1u);
    auto const& first_revision = first_request.result.revisions.front();
    EXPECT_EQ(first_revision.package_id, good.package_id);
    EXPECT_EQ(first_revision.package_root, good.package_root);
    EXPECT_EQ(first_revision.provenance, iv::PackageProvenance::common);
    EXPECT_EQ(first_revision.revision, 1u);
    EXPECT_TRUE(std::filesystem::is_regular_file(
        first_revision.compiler_artifact.bitcode_path));
    EXPECT_EQ(
        first_revision.compiler_artifact.bitcode_path.extension(),
        ".bc");
    EXPECT_TRUE(static_cast<bool>(first_revision.package_code));
    ASSERT_EQ(first_revision.module_definitions.size(), 1u);
    EXPECT_EQ(
        first_revision.module_definitions.front().definition_id,
        "iv.test.package_jit_batch_good");

    EXPECT_EQ(first_request.result.failed.front().package_id, bad.package_id);
    EXPECT_EQ(first_request.result.failed.front().package_root, bad.package_root);
    EXPECT_EQ(
        first_request.result.failed.front().provenance,
        iv::PackageProvenance::project_local);
    EXPECT_FALSE(first_request.result.failed.front().message.empty());

    iv::PackageJitBatchRequest second_request{
        .declarations = {good},
    };
    jit.handle_build_request(second_request);

    ASSERT_EQ(second_request.result.revisions.size(), 1u);
    EXPECT_TRUE(second_request.result.failed.empty());
    EXPECT_EQ(second_request.result.revisions.front().package_id, good.package_id);
    EXPECT_EQ(second_request.result.revisions.front().revision, 2u);
    EXPECT_EQ(
        second_request.result.revisions.front().compiler_artifact.bitcode_path,
        first_revision.compiler_artifact.bitcode_path);
    EXPECT_TRUE(static_cast<bool>(second_request.result.revisions.front().package_code));
}

TEST(PackageJit, ToolchainConfigurationRoundTripsWithoutForcingABuild)
{
    auto const project = iv::test_support::fresh_module_fixture_workspace(
        "package_jit_toolchain_round_trip");
    iv::StartupConfig startup_config(project, iv::test::repo_root(), {});
    iv::PackageJit jit(startup_config.initialize());

    auto toolchain = jit.toolchain_config();
    toolchain.cmake_generator = "Synthetic Generator";
    toolchain.make_program = project / "synthetic-ninja";
    jit.set_toolchain_config(toolchain);

    auto const round_trip = jit.toolchain_config();
    EXPECT_EQ(round_trip.cmake_generator, toolchain.cmake_generator);
    EXPECT_EQ(round_trip.make_program, toolchain.make_program);
}

TEST(PackageJit, ClearingToolchainOverridesRestoresInstallationDefaults)
{
    iv::StartupConfigState startup;
    startup.toolchain.c_compiler = "/installation/toolchain/clang";
    startup.toolchain.cxx_compiler = "/installation/toolchain/clang++";
    startup.toolchain.cmake_program = "/installation/bin/cmake";
    startup.toolchain.cmake_generator = "Ninja";
    startup.toolchain.make_program = "/installation/bin/ninja";
    startup.toolchain.juce_dir = "/installation/juce";
    startup.toolchain.iv_package_pch = "/installation/iv_dsl.pch";
    auto const defaults = startup.toolchain;
    iv::PackageJit jit(std::move(startup));
    auto changed = defaults;
    changed.cxx_compiler = "/project/toolchain/clang++";
    changed.cmake_generator = "Unix Makefiles";
    jit.set_toolchain_config(changed);
    EXPECT_EQ(jit.toolchain_config().cxx_compiler, changed.cxx_compiler);
    jit.set_toolchain_config({});
    auto restored = jit.toolchain_config();
    EXPECT_EQ(restored.c_compiler, defaults.c_compiler);
    EXPECT_EQ(restored.cxx_compiler, defaults.cxx_compiler);
    EXPECT_EQ(restored.cmake_program, defaults.cmake_program);
    EXPECT_EQ(restored.cmake_generator, defaults.cmake_generator);
    EXPECT_EQ(restored.make_program, defaults.make_program);
    EXPECT_EQ(restored.juce_dir, defaults.juce_dir);
    EXPECT_EQ(restored.iv_package_pch, defaults.iv_package_pch);
}
