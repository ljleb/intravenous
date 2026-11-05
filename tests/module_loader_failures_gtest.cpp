#include "module_test_utils.h"

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/runtime/configuration_expression.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>

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
        [&] { (void)loader.load_project_package_definitions(missing_dir); },
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
        "IV_MODULE(\"iv.test.canonical_source.secondary\", module_secondary);\n"
        "IV_NODE_PRESENTATION(\"iv.test.canonical_source\", "
            "\"iv.test.presentation\", \"Canonical controls\");\n"
        "IV_NODE_PRESENTATION(\"iv.test.canonical_source.secondary\", "
            "\"iv.test.presentation\", \"Secondary controls\");\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_project_package(runtime_root);

    ASSERT_EQ(loaded.definitions.size(), 2);
    auto const primary = std::ranges::find(
        loaded.definitions,
        "iv.test.canonical_source",
        &iv::ModuleLoader::LoadedDefinition::module_id);
    auto const secondary = std::ranges::find(
        loaded.definitions,
        "iv.test.canonical_source.secondary",
        &iv::ModuleLoader::LoadedDefinition::module_id);
    ASSERT_NE(primary, loaded.definitions.end());
    ASSERT_NE(secondary, loaded.definitions.end());
    EXPECT_NE(primary->configured_graph, nullptr);
    EXPECT_NE(secondary->configured_graph, nullptr);
    ASSERT_EQ(loaded.node_presentations.size(), 2u);
    EXPECT_EQ(loaded.node_presentations[0], (iv::NodePresentationDescriptor{
        .definition_id = "iv.test.canonical_source",
        .presentation_id = "iv.test.presentation",
        .display_name = "Canonical controls",
    }));
    EXPECT_EQ(loaded.node_presentations[1], (iv::NodePresentationDescriptor{
        .definition_id = "iv.test.canonical_source.secondary",
        .presentation_id = "iv.test.presentation",
        .display_name = "Secondary controls",
    }));
}

TEST(ModuleLoaderPackages, BuiltinPackageUsesExecutableOwnedArtifactRoot)
{
    // make_loader() loads the shipped package before loading project packages.
    // It must not make the source checkout's builtin directory a build owner.
    auto loader = iv::test::make_loader();
    auto const staged = iv::test::staged_builtin_package_root();
    auto const artifact = loader.compile_package(iv::PackageBuildRequest{
        .package_root = staged,
        .provenance = iv::PackageProvenance::built_in,
        .artifact_root = iv::test::builtin_package_artifact_root(),
    });
    auto const build_root = iv::test::builtin_package_artifact_root() / "build/builtin";
    auto const relative_artifact = artifact.lexically_relative(build_root);

    ASSERT_FALSE(relative_artifact.empty());
    EXPECT_NE(*relative_artifact.begin(), std::filesystem::path{".."});
    EXPECT_TRUE(std::filesystem::is_regular_file(artifact));
}

TEST(ModuleLoaderFailures, BuiltinPackageWithoutExecutableArtifactRootFails)
{
    iv::ModuleLoader loader(iv::test::repo_root());
    EXPECT_THROW(
        static_cast<void>(loader.compile_package(iv::PackageBuildRequest{
            .package_root = iv::test::staged_builtin_package_root(),
            .provenance = iv::PackageProvenance::built_in,
        })),
        std::runtime_error);
}

