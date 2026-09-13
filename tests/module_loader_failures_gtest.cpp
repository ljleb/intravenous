#include "module_test_utils.h"

#include <intravenous/graph/configured_graph.hpp>

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
        / "canonical_package_manifest";
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

TEST(ModuleLoaderPackages, ScalarSourceOutputResolvesShippedBuiltinConstant)
{
    auto const package_root = iv::test::runtime_modules_root()
        / "scalar_output_uses_builtin_constant";
    std::filesystem::remove_all(package_root);
    std::filesystem::create_directories(package_root);
    iv::test::write_text(package_root / "iv_project.jsonl", "");
    iv::test::write_text(
        package_root / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        package_root / "module.cpp",
        "#include <intravenous/dsl.h>\n\n"
        "void scalar_output(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(0.25f);\n"
        "}\n\n"
        "IV_MODULE(\"iv.test.scalar_output\", scalar_output);\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_package_definitions(package_root);
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().module_id, "iv.test.scalar_output");
    EXPECT_TRUE(static_cast<bool>(loaded.front().root));
}

TEST(ModuleLoaderPackages, BuiltinNodeShortIdCanConstructATiledRegisteredNode)
{
    auto const package_root = iv::test::runtime_modules_root()
        / "source_uses_shipped_builtin_node";
    std::filesystem::remove_all(package_root);
    std::filesystem::create_directories(package_root);
    iv::test::write_text(package_root / "iv_project.jsonl", "");
    iv::test::write_text(
        package_root / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        package_root / "module.cpp",
        "#include <intravenous/dsl.h>\n\n"
        "void builtin_oscillator(iv::GraphBuilder& g)\n"
        "{\n"
        "    auto oscillator = g.node<\"saw_oscillator\", iv::stereo>();\n"
        "    oscillator.connect_input(\"frequency\", 440.0f);\n"
        "    g.outputs(oscillator);\n"
        "}\n\n"
        "IV_MODULE(\"iv.test.builtin_oscillator\", builtin_oscillator);\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_package_definitions(package_root);

    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().module_id, "iv.test.builtin_oscillator");
    EXPECT_TRUE(static_cast<bool>(loaded.front().root));
}

TEST(ModuleLoaderPackages, ExactShortRegistrationOverridesBuiltinConvenienceAlias)
{
    auto const package_root = iv::test::runtime_modules_root()
        / "exact_short_registration_overrides_builtin_alias";
    std::filesystem::remove_all(package_root);
    std::filesystem::create_directories(package_root);
    iv::test::write_text(package_root / "iv_project.jsonl", "");
    iv::test::write_text(
        package_root / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        package_root / "module.cpp",
        "#include <intravenous/dsl.h>\n"
        "#include <array>\n\n"
        "struct LocalShortNameOscillator {\n"
        "    iv::Sample value;\n"
        "    explicit LocalShortNameOscillator(iv::Sample value_) : value(value_) {}\n"
        "    static constexpr auto outputs()\n"
        "    { return std::array<iv::OutputConfig, 1>{}; }\n"
        "    void tick(iv::TickSampleContext<LocalShortNameOscillator> const& ctx) const\n"
        "    { ctx.outputs[0].push(value); }\n"
        "};\n\n"
        "void local_short_name_consumer(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"saw_oscillator\">(iv::Sample{0.25f}));\n"
        "}\n\n"
        "IV_NODE(\"saw_oscillator\", LocalShortNameOscillator);\n"
        "IV_MODULE(\"iv.test.local_short_name_consumer\", local_short_name_consumer);\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_package_definitions(package_root);

    // The shipped saw oscillator does not accept this construction argument.
    // Reaching a valid root proves the exact local ID won before the bare-ID
    // builtin alias was considered.
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().module_id, "iv.test.local_short_name_consumer");
    EXPECT_TRUE(static_cast<bool>(loaded.front().root));
}

