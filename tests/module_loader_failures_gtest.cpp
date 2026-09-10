#include "module_test_utils.h"

#include <gtest/gtest.h>
#include <string_view>

namespace {
template<typename Fn>
void expect_failure_contains(Fn&& fn, std::string_view needle)
{
    try {
        fn();
    } catch (std::exception const& e) {
        EXPECT_NE(std::string_view(e.what()).find(needle), std::string_view::npos)
            << e.what();
        return;
    }
    FAIL() << "expected exception containing: " << needle;
}
}

TEST(ModuleLoaderFailures, MissingManifestFails)
{
    auto const runtime_root = iv::test::runtime_modules_root();
    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);
    auto loader = iv::test::make_loader();
    auto missing_dir = runtime_root / "missing_entry";
    std::filesystem::create_directories(missing_dir);
    expect_failure_contains(
        [&] { (void)loader.load_root_definition(missing_dir); },
        "iv_source.json");
}

TEST(ModuleLoaderSources, CanonicalSourceManifestLoads)
{
    auto const runtime_root = iv::test::runtime_modules_root()
        / "canonical_source_manifest";
    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);
    iv::test::write_text(runtime_root / "iv_project.jsonl", "");
    iv::test::write_text(
        runtime_root / "iv_source.json",
        "{\"schema\":1,\"id\":\"iv.test.canonical_source\","
        "\"entry\":\"module.cpp\",\"main\":\"module_main\"}\n");
    iv::test::write_text(
        runtime_root / "module.cpp",
        "#include <intravenous/dsl.h>\n"
        "void module_main(iv::GraphBuilder& g) { g.outputs(); }\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_root_definition(runtime_root);

    EXPECT_EQ(loaded.module_id, "iv.test.canonical_source");
    EXPECT_TRUE(static_cast<bool>(loaded.root));
}

TEST(ModuleLoaderFailures, SourceWithoutManifestFails)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_root_definition(fixtures / "missing_export"); },
        "iv_source.json");
}

TEST(ModuleLoaderFailures, BuildFailurePropagates)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_root_definition(fixtures / "build_failure"); },
        "command failed");
}

TEST(ModuleLoaderFailures, MissingDependencyFails)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_root_definition(fixtures / "missing_dependency"); },
        "imports missing");
}

TEST(ModuleLoaderFailures, DuplicateSourceIdFails)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader({fixtures, iv::test::duplicate_modules_root()});
    expect_failure_contains(
        [&] { (void)loader.load_root_definition(fixtures / "nested_loader_project"); },
        "duplicate IV source id");
}