TEST(ModuleLoaderPackages, CommonPackageUsesSharedCacheInsteadOfProjectBuildTree)
{
    auto const package_root = iv::test_support::make_inline_module_workspace(
        "module_loader_common_artifact_owner",
        R"cpp(
#include <intravenous/dsl.h>
void common_artifact_owner(iv::GraphBuilder& g) { g.outputs(); }
)cpp");
    auto const common_cache = package_root.parent_path() / "common-cache";
    iv::test_support::ScopedEnvVar cache_env(
        "IV_GLOBAL_MODULE_CACHE", common_cache.string());
    iv::ModuleLoader loader(iv::test::repo_root());

    auto const request = iv::PackageBuildRequest{
        .package_root = package_root,
        .provenance = iv::PackageProvenance::common,
    };
    auto const artifact = loader.compile_package(request);

    auto const relative_artifact = artifact.lexically_relative(common_cache);
    ASSERT_FALSE(relative_artifact.empty());
    EXPECT_NE(*relative_artifact.begin(), std::filesystem::path{".."});
    EXPECT_FALSE(std::filesystem::exists(package_root / "build/iv"));

    auto const source_path = package_root / "module.cpp";
    auto const original_stamp = std::filesystem::last_write_time(source_path);
    iv::test_support::write_text(
        source_path,
        iv::test_support::read_text(source_path) + "\n// changed without a timestamp change\n");
    std::filesystem::last_write_time(source_path, original_stamp);
    auto const changed_artifact = loader.compile_package(request);
    EXPECT_NE(changed_artifact, artifact);
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
    auto loaded = loader.load_project_package_definitions(package_root);
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().module_id, "iv.test.scalar_output");
    EXPECT_NE(loaded.front().configured_graph, nullptr);
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
    auto loaded = loader.load_project_package_definitions(package_root);

    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().module_id, "iv.test.builtin_oscillator");
    EXPECT_NE(loaded.front().configured_graph, nullptr);
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
        "    { return std::array<iv::TickOutputConfig<iv::Sample>, 1>{}; }\n"
        "    void tick(iv::TickSampleContext<LocalShortNameOscillator> const& ctx) const\n"
        "    { ctx.template output<0>().push(value); }\n"
        "};\n\n"
        "void local_short_name_consumer(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"saw_oscillator\">(iv::Sample{0.25f}));\n"
        "}\n\n"
        "IV_NODE(\"saw_oscillator\", LocalShortNameOscillator);\n"
        "IV_MODULE(\"iv.test.local_short_name_consumer\", local_short_name_consumer);\n");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_project_package_definitions(package_root);

    // The shipped saw oscillator does not accept this construction argument.
    // Reaching a valid root proves the exact local ID won before the bare-ID
    // builtin alias was considered.
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().module_id, "iv.test.local_short_name_consumer");
    EXPECT_NE(loaded.front().configured_graph, nullptr);
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
    auto loaded = loader.load_project_package_definitions(package_root);

    ASSERT_EQ(loaded.size(), 2u);
    auto const stereo = std::ranges::find(
        loaded,
        "iv.test.tiled_stereo_voice",
        &iv::ModuleLoader::LoadedDefinition::module_id);
    ASSERT_NE(stereo, loaded.end());
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
        [&] { (void)loader.load_project_package_definitions(package_root); },
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
        [&] { (void)loader.load_project_package_definitions(package_root); },
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
    auto loaded = loader.load_project_package_definitions(package_root);
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
    auto loader = iv::test::make_loader();

    // Provider packages are independent compilation units. Make the active
    // package set explicit instead of relying on loader-wide source scanning.
    ASSERT_NO_THROW((void)loader.load_project_package_definitions(
        fixtures / "nested_loader_voice"));
    auto loaded = loader.load_project_package_definitions(fixtures / "nested_loader_project");

    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().module_id, "iv.test.nested_loader_project");
    EXPECT_NE(loaded.front().configured_graph, nullptr);
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
        "        return std::array<iv::TickOutputConfig<iv::Sample>, 1>{};\n"
        "    }\n\n"
        "    void tick(iv::TickSampleContext<RegisteredPackageNode> const& ctx) const\n"
        "    {\n"
        "        ctx.template output<0>().push(0.25);\n"
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
    auto batch = loader.load_packages({
        iv::PackageBuildRequest{
            .package_root = node_package,
            .provenance = iv::PackageProvenance::project_local,
        },
        iv::PackageBuildRequest{
            .package_root = consumer_package,
            .provenance = iv::PackageProvenance::project_local,
        },
    });
    ASSERT_EQ(batch.size(), 2u);
    ASSERT_TRUE(batch[0]) << batch[0].error;
    ASSERT_TRUE(batch[1]) << batch[1].error;
    auto loaded = std::move(batch[1].package->definitions);

    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded.front().module_id, "iv.test.registered_node_consumer");
    EXPECT_NE(loaded.front().configured_graph, nullptr);
    ASSERT_NE(loaded.front().configured_graph, nullptr);

    std::optional<iv::RegisteredNodeTypeIdentity> registered_identity;
    loaded.front().configured_graph->node_bundles.for_each_configured_bundle(
        [&](iv::ConfiguredNodeBundleView const& bundle) {
            if (bundle.registered_node_type_identity) {
                ASSERT_FALSE(registered_identity.has_value());
                registered_identity = *bundle.registered_node_type_identity;
            }
        });
    ASSERT_TRUE(registered_identity.has_value());
    EXPECT_EQ(
        registered_identity->node_type_id,
        "iv.test.registered_package_node");
    EXPECT_EQ(
        std::filesystem::path(registered_identity->provider_package_root),
        std::filesystem::weakly_canonical(node_package));

    // A source disappearing must stop being a provider for new configurations
    // immediately. Existing LoadedDefinition objects above still retain the
    // old node package through ModuleRef, but rebuilding this consumer may not
    // silently bind to a package that discovery has removed.
    loader.remove_package(node_package);
    expect_failure_contains(
        [&] { (void)loader.load_project_package_definitions(consumer_package); },
        "iv.test.registered_package_node");
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
        "#include <array>\n"
        "#include <vector>\n\n"
        "namespace {\n"
        "struct ConfiguredProviderNode {\n"
        "    iv::Sample gain;\n"
        "    constexpr explicit ConfiguredProviderNode(iv::Sample gain_ = iv::Sample{0.25f})\n"
        "        : gain(gain_) {}\n"
        "    static constexpr auto outputs()\n"
        "    { return std::array{iv::TickOutputConfig<iv::Sample>{.name = \"out\"}}; }\n"
        "    void tick(iv::TickSampleContext<ConfiguredProviderNode> const& ctx) const\n"
        "    { ctx.output<\"out\">().push(gain); }\n"
        "};\n"
        "struct RequiredConfiguredProviderNode {\n"
        "    iv::Sample gain;\n"
        "    constexpr explicit RequiredConfiguredProviderNode(iv::Sample gain_)\n"
        "        : gain(gain_) {}\n"
        "    static constexpr auto outputs()\n"
        "    { return std::array{iv::TickOutputConfig<iv::Sample>{.name = \"out\"}}; }\n"
        "    void tick(iv::TickSampleContext<RequiredConfiguredProviderNode> const& ctx) const\n"
        "    { ctx.output<\"out\">().push(gain); }\n"
        "};\n"
        "struct LabelConfiguredProviderNode {\n"
        "    char const* label;\n"
        "    constexpr explicit LabelConfiguredProviderNode(char const* label_)\n"
        "        : label(label_) {}\n"
        "    static constexpr auto outputs()\n"
        "    { return std::array{iv::TickOutputConfig<iv::Sample>{.name = \"out\"}}; }\n"
        "    void tick(iv::TickSampleContext<LabelConfiguredProviderNode> const& ctx) const\n"
        "    { ctx.output<\"out\">().push(iv::Sample{}); }\n"
        "};\n"
        "enum class FilterMode : unsigned { lowpass, highpass };\n"
        "struct Band {\n"
        "    double frequency = 1000.0;\n"
        "    double gain = 0.0;\n"
        "};\n"
        "struct FilterOptions {\n"
        "    double cutoff = 440.0;\n"
        "    FilterMode mode = FilterMode::lowpass;\n"
        "    std::vector<Band> bands{};\n"
        "    std::array<double, 2> resonance{};\n"
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
        "IV_MODULE(\"iv.test.required_provider_module\", required_provider_module);\n"
        "void aggregate_provider_module(iv::GraphBuilder& g, FilterOptions)\n"
        "{ g.outputs(); }\n"
        "IV_MODULE(\"iv.test.aggregate_provider_module\", aggregate_provider_module);\n"
        "void framework_enum_provider_module(\n"
        "    iv::GraphBuilder& g, iv::SampleStreamLayout)\n"
        "{ g.outputs(); }\n"
        "IV_MODULE(\"iv.test.framework_enum_provider_module\",\n"
        "    framework_enum_provider_module);\n"
        "struct ClassProviderModule {\n"
        "    iv::Sample gain;\n"
        "    explicit ClassProviderModule(iv::Sample gain_) : gain(gain_) {}\n"
        "    void build_graph(iv::GraphBuilder& g) const\n"
        "    {\n"
        "        auto source = g.node<\"iv.test.configured_provider_node\">(gain);\n"
        "        g.outputs(source);\n"
        "    }\n"
        "    void on_message(\n"
        "        iv::ModuleMessageContext<ClassProviderModule>& ctx,\n"
        "        iv::Sample const& event) const\n"
        "    { ctx.send(ctx.ref<\"source\">(), event); }\n"
        "};\n"
        "IV_MODULE_CLASS(\"iv.test.class_provider_module\", ClassProviderModule);\n");
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
        "void class_module_reference(iv::GraphBuilder& g)\n"
        "{\n"
        "    g.outputs(g.node<\"iv.test.class_provider_module\">(iv::Sample{0.25f}));\n"
        "}\n"
        "IV_MODULE(\"iv.test.direct_configured_node\", direct_configured_node);\n"
        "IV_MODULE(\"iv.test.configured_module_reference\", configured_module_reference);\n"
        "IV_MODULE(\"iv.test.required_configured_node\", required_configured_node);\n"
        "IV_MODULE(\"iv.test.required_configured_module\", required_configured_module);\n"
        "IV_MODULE(\"iv.test.string_literal_configuration\", string_literal_configuration);\n"
        "IV_MODULE(\"iv.test.default_configured_module_reference\", "
        "default_configured_module_reference);\n"
        "IV_MODULE(\"iv.test.class_module_reference\", class_module_reference);\n");

    auto loader = iv::test::make_loader();
    auto provider_package_revision = loader.load_project_package(provider_package);
    ASSERT_TRUE(std::filesystem::is_regular_file(
        provider_package_revision.compiler_artifact.node_event_protocol_path));
    std::ifstream event_protocol_stream(
        provider_package_revision.compiler_artifact.node_event_protocol_path);
    ASSERT_TRUE(event_protocol_stream);
    auto const event_protocol = nlohmann::ordered_json::parse(
        event_protocol_stream);
    EXPECT_EQ(event_protocol["formatVersion"], 1);
    ASSERT_EQ(event_protocol["eventTypes"].size(), 1u);
    EXPECT_FALSE(event_protocol["eventTypes"][0]["nominalId"]
        .get<std::string>().empty());
    EXPECT_FALSE(event_protocol["eventTypes"][0]["definitionFingerprint"]
        .get<std::string>().empty());
    EXPECT_EQ(
        event_protocol["eventTypes"][0]["payload"]["kind"],
        "convertedScalar");
    ASSERT_EQ(event_protocol["definitions"].size(), 1u);
    EXPECT_EQ(
        event_protocol["definitions"][0]["definitionId"],
        "iv.test.class_provider_module");
    ASSERT_EQ(
        event_protocol["definitions"][0]["supportedEventTypes"].size(),
        1u);
    EXPECT_EQ(
        event_protocol["definitions"][0]["supportedEventTypes"][0]
            ["nominalId"],
        event_protocol["eventTypes"][0]["nominalId"]);
    auto const protocol_text = event_protocol.dump();
    EXPECT_EQ(protocol_text.find("nodeCodeKey"), std::string::npos);
    EXPECT_EQ(protocol_text.find("methodSymbol"), std::string::npos);
    EXPECT_EQ(protocol_text.find("bitOffset"), std::string::npos);
    auto const& provider_definitions = provider_package_revision.definitions;
    ASSERT_EQ(provider_package_revision.module_message_handlers.size(), 1u);
    auto const& class_handler =
        provider_package_revision.module_message_handlers.front();
    EXPECT_EQ(
        std::string_view(
            static_cast<char const*>(class_handler.definition_id.data),
            class_handler.definition_id.size),
        "iv.test.class_provider_module");
    EXPECT_NE(class_handler.event_type, nullptr);
    EXPECT_NE(class_handler.invoke, nullptr);
    auto const provider_definition = std::ranges::find(
        provider_definitions,
        "iv.test.configured_provider_module",
        &iv::ModuleLoader::LoadedDefinition::module_id);
    ASSERT_NE(provider_definition, provider_definitions.end());
    ASSERT_NE(provider_definition->provider.signature, nullptr);
    auto const* provider_signature = provider_definition->provider.signature();
    ASSERT_NE(provider_signature, nullptr);
    ASSERT_EQ(provider_signature->argument_count, 1u);
    ASSERT_NE(provider_signature->parameter_types[0], nullptr);
    ASSERT_NE(provider_signature->parameter_types[0]->expression_schema, nullptr);
    EXPECT_EQ(
        provider_signature->parameter_types[0]->expression_schema->kind,
        iv::details::ConfigurationExpressionTypeKind::converted_scalar);
    EXPECT_NO_THROW({
        auto values = iv::compile_configuration_expression_list(
            "0.75", *provider_signature);
        EXPECT_EQ(values.arguments().size(), 1u);
    });
    auto const aggregate_definition = std::ranges::find(
        provider_definitions,
        "iv.test.aggregate_provider_module",
        &iv::ModuleLoader::LoadedDefinition::module_id);
    ASSERT_NE(aggregate_definition, provider_definitions.end());
    auto const* aggregate_signature = aggregate_definition->provider.signature();
    ASSERT_NE(aggregate_signature, nullptr);
    ASSERT_EQ(aggregate_signature->argument_count, 1u);
    auto const* aggregate_schema =
        aggregate_signature->parameter_types[0]->expression_schema;
    ASSERT_NE(aggregate_schema, nullptr);
    ASSERT_EQ(
        aggregate_schema->kind,
        iv::details::ConfigurationExpressionTypeKind::aggregate);
    ASSERT_EQ(aggregate_schema->field_count, 4u);
    EXPECT_EQ(
        aggregate_schema->fields[1].type->kind,
        iv::details::ConfigurationExpressionTypeKind::enumeration);
    EXPECT_EQ(aggregate_schema->fields[1].type->enumerator_count, 2u);
    EXPECT_EQ(
        aggregate_schema->fields[2].type->kind,
        iv::details::ConfigurationExpressionTypeKind::sequence);
    ASSERT_NE(aggregate_schema->fields[2].type->element_type, nullptr);
    EXPECT_EQ(
        aggregate_schema->fields[2].type->element_type->kind,
        iv::details::ConfigurationExpressionTypeKind::aggregate);
    EXPECT_EQ(
        aggregate_schema->fields[3].type->kind,
        iv::details::ConfigurationExpressionTypeKind::sequence);
    EXPECT_EQ(aggregate_schema->fields[3].type->fixed_element_count, 2u);
    EXPECT_NO_THROW({
        auto values = iv::compile_configuration_expression_list(
            "{.cutoff = 1800, .mode = highpass, "
            ".bands = {{.frequency = 200, .gain = -3}, "
            "{.frequency = 2000, .gain = 2.5}}, "
            ".resonance = {0.25, 0.5}}",
            *aggregate_signature);
        EXPECT_EQ(values.arguments().size(), 1u);
    });
    auto const framework_enum_definition = std::ranges::find(
        provider_definitions,
        "iv.test.framework_enum_provider_module",
        &iv::ModuleLoader::LoadedDefinition::module_id);
    ASSERT_NE(framework_enum_definition, provider_definitions.end());
    auto const* framework_enum_signature =
        framework_enum_definition->provider.signature();
    ASSERT_NE(framework_enum_signature, nullptr);
    ASSERT_EQ(framework_enum_signature->argument_count, 1u);
    auto const* framework_enum_schema =
        framework_enum_signature->parameter_types[0]->expression_schema;
    ASSERT_NE(framework_enum_schema, nullptr);
    EXPECT_EQ(
        framework_enum_schema->kind,
        iv::details::ConfigurationExpressionTypeKind::enumeration);
    ASSERT_GE(framework_enum_schema->enumerator_count, 2u);
    EXPECT_EQ(
        std::string_view(
            framework_enum_schema->enumerators[0].name,
            framework_enum_schema->enumerators[0].name_size),
        "planar");
    EXPECT_EQ(
        std::string_view(
            framework_enum_schema->enumerators[1].name,
            framework_enum_schema->enumerators[1].name_size),
        "interleaved");
    EXPECT_NO_THROW({
        auto values = iv::compile_configuration_expression_list(
            "interleaved", *framework_enum_signature);
        EXPECT_EQ(values.arguments().size(), 1u);
    });
    auto const class_definition = std::ranges::find(
        provider_definitions,
        "iv.test.class_provider_module",
        &iv::ModuleLoader::LoadedDefinition::module_id);
    ASSERT_NE(class_definition, provider_definitions.end());
    auto const* class_signature = class_definition->provider.signature();
    ASSERT_NE(class_signature, nullptr);
    ASSERT_EQ(class_signature->argument_count, 1u);
    auto loaded = loader.load_project_package_definitions(consumer_package);

    ASSERT_EQ(loaded.size(), 7u);
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
        "iv.test.class_module_reference",
        &iv::ModuleLoader::LoadedDefinition::module_id),
        loaded.end());
    EXPECT_NE(std::ranges::find(
        loaded,
        "iv.test.default_configured_module_reference",
        &iv::ModuleLoader::LoadedDefinition::module_id),
        loaded.end());

    auto const& provider_loaded = provider_package_revision.definitions;
    ASSERT_EQ(provider_loaded.size(), 5u);
    auto const required_provider = std::ranges::find(
        provider_loaded,
        "iv.test.required_provider_module",
        &iv::ModuleLoader::LoadedDefinition::module_id);
    ASSERT_NE(required_provider, provider_loaded.end());
    EXPECT_EQ(required_provider->configured_graph, nullptr);
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
        "    { return std::array<iv::TickOutputConfig<iv::Sample>, 1>{}; }\n"
        "    void tick(iv::TickSampleContext<SampleConfiguredNode> const& ctx) const\n"
        "    { ctx.template output<0>().push(iv::Sample{}); }\n"
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
    ASSERT_NO_THROW((void)loader.load_project_package_definitions(provider_package));
    expect_failure_contains(
        [&] { (void)loader.load_project_package_definitions(consumer_package); },
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
        "    { return std::array<iv::TickOutputConfig<iv::Sample>, 1>{}; }\n"
        "    void tick(iv::TickSampleContext<DuplicateIdNode> const& ctx) const\n"
        "    { ctx.template output<0>().push(iv::Sample{}); }\n"
        "};\n\n"
        "void duplicate_id_module(iv::GraphBuilder& g) { g.outputs(); }\n\n"
        "IV_NODE(\"iv.test.duplicate_local_id\", DuplicateIdNode);\n"
        "IV_MODULE(\"iv.test.duplicate_local_id\", duplicate_id_module);\n");

    auto loader = iv::test::make_loader();
    // IV_NODE and IV_MODULE share the registered-ID namespace. This must fail
    // while compiling the one package rather than survive as a later registry
    // conflict that depends on unrelated reload order.
    expect_failure_contains(
        [&] { (void)loader.load_project_package_definitions(package_root); },
        "command failed");
}