TEST(ModuleLoaderPackages, RegisteredModuleCanConstructATiledMonoInterface)
{
    auto const package_root = iv::test::runtime_modules_root()
        / "tiled_registered_module";
    std::filesystem::remove_all(package_root);
    std::filesystem::create_directories(package_root);
    iv::test::write_text(package_root / "iv_project.jsonl", "");
    iv::test::write_text(
        package_root / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        package_root / "module.cpp",
        "#include <intravenous/dsl.h>\n\n"
        "void mono_voice(iv::GraphBuilder& g)\n"
        "{\n"
        "    auto frequency = g.input<\"frequency\">(220.0f);\n"
        "    auto oscillator = g.node<\"saw_oscillator\">();\n"
        "    oscillator.connect_input(\"frequency\", frequency);\n"
        "    g.outputs(oscillator);\n"
        "}\n\n"
        "void stereo_voice(iv::GraphBuilder& g)\n"
        "{\n"
        "    auto frequency = g.input<\"frequency\", iv::stereo>(440.0f);\n"
        "    auto voice = g.node<\"iv.test.tiled_mono_voice\", iv::stereo>();\n"
        "    voice.connect_input(\"frequency\", frequency);\n"
        "    g.outputs(voice);\n"
        "}\n\n"
        "IV_MODULE(\"iv.test.tiled_mono_voice\", mono_voice);\n"
        "IV_MODULE(\"iv.test.tiled_stereo_voice\", stereo_voice);\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_package_definitions(package_root);

    ASSERT_EQ(loaded.size(), 2u);
    auto const stereo = std::ranges::find(
        loaded,
        "iv.test.tiled_stereo_voice",
        &iv::ModuleLoader::LoadedDefinition::module_id);
    ASSERT_NE(stereo, loaded.end());
    EXPECT_TRUE(static_cast<bool>(stereo->root));
    ASSERT_NE(stereo->configured_graph, nullptr);
    auto const inputs = stereo->configured_graph->public_ports.sample_inputs(
        stereo->configured_graph->node_bundles);
    auto const outputs = stereo->configured_graph->public_ports.sample_outputs(
        stereo->configured_graph->node_bundles);
    ASSERT_EQ(inputs.size(), 1u);
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(inputs.front().channel_layout.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(outputs.front().channel_layout.channel_type, iv::ChannelTypeId::stereo);
}

TEST(ModuleLoaderFailures, TiledRegisteredModuleRequiresMonoSampleInterface)
{
    auto const package_root = iv::test::runtime_modules_root()
        / "tiled_registered_module_nonmono_interface";
    std::filesystem::remove_all(package_root);
    std::filesystem::create_directories(package_root);
    iv::test::write_text(package_root / "iv_project.jsonl", "");
    iv::test::write_text(
        package_root / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        package_root / "module.cpp",
        "#include <intravenous/dsl.h>\n\n"
        "void stereo_provider(iv::GraphBuilder& g)\n"
        "{\n"
        "    auto input = g.input<\"input\", iv::stereo>();\n"
        "    g.outputs(input);\n"
        "}\n\n"
        "void invalid_tiled_use(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.stereo_provider\", iv::stereo>());\n"
        "}\n\n"
        "IV_MODULE(\"iv.test.stereo_provider\", stereo_provider);\n"
        "IV_MODULE(\"iv.test.invalid_tiled_use\", invalid_tiled_use);\n");

    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_package_definitions(package_root); },
        "tiled IV module members must expose only mono sample inputs");
}

TEST(ModuleLoaderFailures, TiledRegisteredModuleRequiresMonoSampleOutputs)
{
    auto const package_root = iv::test::runtime_modules_root()
        / "tiled_registered_module_nonmono_output";
    std::filesystem::remove_all(package_root);
    std::filesystem::create_directories(package_root);
    iv::test::write_text(package_root / "iv_project.jsonl", "");
    iv::test::write_text(
        package_root / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        package_root / "module.cpp",
        "#include <intravenous/dsl.h>\n\n"
        "void stereo_output_provider(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"saw_oscillator\", iv::stereo>());\n"
        "}\n\n"
        "void invalid_tiled_use(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.stereo_output_provider\", iv::stereo>());\n"
        "}\n\n"
        "IV_MODULE(\"iv.test.stereo_output_provider\", stereo_output_provider);\n"
        "IV_MODULE(\"iv.test.invalid_tiled_output_use\", invalid_tiled_use);\n");

    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_package_definitions(package_root); },
        "tiled IV module members must expose only mono sample outputs");
}

