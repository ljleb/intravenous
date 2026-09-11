#include "module_test_utils.h"

#include <gtest/gtest.h>

#include <algorithm>
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
        [&] { (void)loader.load_package_definitions(missing_dir); },
        "iv_package.json");
}

TEST(ModuleLoaderPackages, CanonicalPackageManifestLoads)
{
    auto const runtime_root = iv::test::runtime_modules_root()
        / "canonical_source_manifest";
    std::filesystem::remove_all(runtime_root);
    std::filesystem::create_directories(runtime_root);
    iv::test::write_text(runtime_root / "iv_project.jsonl", "");
    iv::test::write_text(
        runtime_root / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        runtime_root / "module.cpp",
        "#include <intravenous/dsl.h>\n"
        "void module_main(iv::GraphBuilder& g) { g.outputs(); }\n"
        "void module_secondary(iv::GraphBuilder& g) { g.outputs(); }\n"
        "IV_MODULE(\"iv.test.canonical_source\", module_main);\n"
        "IV_MODULE(\"iv.test.canonical_source.secondary\", module_secondary);\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_package_definitions(runtime_root);

    ASSERT_EQ(loaded.size(), 2);
    auto const primary = std::ranges::find(
        loaded, "iv.test.canonical_source", &iv::ModuleLoader::LoadedDefinition::module_id);
    auto const secondary = std::ranges::find(
        loaded,
        "iv.test.canonical_source.secondary",
        &iv::ModuleLoader::LoadedDefinition::module_id);
    ASSERT_NE(primary, loaded.end());
    ASSERT_NE(secondary, loaded.end());
    EXPECT_TRUE(static_cast<bool>(primary->root));
    EXPECT_TRUE(static_cast<bool>(secondary->root));
}

TEST(ModuleLoaderPackages, RootPackageDoesNotPublishOtherPackageDefinitions)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader();

    auto loaded = loader.load_package_definitions(fixtures / "nested_loader_project");

    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().module_id, "iv.test.nested_loader_project");
    EXPECT_TRUE(static_cast<bool>(loaded.front().root));
}

TEST(ModuleLoaderPackages, RegisteredPackageNodeIsResolvedFromLoadedPackageDefinitions)
{
    auto const project_root = iv::test::runtime_modules_root()
        / "registered_source_node";
    auto const node_source = project_root / "modules" / "registered_node";
    auto const consumer_source = project_root / "modules" / "consumer";
    std::filesystem::remove_all(project_root);
    std::filesystem::create_directories(node_source);
    std::filesystem::create_directories(consumer_source);
    iv::test::write_text(project_root / "iv_project.jsonl", "");
    iv::test::write_text(
        node_source / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        node_source / "module.cpp",
        "#include <intravenous/dsl.h>\n"
        "#include <array>\n\n"
        "namespace {\n"
        "struct RegisteredPackageNode {\n"
        "    static constexpr auto outputs()\n"
        "    {\n"
        "        return std::array<iv::OutputConfig, 1>{};\n"
        "    }\n\n"
        "    void tick(iv::TickSampleContext<RegisteredPackageNode> const& ctx) const\n"
        "    {\n"
        "        ctx.outputs[0].push(0.25);\n"
        "    }\n"
        "};\n"
        "}\n\n"
        "IV_NODE(\"iv.test.registered_source_node\", RegisteredPackageNode);\n");
    iv::test::write_text(
        consumer_source / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        consumer_source / "module.cpp",
        "#include <intravenous/dsl.h>\n\n"
        "void registered_node_consumer(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.registered_source_node\">());\n"
        "}\n\n"
        "IV_MODULE(\"iv.test.registered_node_consumer\", registered_node_consumer);\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_package_definitions(consumer_source);

    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded.front().module_id, "iv.test.registered_node_consumer");
    EXPECT_TRUE(static_cast<bool>(loaded.front().root));

}

TEST(ModuleLoaderFailures, SourceWithoutManifestFails)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_package_definitions(fixtures / "missing_export"); },
        "iv_package.json");
}

TEST(ModuleLoaderFailures, BuildFailurePropagates)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_package_definitions(fixtures / "build_failure"); },
        "command failed");
}

TEST(ModuleLoaderFailures, MissingDependencyFails)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_package_definitions(fixtures / "missing_dependency"); },
        "is unavailable in the loaded package definitions");
}

TEST(ModuleLoaderFailures, UnrelatedDuplicateSourceIdsDoNotBlockLoading)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader({fixtures, iv::test::duplicate_modules_root()});
    auto const definitions = loader.load_package_definitions(
        fixtures / "nested_loader_project");
    EXPECT_EQ(definitions.size(), 1u);
    EXPECT_EQ(definitions.front().module_id, "iv.test.nested_loader_project");
}