TEST(ModuleLoaderFailures, SourceWithoutManifestFails)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_project_package_definitions(fixtures / "missing_export"); },
        "iv_package.json");
}

TEST(ModuleLoaderFailures, BuildFailurePropagates)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_project_package_definitions(fixtures / "build_failure"); },
        "command failed");
}

TEST(ModuleLoaderFailures, MissingDependencyFails)
{
    auto const fixtures = iv::test::test_modules_root();
    auto loader = iv::test::make_loader();
    expect_failure_contains(
        [&] { (void)loader.load_project_package_definitions(fixtures / "missing_dependency"); },
        "is unavailable in the loaded package definitions");
}

TEST(ModuleLoaderFailures, UnrelatedDuplicateDefinitionIdsDoNotBlockLoading)
{
    auto const fixtures = iv::test::test_modules_root();
    auto const duplicates = iv::test::duplicate_modules_root();
    auto loader = iv::test::make_loader();
    ASSERT_NO_THROW((void)loader.load_project_package_definitions(
        fixtures / "nested_loader_voice"));
    auto const definitions = loader.load_project_package_definitions(
        fixtures / "nested_loader_project");
    EXPECT_EQ(definitions.size(), 1u);
    EXPECT_EQ(definitions.front().module_id, "iv.test.nested_loader_project");
}