TEST(ModuleLoaderPackages, RejectedTiledModuleLeavesNoPartialSubgraphs)
{
    auto const package_root = iv::test::runtime_modules_root()
        / "rejected_tiled_module_has_no_partial_graph_state";
    std::filesystem::remove_all(package_root);
    std::filesystem::create_directories(package_root);
    iv::test::write_text(package_root / "iv_project.jsonl", "");
    iv::test::write_text(
        package_root / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        package_root / "module.cpp",
        "#include <intravenous/dsl.h>\n\n"
        "void incompatible_provider(iv::GraphBuilder& g)\n"
        "{\n"
        "    auto input = g.input<\"input\", iv::stereo>();\n"
        "    g.outputs(input);\n"
        "}\n\n"
        "void catches_rejected_tile(iv::GraphBuilder& g)\n"
        "{\n"
        "    try {\n"
        "        (void)g.node<\"iv.test.incompatible_provider\", iv::stereo>();\n"
        "    } catch (...) {\n"
        "    }\n"
        "    g.outputs(0.25f);\n"
        "}\n\n"
        "IV_MODULE(\"iv.test.incompatible_provider\", incompatible_provider);\n"
        "IV_MODULE(\"iv.test.catches_rejected_tile\", catches_rejected_tile);\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_package_definitions(package_root);
    auto const fallback = std::ranges::find(
        loaded,
        "iv.test.catches_rejected_tile",
        &iv::ModuleLoader::LoadedDefinition::module_id);
    ASSERT_NE(fallback, loaded.end());
    ASSERT_NE(fallback->configured_graph, nullptr);

    std::size_t subgraphs = 0;
    fallback->configured_graph->node_bundles.for_each_configured_bundle(
        [&](iv::ConfiguredNodeBundleView const& bundle) {
            if (bundle.kind == iv::ConfiguredNodeBundleKind::subgraph) ++subgraphs;
        });
    EXPECT_EQ(subgraphs, 0u);
}

TEST(ModuleLoaderPackages, RootPackageDoesNotPublishOtherPackageDefinitions)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader(
        {fixtures / "nested_loader_project", fixtures / "nested_loader_voice"});

    auto loaded = loader.load_package_definitions(fixtures / "nested_loader_project");

    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().module_id, "iv.test.nested_loader_project");
    EXPECT_TRUE(static_cast<bool>(loaded.front().root));
}

TEST(ModuleLoaderPackages, RegisteredPackageNodeIsResolvedFromLoadedPackageDefinitions)
{
    auto const project_root = iv::test::runtime_modules_root()
        / "registered_package_node";
    auto const node_package = project_root / "modules" / "registered_node";
    auto const consumer_package = project_root / "modules" / "consumer";
    std::filesystem::remove_all(project_root);
    std::filesystem::create_directories(node_package);
    std::filesystem::create_directories(consumer_package);
    iv::test::write_text(project_root / "iv_project.jsonl", "");
    iv::test::write_text(
        node_package / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        node_package / "module.cpp",
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
        "IV_NODE(\"iv.test.registered_package_node\", RegisteredPackageNode);\n");
    iv::test::write_text(
        consumer_package / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        consumer_package / "module.cpp",
        "#include <intravenous/dsl.h>\n\n"
        "void registered_node_consumer(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.registered_package_node\">());\n"
        "}\n\n"
        "IV_MODULE(\"iv.test.registered_node_consumer\", registered_node_consumer);\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_package_definitions(consumer_package);

    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded.front().module_id, "iv.test.registered_node_consumer");
    EXPECT_TRUE(static_cast<bool>(loaded.front().root));

}

