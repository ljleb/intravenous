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
        [&] { (void)loader.load_source_definitions(missing_dir); },
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
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        runtime_root / "module.cpp",
        "#include <intravenous/dsl.h>\n"
        "void module_main(iv::GraphBuilder& g) { g.outputs(); }\n"
        "void module_secondary(iv::GraphBuilder& g) { g.outputs(); }\n"
        "IV_MODULE(\"iv.test.canonical_source\", module_main);\n"
        "IV_MODULE(\"iv.test.canonical_source.secondary\", module_secondary);\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_source_definitions(runtime_root);

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

TEST(ModuleLoaderSources, RegisteredSourceNodeIsImportedThroughDefinitionHeader)
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
        node_source / "iv_source.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        node_source / "module.cpp",
        "#include <intravenous/dsl.h>\n"
        "#include <array>\n\n"
        "namespace {\n"
        "struct RegisteredSourceNode {\n"
        "    static constexpr auto outputs()\n"
        "    {\n"
        "        return std::array<iv::OutputConfig, 1>{};\n"
        "    }\n\n"
        "    void tick(iv::TickSampleContext<RegisteredSourceNode> const& ctx) const\n"
        "    {\n"
        "        ctx.outputs[0].push(0.25);\n"
        "    }\n"
        "};\n"
        "}\n\n"
        "IV_NODE(\"iv.test.registered_source_node\", RegisteredSourceNode);\n");
    iv::test::write_text(
        consumer_source / "iv_source.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        consumer_source / "module.cpp",
        "#include <intravenous/dsl.h>\n"
        "#include <iv/nodes/iv.test.registered_source_node>\n\n"
        "void registered_node_consumer(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.registered_source_node\">());\n"
        "}\n\n"
        "IV_MODULE(\"iv.test.registered_node_consumer\", registered_node_consumer);\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_source_definitions(consumer_source);

    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded.front().module_id, "iv.test.registered_node_consumer");
    EXPECT_TRUE(static_cast<bool>(loaded.front().root));

    auto const generated_header = project_root / "build" / "iv" / "imports"
        / "iv" / "nodes" / "iv.test.registered_source_node";
    ASSERT_TRUE(std::filesystem::exists(generated_header));
    EXPECT_NE(
        iv::test::read_text(generated_header).find(
            "IV_NODE_INTERFACE(\"iv.test.registered_source_node\")"),
        std::string::npos);
}

TEST(ModuleLoaderFailures, SourceWithoutManifestFails)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_source_definitions(fixtures / "missing_export"); },
        "iv_source.json");
}

TEST(ModuleLoaderFailures, BuildFailurePropagates)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_source_definitions(fixtures / "build_failure"); },
        "command failed");
}

TEST(ModuleLoaderFailures, MissingDependencyFails)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_source_definitions(fixtures / "missing_dependency"); },
        "imports missing");
}

TEST(ModuleLoaderFailures, DuplicateSourceIdFails)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader({fixtures, iv::test::duplicate_modules_root()});
    expect_failure_contains(
        [&] { (void)loader.load_source_definitions(fixtures / "nested_loader_project"); },
        "duplicate stable IV definition ID");
}