TEST(ModuleLoaderPackages, RegisteredNodeAndModuleUseProviderConstructionArguments)
{
    auto const project_root = iv::test::runtime_modules_root()
        / "registered_configuration_arguments";
    auto const provider_package = project_root / "modules" / "provider";
    auto const consumer_package = project_root / "modules" / "consumer";
    std::filesystem::remove_all(project_root);
    std::filesystem::create_directories(provider_package);
    std::filesystem::create_directories(consumer_package);
    iv::test::write_text(project_root / "iv_project.jsonl", "");
    iv::test::write_text(
        provider_package / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        provider_package / "module.cpp",
        "#include <intravenous/dsl.h>\n"
        "#include <array>\n\n"
        "namespace {\n"
        "struct ConfiguredProviderNode {\n"
        "    iv::Sample gain;\n"
        "    constexpr explicit ConfiguredProviderNode(iv::Sample gain_ = iv::Sample{0.25f})\n"
        "        : gain(gain_) {}\n"
        "    static constexpr auto outputs()\n"
        "    { return std::array<iv::OutputConfig, 1>{}; }\n"
        "    void tick(iv::TickSampleContext<ConfiguredProviderNode> const& ctx) const\n"
        "    { ctx.outputs[0].push(gain); }\n"
        "};\n"
        "struct RequiredConfiguredProviderNode {\n"
        "    iv::Sample gain;\n"
        "    constexpr explicit RequiredConfiguredProviderNode(iv::Sample gain_)\n"
        "        : gain(gain_) {}\n"
        "    static constexpr auto outputs()\n"
        "    { return std::array<iv::OutputConfig, 1>{}; }\n"
        "    void tick(iv::TickSampleContext<RequiredConfiguredProviderNode> const& ctx) const\n"
        "    { ctx.outputs[0].push(gain); }\n"
        "};\n"
        "struct LabelConfiguredProviderNode {\n"
        "    char const* label;\n"
        "    constexpr explicit LabelConfiguredProviderNode(char const* label_)\n"
        "        : label(label_) {}\n"
        "    static constexpr auto outputs()\n"
        "    { return std::array<iv::OutputConfig, 1>{}; }\n"
        "    void tick(iv::TickSampleContext<LabelConfiguredProviderNode> const& ctx) const\n"
        "    { ctx.outputs[0].push(iv::Sample{}); }\n"
        "};\n"
        "}\n\n"
        "IV_NODE(\"iv.test.configured_provider_node\", ConfiguredProviderNode);\n\n"
        "IV_NODE(\"iv.test.required_configured_provider_node\", "
        "RequiredConfiguredProviderNode);\n\n"
        "IV_NODE(\"iv.test.label_configured_provider_node\", "
        "LabelConfiguredProviderNode);\n\n"
        "void configured_provider_module(iv::GraphBuilder& g,\n"
        "    iv::Sample gain = iv::Sample{0.5f})\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.configured_provider_node\">(gain));\n"
        "}\n"
        "IV_MODULE(\"iv.test.configured_provider_module\", configured_provider_module);\n"
        "void required_provider_module(iv::GraphBuilder& g, iv::Sample gain)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.configured_provider_node\">(gain));\n"
        "}\n"
        "IV_MODULE(\"iv.test.required_provider_module\", required_provider_module);\n");
    iv::test::write_text(
        consumer_package / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        consumer_package / "module.cpp",
        "#include <intravenous/dsl.h>\n\n"
        "void direct_configured_node(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.configured_provider_node\">(iv::Sample{0.75f}));\n"
        "}\n"
        "void configured_module_reference(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.configured_provider_module\">(iv::Sample{0.875f}));\n"
        "}\n"
        "void required_configured_node(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.required_configured_provider_node\">(iv::Sample{0.625f}));\n"
        "}\n"
        "void required_configured_module(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.required_provider_module\">(iv::Sample{0.375f}));\n"
        "}\n"
        "void string_literal_configuration(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.label_configured_provider_node\">(\"literal label\"));\n"
        "}\n"
        "void default_configured_module_reference(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.configured_provider_module\">());\n"
        "}\n"
        "IV_MODULE(\"iv.test.direct_configured_node\", direct_configured_node);\n"
        "IV_MODULE(\"iv.test.configured_module_reference\", configured_module_reference);\n"
        "IV_MODULE(\"iv.test.required_configured_node\", required_configured_node);\n"
        "IV_MODULE(\"iv.test.required_configured_module\", required_configured_module);\n"
        "IV_MODULE(\"iv.test.string_literal_configuration\", string_literal_configuration);\n"
        "IV_MODULE(\"iv.test.default_configured_module_reference\", "
        "default_configured_module_reference);\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_package_definitions(consumer_package);

    ASSERT_EQ(loaded.size(), 6u);
    EXPECT_NE(std::ranges::find(
        loaded, "iv.test.direct_configured_node", &iv::ModuleLoader::LoadedDefinition::module_id),
        loaded.end());
    EXPECT_NE(std::ranges::find(
        loaded,
        "iv.test.configured_module_reference",
        &iv::ModuleLoader::LoadedDefinition::module_id),
        loaded.end());
    EXPECT_NE(std::ranges::find(
        loaded,
        "iv.test.required_configured_node",
        &iv::ModuleLoader::LoadedDefinition::module_id),
        loaded.end());
    EXPECT_NE(std::ranges::find(
        loaded,
        "iv.test.required_configured_module",
        &iv::ModuleLoader::LoadedDefinition::module_id),
        loaded.end());
    EXPECT_NE(std::ranges::find(
        loaded,
        "iv.test.string_literal_configuration",
        &iv::ModuleLoader::LoadedDefinition::module_id),
        loaded.end());
    EXPECT_NE(std::ranges::find(
        loaded,
        "iv.test.default_configured_module_reference",
        &iv::ModuleLoader::LoadedDefinition::module_id),
        loaded.end());

    auto provider_loaded = loader.load_package_definitions(provider_package);
    ASSERT_EQ(provider_loaded.size(), 2u);
    auto const required_provider = std::ranges::find(
        provider_loaded,
        "iv.test.required_provider_module",
        &iv::ModuleLoader::LoadedDefinition::module_id);
    ASSERT_NE(required_provider, provider_loaded.end());
    EXPECT_FALSE(static_cast<bool>(required_provider->root));
}

TEST(ModuleLoaderFailures, RegisteredConfigurationRejectsImplicitConversions)
{
    auto const project_root = iv::test::runtime_modules_root()
        / "registered_configuration_type_mismatch";
    auto const provider_package = project_root / "modules" / "provider";
    auto const consumer_package = project_root / "modules" / "consumer";
    std::filesystem::remove_all(project_root);
    std::filesystem::create_directories(provider_package);
    std::filesystem::create_directories(consumer_package);
    iv::test::write_text(project_root / "iv_project.jsonl", "");
    iv::test::write_text(
        provider_package / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        provider_package / "module.cpp",
        "#include <intravenous/dsl.h>\n"
        "#include <array>\n\n"
        "namespace {\n"
        "struct SampleConfiguredNode {\n"
        "    constexpr explicit SampleConfiguredNode(iv::Sample = iv::Sample{}) {}\n"
        "    static constexpr auto outputs()\n"
        "    { return std::array<iv::OutputConfig, 1>{}; }\n"
        "    void tick(iv::TickSampleContext<SampleConfiguredNode> const& ctx) const\n"
        "    { ctx.outputs[0].push(iv::Sample{}); }\n"
        "};\n"
        "}\n\n"
        "IV_NODE(\"iv.test.sample_configured_node\", SampleConfiguredNode);\n");
    iv::test::write_text(
        consumer_package / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        consumer_package / "module.cpp",
        "#include <intravenous/dsl.h>\n"
        "#include <cstddef>\n\n"
        "void mismatched_configuration(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.sample_configured_node\">(std::size_t{1}));\n"
        "}\n"
        "IV_MODULE(\"iv.test.mismatched_configuration\", mismatched_configuration);\n");

    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_package_definitions(consumer_package); },
        "registered-ID argument conversion is not supported");
}

TEST(ModuleLoaderFailures, SamePackageNodeAndModuleCannotShareStableId)
{
    auto const package_root = iv::test::runtime_modules_root()
        / "duplicate_local_registered_id";
    std::filesystem::remove_all(package_root);
    std::filesystem::create_directories(package_root);
    iv::test::write_text(package_root / "iv_project.jsonl", "");
    iv::test::write_text(
        package_root / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test::write_text(
        package_root / "module.cpp",
        "#include <intravenous/dsl.h>\n"
        "#include <array>\n\n"
        "struct DuplicateIdNode {\n"
        "    static constexpr auto outputs()\n"
        "    { return std::array<iv::OutputConfig, 1>{}; }\n"
        "    void tick(iv::TickSampleContext<DuplicateIdNode> const& ctx) const\n"
        "    { ctx.outputs[0].push(iv::Sample{}); }\n"
        "};\n\n"
        "void duplicate_id_module(iv::GraphBuilder& g) { g.outputs(); }\n\n"
        "IV_NODE(\"iv.test.duplicate_local_id\", DuplicateIdNode);\n"
        "IV_MODULE(\"iv.test.duplicate_local_id\", duplicate_id_module);\n");

    auto loader = iv::test::make_loader();
    // IV_NODE and IV_MODULE share the registered-ID namespace. This must fail
    // while compiling the one package rather than survive as a later registry
    // conflict that depends on unrelated reload order.
    expect_failure_contains(
        [&] { (void)loader.load_package_definitions(package_root); },
        "command failed");
}

TEST(ModuleLoaderFailures, SourceWithoutManifestFails)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader({fixtures / "missing_export"});
    expect_failure_contains(
        [&] { (void)loader.load_package_definitions(fixtures / "missing_export"); },
        "iv_package.json");
}

TEST(ModuleLoaderFailures, BuildFailurePropagates)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader({fixtures / "build_failure"});
    expect_failure_contains(
        [&] { (void)loader.load_package_definitions(fixtures / "build_failure"); },
        "command failed");
}

TEST(ModuleLoaderFailures, MissingDependencyFails)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader({fixtures / "missing_dependency"});
    expect_failure_contains(
        [&] { (void)loader.load_package_definitions(fixtures / "missing_dependency"); },
        "is unavailable in the loaded package definitions");
}

TEST(ModuleLoaderFailures, UnrelatedDuplicateDefinitionIdsDoNotBlockLoading)
{
    auto const fixtures = iv::test::test_modules_root();
    auto const duplicates = iv::test::duplicate_modules_root();
    auto loader = iv::test::make_loader({
        fixtures / "nested_loader_project",
        fixtures / "nested_loader_voice",
        duplicates / "one",
        duplicates / "two",
    });
    auto const definitions = loader.load_package_definitions(
        fixtures / "nested_loader_project");
    EXPECT_EQ(definitions.size(), 1u);
    EXPECT_EQ(definitions.front().module_id, "iv.test.nested_loader_project");
}
