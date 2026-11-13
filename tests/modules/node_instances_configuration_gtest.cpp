#include <intravenous/basic_nodes/arithmetic.h>
#include "../event_source_test_support.h"
#include "../node_instances_test_driver.h"
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/module/package_definitions.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/package_pipeline_types.h>
#include <intravenous/runtime/project_node_path_resolver.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using iv::details::ConfigurationArgument;
using iv::details::ConfigurationTypeIdentity;
using iv::details::ConfigurationValueOperations;
using iv::details::PackageDefinition;
using iv::details::PackageDefinitionKind;
using iv::details::RegisteredSignature;

constexpr char package_root_text[] = "/tmp/iv-node-instances-configuration-tests";
constexpr char source_file_text[] =
    "/tmp/iv-node-instances-configuration-tests/module.cpp";
constexpr char int_nominal[] = "test:int";
constexpr char int_fingerprint[] = "test:int:v1";
constexpr char int_display[] = "int";

iv::details::ConfigurationExpressionTypeSchema const int_expression_schema{
    .kind = iv::details::ConfigurationExpressionTypeKind::signed_integer,
    .size = sizeof(int),
    .alignment = alignof(int),
    .integer_is_signed = true,
    .integer_bit_width = sizeof(int) * 8,
    .construct_scalar =
        &iv::details::configuration_expression_details::construct_scalar<int>,
    .default_construct =
        &iv::details::configuration_expression_details::default_construct<int>,
    .destroy = &iv::details::configuration_expression_details::destroy<int>,
};

ConfigurationTypeIdentity const int_identity{
    .nominal_id = int_nominal,
    .nominal_id_size = sizeof(int_nominal) - 1,
    .definition_fingerprint = int_fingerprint,
    .definition_fingerprint_size = sizeof(int_fingerprint) - 1,
    .display_name = int_display,
    .display_name_size = sizeof(int_display) - 1,
    .expression_schema = &int_expression_schema,
};

ConfigurationTypeIdentity const* const int_types[]{&int_identity};
ConfigurationValueOperations const* const int_operations[]{
    iv::details::configuration_value_operations<int>(),
};

RegisteredSignature const zero_signature{};
RegisteredSignature const one_int_signature{
    .parameter_types = int_types,
    .parameter_operations = int_operations,
    .required_argument_count = 1,
    .argument_count = 1,
};

ConfigurationValueOperations const noncacheable_int_operations{
    .size = sizeof(int),
    .alignment = alignof(int),
    .copy_construct = [](void* destination, void const* source) {
        ::new (destination) int(*static_cast<int const*>(source));
    },
    .destroy = [](void* value) noexcept {
        std::destroy_at(static_cast<int*>(value));
    },
};
ConfigurationValueOperations const* const noncacheable_int_operations_array[]{
    &noncacheable_int_operations,
};
RegisteredSignature const one_noncacheable_int_signature{
    .parameter_types = int_types,
    .parameter_operations = noncacheable_int_operations_array,
    .required_argument_count = 1,
    .argument_count = 1,
};

std::weak_ptr<void> provider_lifetime_weak{};
std::atomic<bool> value_destroyed_after_provider_release{false};
ConfigurationValueOperations const lifetime_checked_int_operations{
    .size = sizeof(int),
    .alignment = alignof(int),
    .copy_construct = [](void* destination, void const* source) {
        ::new (destination) int(*static_cast<int const*>(source));
    },
    .destroy = [](void* value) noexcept {
        if (provider_lifetime_weak.expired()) {
            value_destroyed_after_provider_release = true;
        }
        std::destroy_at(static_cast<int*>(value));
    },
    .equal = [](void const* lhs, void const* rhs) {
        return *static_cast<int const*>(lhs) == *static_cast<int const*>(rhs);
    },
    .hash = [](void const* value) {
        return std::hash<int>{}(*static_cast<int const*>(value));
    },
};
ConfigurationValueOperations const* const lifetime_checked_int_operations_array[]{
    &lifetime_checked_int_operations,
};
RegisteredSignature const lifetime_checked_int_signature{
    .parameter_types = int_types,
    .parameter_operations = lifetime_checked_int_operations_array,
    .required_argument_count = 1,
    .argument_count = 1,
};

RegisteredSignature const* zero_signature_callback() { return &zero_signature; }
RegisteredSignature const* int_signature_callback() { return &one_int_signature; }
RegisteredSignature const* noncacheable_int_signature_callback()
{
    return &one_noncacheable_int_signature;
}
RegisteredSignature const* lifetime_checked_int_signature_callback()
{
    return &lifetime_checked_int_signature;
}

ConfigurationArgument int_argument(int& value)
{
    return ConfigurationArgument{
        .data = &value,
        .type = &int_identity,
        .is_const = false,
        .is_rvalue = false,
        .is_array_decay = false,
    };
}

std::atomic<int> counted_calls{0};
std::atomic<int> nested_parent_calls{0};
std::atomic<int> nested_child_calls{0};
std::atomic<int> noncacheable_calls{0};
std::atomic<int> leaf_calls{0};

void counted_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    ++counted_calls;
    graph.outputs();
}

constexpr char counted_id[] = "iv.test.node_instances.counted";
constexpr char nested_parent_id[] = "iv.test.node_instances.parent";
constexpr char nested_child_id[] = "iv.test.node_instances.child";
constexpr char noncacheable_id[] = "iv.test.node_instances.noncacheable";
constexpr char cycle_a_id[] = "iv.test.node_instances.cycle_a";
constexpr char cycle_b_id[] = "iv.test.node_instances.cycle_b";
constexpr char leaf_id[] = "iv.test.node_instances.leaf";

void nested_child_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    ++nested_child_calls;
    graph.outputs();
}

void nested_parent_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    ++nested_parent_calls;
    int child_value = 7;
    auto argument = int_argument(child_value);
    std::array arguments{argument};
    (void)iv::details::configure_package_definition(
        graph, nested_child_id, std::span<ConfigurationArgument>(arguments));
    graph.outputs();
}

void noncacheable_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    ++noncacheable_calls;
    graph.outputs();
}

void cycle_a_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    (void)iv::details::configure_package_definition(graph, cycle_b_id, {});
    graph.outputs();
}

void cycle_b_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    (void)iv::details::configure_package_definition(graph, cycle_a_id, {});
    graph.outputs();
}

iv::NodeRef leaf_node(
    iv::GraphBuilder& graph,
    std::span<ConfigurationArgument>,
    iv::ChannelLayout const* layout)
{
    ++leaf_calls;
    if (layout) throw std::invalid_argument("test leaf does not support tiled construction");
    return iv::details::configure_concrete_node<iv::Constant>(
        graph, iv::Sample{0.25f}).node_ref();
}

struct DefinitionSpec {
    std::string_view id{};
    PackageDefinitionKind kind = PackageDefinitionKind::module;
    iv::details::IvModuleConfigureFunction module_build = nullptr;
    iv::details::NodeTypeConfigureFunction node_build = nullptr;
    iv::details::RegisteredSignatureFunction signature = nullptr;
};

PackageDefinition raw_definition(DefinitionSpec const& spec)
{
    return PackageDefinition{
        .kind = spec.kind,
        .id = spec.id.data(),
        .id_size = spec.id.size(),
        .source_file = source_file_text,
        .source_file_size = sizeof(source_file_text) - 1,
        .package_root = package_root_text,
        .package_root_size = sizeof(package_root_text) - 1,
        .module_build = spec.module_build,
        .node_build = spec.node_build,
        .node_compiler_record = spec.kind == PackageDefinitionKind::node
            ? static_cast<void const*>(
                &iv::details::node_compiler_record<iv::Constant>)
            : nullptr,
        .signature = spec.signature,
    };
}

std::shared_ptr<iv::NodeDefinitionsSnapshot const> make_snapshot(
    std::uint64_t generation,
    std::initializer_list<DefinitionSpec> specs,
    iv::ModuleRef package_code = {})
{
    auto revision = std::make_shared<iv::PackageRevision>();
    revision->package_id = "iv.test.node_instances.package";
    revision->package_root = package_root_text;
    revision->revision = generation;
    revision->package_code = std::move(package_code);
    revision->provider_definitions.reserve(specs.size());

    auto snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>();
    snapshot->generation = generation;
    snapshot->by_id.reserve(specs.size());
    for (auto const& spec : specs) {
        auto raw = raw_definition(spec);
        revision->provider_definitions.push_back(raw);
        auto provider = iv::NodeDefinitionProvider{
            .module_build = spec.module_build,
            .leaf_build = spec.node_build,
            .signature = spec.signature,
        };
        if (spec.kind == PackageDefinitionKind::module) {
            snapshot->by_id.emplace(std::string(spec.id), iv::NodeDefinitionEntry{
                .definition_id = std::string(spec.id),
                .kind = iv::NodeDefinitionKind::module,
                .version = generation,
                .definition = iv::ModuleNodeDefinition{
                    .definition_id = std::string(spec.id),
                    .package_id = revision->package_id,
                    .package_root = revision->package_root,
                    .module_id = std::string(spec.id),
                    .provider = provider,
                },
            });
        } else {
            snapshot->by_id.emplace(std::string(spec.id), iv::NodeDefinitionEntry{
                .definition_id = std::string(spec.id),
                .kind = iv::NodeDefinitionKind::leaf,
                .version = generation,
                .definition = iv::LeafNodeDefinition{
                    .definition_id = std::string(spec.id),
                    .package_id = revision->package_id,
                    .package_root = revision->package_root,
                    .provider = provider,
                    .compiler_record = iv::details::node_compiler_record<iv::Constant>,
                },
            });
        }
    }
    snapshot->package_revisions.push_back(std::move(revision));
    return snapshot;
}

DefinitionSpec counted_spec()
{
    return {
        .id = counted_id,
        .module_build = &counted_module,
        .signature = &int_signature_callback,
    };
}

iv::NodeInstanceConfigurationRequest request(
    std::string instance_id,
    std::string_view definition_id,
    std::span<ConfigurationArgument> arguments = {})
{
    return {
        .instance_id = std::move(instance_id),
        .definition_id = std::string(definition_id),
        .arguments = arguments,
    };
}
} // namespace

TEST(NodeInstancesConfiguration, EqualTypedValuesShareConfigurationButNotPlacement)
{
    counted_calls = 0;
    auto snapshot = make_snapshot(1, {counted_spec()});
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    int first_value = 11;
    int second_value = 11;
    std::array first_arguments{int_argument(first_value)};
    std::array second_arguments{int_argument(second_value)};
    std::array requests{
        request("first", counted_id, first_arguments),
        request("second", counted_id, second_arguments),
    };

    auto result = instances.configure_and_embed(snapshot, root, requests);

    ASSERT_TRUE(result.diagnostics.empty());
    ASSERT_EQ(result.placements.size(), 2u);
    EXPECT_EQ(counted_calls.load(), 1);
    EXPECT_EQ(instances.configuration_cache_size(), 1u);
    auto const& first = result.placements.at("first");
    auto const& second = result.placements.at("second");
    EXPECT_EQ(first.configured, second.configured);
    EXPECT_NE(first.root, second.root);
}

TEST(NodeInstancesConfiguration, ExpressionValuesEnterTheExistingTypedCachePath)
{
    counted_calls = 0;
    auto snapshot = make_snapshot(1, {counted_spec()});
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    std::array requests{
        iv::NodeInstanceConfigurationRequest{
            .instance_id = "first",
            .definition_id = counted_id,
            .configuration_source = "5 + 6",
        },
        iv::NodeInstanceConfigurationRequest{
            .instance_id = "second",
            .definition_id = counted_id,
            .configuration_source = "11",
        },
    };

    auto result = instances.configure_and_embed(snapshot, root, requests);

    ASSERT_TRUE(result.diagnostics.empty());
    ASSERT_EQ(result.placements.size(), 2u);
    EXPECT_EQ(counted_calls.load(), 1);
    EXPECT_EQ(instances.configuration_cache_size(), 1u);
}

TEST(NodeInstancesConfiguration, DifferentTypedValuesCreateDistinctCacheEntries)
{
    counted_calls = 0;
    auto snapshot = make_snapshot(1, {counted_spec()});
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    int first_value = 11;
    int second_value = 12;
    std::array first_arguments{int_argument(first_value)};
    std::array second_arguments{int_argument(second_value)};
    std::array requests{
        request("first", counted_id, first_arguments),
        request("second", counted_id, second_arguments),
    };

    auto result = instances.configure_and_embed(snapshot, root, requests);

    ASSERT_TRUE(result.diagnostics.empty());
    EXPECT_EQ(counted_calls.load(), 2);
    EXPECT_EQ(instances.configuration_cache_size(), 2u);
    EXPECT_NE(
        result.placements.at("first").configured,
        result.placements.at("second").configured);
}

TEST(NodeInstancesConfiguration, NestedCacheMissesUseOneSnapshotAndReuseChildValues)
{
    nested_parent_calls = 0;
    nested_child_calls = 0;
    auto snapshot = make_snapshot(7, {
        DefinitionSpec{
            .id = nested_parent_id,
            .module_build = &nested_parent_module,
            .signature = &int_signature_callback,
        },
        DefinitionSpec{
            .id = nested_child_id,
            .module_build = &nested_child_module,
            .signature = &int_signature_callback,
        },
    });
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    int first_value = 1;
    int second_value = 2;
    std::array first_arguments{int_argument(first_value)};
    std::array second_arguments{int_argument(second_value)};
    std::array requests{
        request("first", nested_parent_id, first_arguments),
        request("second", nested_parent_id, second_arguments),
    };

    auto result = instances.configure_and_embed(snapshot, root, requests);

    ASSERT_TRUE(result.diagnostics.empty());
    EXPECT_EQ(nested_parent_calls.load(), 2);
    EXPECT_EQ(nested_child_calls.load(), 1);
    EXPECT_EQ(instances.configuration_cache_size(), 3u);
    EXPECT_EQ(result.placements.at("first").configured->definitions, snapshot);
    EXPECT_EQ(result.placements.at("second").configured->definitions, snapshot);
    ASSERT_EQ(result.placements.at("first").configured->dependencies.size(), 1u);
    EXPECT_EQ(
        result.placements.at("first").configured->dependencies.front()
            ->definitions_generation,
        7u);
}

TEST(NodeInstancesConfiguration, NonComparableArgumentsReconfigureAndKeepOwnedLifetime)
{
    noncacheable_calls = 0;
    auto snapshot = make_snapshot(1, {
        DefinitionSpec{
            .id = noncacheable_id,
            .module_build = &noncacheable_module,
            .signature = &noncacheable_int_signature_callback,
        },
    });
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    int first_value = 4;
    int second_value = 4;
    std::array first_arguments{int_argument(first_value)};
    std::array second_arguments{int_argument(second_value)};
    std::array requests{
        request("first", noncacheable_id, first_arguments),
        request("second", noncacheable_id, second_arguments),
    };

    auto result = instances.configure_and_embed(snapshot, root, requests);

    ASSERT_TRUE(result.diagnostics.empty());
    EXPECT_EQ(noncacheable_calls.load(), 2);
    EXPECT_EQ(instances.configuration_cache_size(), 0u);
    EXPECT_NE(
        result.placements.at("first").configured,
        result.placements.at("second").configured);
    EXPECT_NE(
        result.placements.at("first").configured->configuration_lifetime,
        nullptr);
}

TEST(NodeInstancesConfiguration, RecursiveDefinitionCycleIsDiagnosedWithoutCachingPartialGraphs)
{
    auto snapshot = make_snapshot(1, {
        DefinitionSpec{
            .id = cycle_a_id,
            .module_build = &cycle_a_module,
            .signature = &zero_signature_callback,
        },
        DefinitionSpec{
            .id = cycle_b_id,
            .module_build = &cycle_b_module,
            .signature = &zero_signature_callback,
        },
    });
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    std::array requests{request("cycle", cycle_a_id)};

    auto result = instances.configure_and_embed(snapshot, root, requests);

    EXPECT_TRUE(result.placements.empty());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_NE(result.diagnostics.front().message.find("cycle"), std::string::npos);
    EXPECT_EQ(instances.configuration_cache_size(), 0u);
}

TEST(NodeInstancesConfiguration, OneBadRequestFailsTheWholeBatchBeforeConstruction)
{
    counted_calls = 0;
    auto snapshot = make_snapshot(1, {counted_spec()});
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    int value = 3;
    std::array arguments{int_argument(value)};
    std::array requests{
        request("good", counted_id, arguments),
        request("missing", "iv.test.node_instances.missing"),
    };

    auto result = instances.configure_and_embed(snapshot, root, requests);

    // An unknown definition is found before construction, so nothing is built.
    EXPECT_TRUE(result.placements.empty());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().instance_id, "missing");
    EXPECT_EQ(counted_calls.load(), 0);
}

TEST(NodeInstancesConfiguration, DuplicateInstanceIdFailsTheBatchBeforeConstruction)
{
    counted_calls = 0;
    auto snapshot = make_snapshot(1, {counted_spec()});
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    int value = 5;
    std::array arguments{int_argument(value)};
    std::array requests{
        request("duplicate", counted_id, arguments),
        request("duplicate", counted_id, arguments),
    };

    auto result = instances.configure_and_embed(snapshot, root, requests);

    EXPECT_TRUE(result.placements.empty());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_NE(result.diagnostics.front().message.find("duplicate"), std::string::npos);
    EXPECT_EQ(counted_calls.load(), 0);
}

TEST(NodeInstancesConfiguration, SnapshotPublicationInvalidatesChangedDefinition)
{
    counted_calls = 0;
    auto first_snapshot = make_snapshot(1, {counted_spec()});
    auto second_snapshot = make_snapshot(2, {counted_spec()});
    iv::NodeInstances instances;
    iv::test::NodeInstancesDriver driver {instances};
    (void)driver.install(first_snapshot);
    int value = 9;
    std::array arguments{int_argument(value)};
    std::array requests{request("first", counted_id, arguments)};
    iv::GraphBuilder first_root;
    auto first = instances.configure_and_embed(first_snapshot, first_root, requests);
    ASSERT_TRUE(first.diagnostics.empty());
    ASSERT_EQ(instances.configuration_cache_size(), 1u);

    (void)driver.install(second_snapshot);
    EXPECT_EQ(instances.configuration_cache_size(), 0u);

    iv::GraphBuilder second_root;
    requests[0].instance_id = "second";
    auto second = instances.configure_and_embed(second_snapshot, second_root, requests);
    ASSERT_TRUE(second.diagnostics.empty());
    EXPECT_EQ(counted_calls.load(), 2);
    EXPECT_EQ(second.placements.at("second").configured->definitions_generation, 2u);
}

TEST(NodeInstancesConfiguration, SnapshotPublicationRetainsUnchangedDefinitionClosure)
{
    counted_calls = 0;
    auto first_snapshot = make_snapshot(1, {counted_spec()});
    iv::NodeInstances instances;
    iv::test::NodeInstancesDriver driver {instances};
    (void)driver.install(first_snapshot);
    int value = 9;
    std::array arguments{int_argument(value)};
    std::array requests{request("first", counted_id, arguments)};
    iv::GraphBuilder first_root;
    auto first = instances.configure_and_embed(first_snapshot, first_root, requests);
    ASSERT_TRUE(first.diagnostics.empty());
    ASSERT_EQ(instances.configuration_cache_size(), 1u);

    auto second_snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>(
        *first_snapshot);
    second_snapshot->generation = 2;
    (void)driver.install(second_snapshot);
    EXPECT_EQ(instances.configuration_cache_size(), 1u);

    iv::GraphBuilder second_root;
    requests[0].instance_id = "second";
    auto second = instances.configure_and_embed(
        second_snapshot, second_root, requests);
    ASSERT_TRUE(second.diagnostics.empty());
    EXPECT_EQ(counted_calls.load(), 1);
    EXPECT_EQ(
        second.placements.at("second").configured,
        first.placements.at("first").configured);
}

TEST(NodeInstancesConfiguration, SnapshotPublicationInvalidatesChangedNestedClosure)
{
    nested_parent_calls = 0;
    nested_child_calls = 0;
    auto first_snapshot = make_snapshot(1, {
        DefinitionSpec{
            .id = nested_parent_id,
            .module_build = &nested_parent_module,
            .signature = &int_signature_callback,
        },
        DefinitionSpec{
            .id = nested_child_id,
            .module_build = &nested_child_module,
            .signature = &int_signature_callback,
        },
    });
    iv::NodeInstances instances;
    iv::test::NodeInstancesDriver driver {instances};
    (void)driver.install(first_snapshot);
    int value = 4;
    std::array arguments{int_argument(value)};
    std::array requests{request("first", nested_parent_id, arguments)};
    iv::GraphBuilder first_root;
    auto first = instances.configure_and_embed(
        first_snapshot, first_root, requests);
    ASSERT_TRUE(first.diagnostics.empty());
    ASSERT_EQ(instances.configuration_cache_size(), 2u);

    auto second_snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>(
        *first_snapshot);
    second_snapshot->generation = 2;
    second_snapshot->by_id.at(nested_child_id).version = 2;
    (void)driver.install(second_snapshot);
    EXPECT_EQ(instances.configuration_cache_size(), 0u);

    iv::GraphBuilder second_root;
    requests[0].instance_id = "second";
    auto second = instances.configure_and_embed(
        second_snapshot, second_root, requests);
    ASSERT_TRUE(second.diagnostics.empty());
    EXPECT_EQ(nested_parent_calls.load(), 2);
    EXPECT_EQ(nested_child_calls.load(), 2);
    EXPECT_NE(
        second.placements.at("second").configured,
        first.placements.at("first").configured);
}


TEST(NodeInstancesConfiguration, OlderPinnedBatchDoesNotRepopulateCacheAfterNewerSnapshotInvalidation)
{
    counted_calls = 0;
    auto older_snapshot = make_snapshot(1, {counted_spec()});
    auto newer_snapshot = make_snapshot(2, {counted_spec()});
    iv::NodeInstances instances;
    iv::test::NodeInstancesDriver driver {instances};
    (void)driver.install(newer_snapshot);

    int value = 13;
    std::array arguments{int_argument(value)};
    std::array older_requests{request("older", counted_id, arguments)};
    iv::GraphBuilder older_root;
    auto older = instances.configure_and_embed(
        older_snapshot, older_root, older_requests);

    ASSERT_TRUE(older.diagnostics.empty());
    ASSERT_EQ(older.placements.size(), 1u);
    EXPECT_EQ(older.placements.at("older").configured->definitions_generation, 1u);
    EXPECT_EQ(instances.configuration_cache_size(), 0u);

    std::array newer_requests{request("newer", counted_id, arguments)};
    iv::GraphBuilder newer_root;
    auto newer = instances.configure_and_embed(
        newer_snapshot, newer_root, newer_requests);

    ASSERT_TRUE(newer.diagnostics.empty());
    ASSERT_EQ(newer.placements.size(), 1u);
    EXPECT_EQ(newer.placements.at("newer").configured->definitions_generation, 2u);
    EXPECT_EQ(instances.configuration_cache_size(), 1u);
    EXPECT_EQ(counted_calls.load(), 2);
}

TEST(NodeInstancesConfiguration, ProviderGenerationOutlivesRetainedTypedValueDestruction)
{
    value_destroyed_after_provider_release = false;
    auto provider_lifetime = std::make_shared<int>(1);
    provider_lifetime_weak = provider_lifetime;
    auto snapshot = make_snapshot(1, {
        DefinitionSpec{
            .id = counted_id,
            .module_build = &counted_module,
            .signature = &lifetime_checked_int_signature_callback,
        },
    }, provider_lifetime);
    provider_lifetime.reset();

    {
        iv::NodeInstances instances;
        iv::GraphBuilder root;
        int value = 21;
        std::array arguments{int_argument(value)};
        std::array requests{request("lifetime", counted_id, arguments)};
        auto result = instances.configure_and_embed(snapshot, root, requests);

        ASSERT_TRUE(result.diagnostics.empty());
        ASSERT_EQ(result.placements.size(), 1u);
        ASSERT_FALSE(provider_lifetime_weak.expired());
        snapshot.reset();
        EXPECT_FALSE(provider_lifetime_weak.expired());
    }

    EXPECT_FALSE(value_destroyed_after_provider_release.load());
    EXPECT_TRUE(provider_lifetime_weak.expired());
    provider_lifetime_weak.reset();
}

TEST(NodeInstancesConfiguration, LeafDefinitionsUseTheSameConfigurationPath)
{
    leaf_calls = 0;
    auto snapshot = make_snapshot(1, {
        DefinitionSpec{
            .id = leaf_id,
            .kind = PackageDefinitionKind::node,
            .node_build = &leaf_node,
            .signature = &zero_signature_callback,
        },
    });
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    std::array requests{request("leaf", leaf_id)};

    auto result = instances.configure_and_embed(snapshot, root, requests);

    ASSERT_TRUE(result.diagnostics.empty());
    ASSERT_EQ(result.placements.size(), 1u);
    EXPECT_EQ(leaf_calls.load(), 1);
    EXPECT_EQ(instances.configuration_cache_size(), 1u);
    EXPECT_EQ(result.placements.at("leaf").configured->definition_id, leaf_id);
}

namespace {
using Pass = iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>;

constexpr char annotated_id[] = "iv.test.node_instances.annotated";
constexpr char parent_scope_id[] = "iv.test.node_instances.parent_scope";
constexpr char failing_id[] = "iv.test.node_instances.failing";
constexpr char receiver_id[] = "iv.test.node_instances.receiver";

std::atomic<int> annotated_calls{0};

// Two calls of one leaf declaration: one miss, then one hit of the same
// leaf invocation, both grouped under the declaration `osc`.
void annotated_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    ++annotated_calls;
    for (int member = 0; member < 2; ++member) {
        auto osc = iv::details::configure_package_definition(graph, leaf_id, {});
        osc._annotate_source_info(
            "osc", "/tmp/iv-node-instances-configuration-tests/module.cpp", 1, 2);
    }
    graph.outputs();
}

void parent_scope_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    auto child = iv::details::configure_package_definition(graph, annotated_id, {});
    child._annotate_source_info(
        "child", "/tmp/iv-node-instances-configuration-tests/module.cpp", 3, 4);
    graph.outputs();
}

void failing_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    (void)iv::details::configure_package_definition(graph, leaf_id, {});
    (void)iv::details::configure_concrete_node<Pass>(graph);
    throw std::runtime_error("module construction failed");
}

// Wires the project node it receives as a live NodeRef of the root session.
void receiver_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument> arguments)
{
    using namespace iv;
    auto const& target = *static_cast<NodeRef const*>(arguments[0].data);
    auto pass = details::configure_concrete_node<Pass>(graph);
    pass(target);
    graph.outputs("out"_P = pass);
}

iv::details::ConfigurationExpressionTypeSchema const node_reference_schema{
    .kind = iv::details::ConfigurationExpressionTypeKind::node_reference,
    .size = sizeof(void*),
    .alignment = alignof(void*),
};
constexpr char node_reference_nominal[] = "test:NodeRef";
constexpr char node_reference_fingerprint[] = "test:NodeRef:v1";
constexpr char node_reference_display[] = "NodeRef";
ConfigurationTypeIdentity const node_reference_identity{
    .nominal_id = node_reference_nominal,
    .nominal_id_size = sizeof(node_reference_nominal) - 1,
    .definition_fingerprint = node_reference_fingerprint,
    .definition_fingerprint_size = sizeof(node_reference_fingerprint) - 1,
    .display_name = node_reference_display,
    .display_name_size = sizeof(node_reference_display) - 1,
    .expression_schema = &node_reference_schema,
};
ConfigurationTypeIdentity const* const node_reference_types[]{&node_reference_identity};
ConfigurationValueOperations const* const node_reference_operations[]{nullptr};
RegisteredSignature const node_reference_signature{
    .parameter_types = node_reference_types,
    .parameter_operations = node_reference_operations,
    .required_argument_count = 1,
    .argument_count = 1,
};
RegisteredSignature const* node_reference_signature_callback()
{
    return &node_reference_signature;
}

DefinitionSpec leaf_spec()
{
    return {
        .id = leaf_id,
        .kind = PackageDefinitionKind::node,
        .node_build = &leaf_node,
        .signature = &zero_signature_callback,
    };
}

DefinitionSpec annotated_spec()
{
    return {
        .id = annotated_id,
        .module_build = &annotated_module,
        .signature = &zero_signature_callback,
    };
}

// The bundle kinds and qualifier-relative virtual IDs of one placed scope.
struct PlacedScope {
    std::vector<std::string> bundle_kinds{};
    std::vector<std::string> virtual_ids{};
    bool operator==(PlacedScope const&) const = default;
};

PlacedScope placed_scope(
    iv::ConfiguredGraph const& graph, iv::NodeBundleHandle root,
    std::string_view qualifier)
{
    auto const info = graph.node_bundles.subgraph_info(root);
    auto const begin = info.child_begin;
    auto const end = info.child_begin + info.child_count;
    PlacedScope result;
    for (auto handle = begin; handle < end; ++handle) {
        auto const& bundle = graph.node_bundles.bundle(handle);
        result.bundle_kinds.push_back(
            bundle.is_concrete() ? "concrete"
            : bundle.is_tiled() ? "tiled"
            : bundle.is_boundary() ? "boundary"
            : "subgraph");
    }
    for (auto const& record : graph.virtual_nodes.records()) {
        if (record.node_bundle_handles.empty()) continue;
        auto const member = record.node_bundle_handles.front();
        if (member < begin || member >= end) continue;
        auto id = record.id;
        auto const at = id.rfind(qualifier);
        EXPECT_NE(at, std::string::npos) << id;
        if (at != std::string::npos) id.erase(at, qualifier.size());
        result.virtual_ids.push_back(std::move(id));
    }
    return result;
}

iv::NodeInstanceConfigurationRequest source_request(
    std::string instance_id,
    std::string_view definition_id,
    std::string source)
{
    return {
        .instance_id = std::move(instance_id),
        .definition_id = std::string(definition_id),
        .configuration_source = std::move(source),
    };
}
} // namespace

TEST(NodeInstancesConfiguration, CacheHitPlacementMatchesTheMissConstruction)
{
    annotated_calls = 0;
    leaf_calls = 0;
    auto snapshot = make_snapshot(1, {leaf_spec(), annotated_spec()});
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    std::array requests{request("first", annotated_id), request("second", annotated_id)};

    auto result = instances.configure_and_embed(snapshot, root, requests);

    ASSERT_TRUE(result.diagnostics.empty()) << result.diagnostics.front().message;
    EXPECT_EQ(annotated_calls.load(), 1);
    EXPECT_EQ(leaf_calls.load(), 1);
    EXPECT_EQ(instances.configuration_cache_size(), 2u);
    auto const& first = result.placements.at("first");
    auto const& second = result.placements.at("second");
    EXPECT_EQ(first.configured, second.configured);
    root.outputs();
    auto const graph = std::move(root).finish();
    auto const constructed = placed_scope(graph, first.root, "@&first");
    auto const imported = placed_scope(graph, second.root, "@&second");
    EXPECT_FALSE(constructed.virtual_ids.empty());
    EXPECT_EQ(constructed, imported);
}

TEST(NodeInstancesConfiguration, ProjectPathsResolveIntoConstructedAndImportedModules)
{
    auto snapshot = make_snapshot(1, {
        leaf_spec(),
        annotated_spec(),
        DefinitionSpec{
            .id = parent_scope_id,
            .module_build = &parent_scope_module,
            .signature = &zero_signature_callback,
        },
    });
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    std::array requests{
        request("constructed", parent_scope_id),
        request("imported", parent_scope_id),
    };
    auto result = instances.configure_and_embed(snapshot, root, requests);
    ASSERT_TRUE(result.diagnostics.empty()) << result.diagnostics.front().message;
    root.outputs();
    auto const graph = std::move(root).finish();

    std::array<iv::ProjectNodePathSelector, 4> const path{
        iv::ProjectVirtualNodeSelector{.source_identity = "child"},
        iv::ProjectVirtualMemberSelector{.index = 0},
        iv::ProjectVirtualNodeSelector{.source_identity = "osc"},
        iv::ProjectVirtualMemberSelector{.index = 1},
    };
    for (auto const* instance_id : {"constructed", "imported"}) {
        auto const matches = iv::resolve_project_node_path(
            graph, result.placements, instance_id, path);
        ASSERT_EQ(matches.size(), 1u) << instance_id;
        EXPECT_EQ(matches.front().kind, iv::ProjectNodePathMatch::Kind::bundle);
        EXPECT_TRUE(graph.node_bundles.bundle(matches.front().bundle).is_concrete());
    }
}

TEST(NodeInstancesConfiguration, ConstructionFailureStopsTheBatchAndFailsTheSession)
{
    counted_calls = 0;
    auto snapshot = make_snapshot(1, {
        counted_spec(),
        leaf_spec(),
        DefinitionSpec{
            .id = failing_id,
            .module_build = &failing_module,
            .signature = &zero_signature_callback,
        },
    });
    int value = 3;
    std::array good_arguments{int_argument(value)};
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    std::array batch{
        request("bad", failing_id),
        request("good", counted_id, good_arguments),
    };

    auto result = instances.configure_and_embed(snapshot, root, batch);

    ASSERT_EQ(result.diagnostics.size(), 1u);
    auto const& diagnostic = result.diagnostics.front();
    EXPECT_EQ(diagnostic.instance_id, "bad");
    // The message, then one context line per boundary it crossed.
    EXPECT_NE(diagnostic.message.find("module construction failed"), std::string::npos)
        << diagnostic.message;
    EXPECT_NE(diagnostic.message.find(failing_id), std::string::npos) << diagnostic.message;
    EXPECT_NE(diagnostic.message.find("project node instance 'bad'"), std::string::npos)
        << diagnostic.message;
    EXPECT_FALSE(diagnostic.stacktrace.empty());
    // Construction stopped at the first failure.
    EXPECT_TRUE(result.placements.empty());
    EXPECT_EQ(counted_calls.load(), 0);
    // The failed session refuses further construction and cannot finish.
    EXPECT_THROW((void)iv::details::configure_concrete_node<Pass>(root), std::logic_error);
    EXPECT_THROW((void)std::move(root).finish(), std::logic_error);
}

TEST(NodeInstancesConfiguration, ProjectNodeReferenceMaterializesAsALiveRefOfTheRootSession)
{
    auto snapshot = make_snapshot(1, {
        leaf_spec(),
        DefinitionSpec{
            .id = receiver_id,
            .module_build = &receiver_module,
            .signature = &node_reference_signature_callback,
        },
    });
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    // The receiver sorts before the node it references, so realization must
    // follow the reference rather than the request order.
    std::string const receiver_instance = "00000000-0000-4000-8000-000000000000";
    std::string const source_instance = "11111111-1111-4111-8111-111111111111";
    std::array requests{
        source_request(receiver_instance, receiver_id, "&" + source_instance),
        request(source_instance, leaf_id),
    };

    auto result = instances.configure_and_embed(snapshot, root, requests);

    ASSERT_TRUE(result.diagnostics.empty()) << result.diagnostics.front().message;
    ASSERT_EQ(result.placements.size(), 2u);
    // The leaf is reusable; the invocation holding a live ref is not.
    EXPECT_EQ(instances.configuration_cache_size(), 1u);
    EXPECT_FALSE(result.placements.at(receiver_instance).configured->graph);
    auto const source_root = result.placements.at(source_instance).root;
    auto const graph = root.finish();
    auto const connections = graph.connections.configured_sample_connections();
    EXPECT_TRUE(std::ranges::any_of(connections, [&](auto const& connection) {
        return std::ranges::any_of(connection.source_channels, [&](auto const& source) {
            return source.channel.bundle == source_root;
        });
    }));
}

TEST(NodeInstancesConfiguration, ProjectNodeReferenceCycleIsDiagnosed)
{
    auto snapshot = make_snapshot(1, {
        DefinitionSpec{
            .id = receiver_id,
            .module_build = &receiver_module,
            .signature = &node_reference_signature_callback,
        },
    });
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    std::string const first = "00000000-0000-4000-8000-000000000000";
    std::string const second = "11111111-1111-4111-8111-111111111111";
    std::array requests{
        source_request(first, receiver_id, "&" + second),
        source_request(second, receiver_id, "&" + first),
    };

    auto result = instances.configure_and_embed(snapshot, root, requests);

    EXPECT_TRUE(result.placements.empty());
    ASSERT_EQ(result.diagnostics.size(), 2u);
    for (auto const& diagnostic : result.diagnostics) {
        EXPECT_NE(diagnostic.message.find("cycle"), std::string::npos);
    }
}

namespace {
constexpr char closure_owner_id[] = "iv.test.node_instances.closure_owner";
constexpr char closure_bank_id[] = "iv.test.node_instances.closure_bank";
constexpr char closure_nominal[] = "test:SubgraphClosure";
constexpr char closure_fingerprint[] = "test:SubgraphClosure:v1";
constexpr char closure_display[] = "SubgraphClosure";

iv::details::ConfigurationExpressionTypeSchema const closure_schema{
    .kind = iv::details::ConfigurationExpressionTypeKind::unsupported,
    .size = sizeof(iv::SubgraphClosure),
    .alignment = alignof(iv::SubgraphClosure),
};
ConfigurationTypeIdentity const closure_identity{
    .nominal_id = closure_nominal,
    .nominal_id_size = sizeof(closure_nominal) - 1,
    .definition_fingerprint = closure_fingerprint,
    .definition_fingerprint_size = sizeof(closure_fingerprint) - 1,
    .display_name = closure_display,
    .display_name_size = sizeof(closure_display) - 1,
    .expression_schema = &closure_schema,
};
ConfigurationTypeIdentity const* const closure_types[]{&closure_identity};
ConfigurationValueOperations const* const closure_operations[]{
    iv::details::configuration_value_operations<iv::SubgraphClosure>(),
};
RegisteredSignature const closure_signature{
    .parameter_types = closure_types,
    .parameter_operations = closure_operations,
    .required_argument_count = 1,
    .argument_count = 1,
};
RegisteredSignature const* closure_signature_callback() { return &closure_signature; }

std::atomic<int> closure_owner_calls{0};
std::atomic<int> closure_bank_calls{0};

// Invokes the closure it receives three times inside its own module scope.
void closure_bank_module(
    iv::GraphBuilder& graph, std::span<ConfigurationArgument> arguments)
{
    ++closure_bank_calls;
    auto& voice = *static_cast<iv::SubgraphClosure*>(arguments[0].data);
    for (int index = 0; index < 3; ++index) (void)graph.subgraph(voice);
    graph.outputs();
}

// Owns a source and passes a closure capturing it to a nested module.
void closure_owner_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    using namespace iv;
    ++closure_owner_calls;
    auto const source = details::configure_package_definition(graph, leaf_id, {});
    SubgraphClosure voice = [&](SubgraphBuilder& boundary) {
        auto pass = details::configure_concrete_node<Pass>(graph);
        pass(source);
        boundary.outputs("out"_P = pass);
    };
    ConfigurationArgument argument{
        .data = &voice,
        .type = &closure_identity,
    };
    std::array arguments{argument};
    (void)details::configure_package_definition(
        graph, closure_bank_id, std::span<ConfigurationArgument>(arguments));
    graph.outputs();
}
} // namespace

TEST(NodeInstancesConfiguration, SubgraphClosureArgumentIsInvokedInADeeperModule)
{
    closure_owner_calls = 0;
    closure_bank_calls = 0;
    auto snapshot = make_snapshot(1, {
        leaf_spec(),
        DefinitionSpec{
            .id = closure_owner_id,
            .module_build = &closure_owner_module,
            .signature = &zero_signature_callback,
        },
        DefinitionSpec{
            .id = closure_bank_id,
            .module_build = &closure_bank_module,
            .signature = &closure_signature_callback,
        },
    });
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    std::array requests{
        request("first", closure_owner_id),
        request("second", closure_owner_id),
    };

    auto result = instances.configure_and_embed(snapshot, root, requests);

    ASSERT_TRUE(result.diagnostics.empty()) << result.diagnostics.front().message;
    // The bank receives a closure without equality and is never reused; the
    // owner contains both ends of every captured connection, so it is.
    EXPECT_EQ(closure_owner_calls.load(), 1);
    EXPECT_EQ(closure_bank_calls.load(), 1);
    EXPECT_EQ(instances.configuration_cache_size(), 2u);
    root.outputs();
    auto const graph = std::move(root).finish();
    auto const& first = result.placements.at("first");
    auto const& second = result.placements.at("second");
    EXPECT_EQ(
        placed_scope(graph, first.root, "@&first"),
        placed_scope(graph, second.root, "@&second"));
    // Every voice is an ordinary connection from the owner's source.
    for (auto const* placement : {&first, &second}) {
        auto const info = graph.node_bundles.subgraph_info(placement->root);
        std::size_t voices = 0;
        for (auto const& connection : graph.connections.configured_sample_connections()) {
            auto const target = connection.target_channels.front().bundle;
            auto const source = connection.source_channels.front().channel.bundle;
            if (target >= info.child_begin && target < info.child_begin + info.child_count
                && source >= info.child_begin && source < info.child_begin + info.child_count
                && graph.node_bundles.bundle(source).is_concrete()
                && graph.node_bundles.bundle(target).is_concrete()) {
                ++voices;
            }
        }
        EXPECT_EQ(voices, 3u);
    }
}

namespace {
constexpr char recovering_id[] = "iv.test.node_instances.recovering";
constexpr char tiled_failing_id[] = "iv.test.node_instances.tiled_failing";
constexpr char tiled_mismatched_id[] = "iv.test.node_instances.tiled_mismatched";

std::atomic<int> recovering_calls{0};
std::atomic<int> tiled_calls{0};

// Catches a failed nested call and tries to continue building.
void recovering_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    ++recovering_calls;
    try {
        (void)iv::details::configure_package_definition(graph, failing_id, {});
    } catch (std::exception const&) {
    }
    (void)iv::details::configure_package_definition(graph, annotated_id, {});
    graph.outputs();
}

// The second member of every stereo request fails.
void tiled_failing_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    using namespace iv;
    bool const second = tiled_calls++ % 2 == 1;
    auto value = details::configure_concrete_node<Constant>(graph, Sample{0.25f});
    if (second) throw std::runtime_error("second tiled member failed");
    graph.outputs("out"_P = value);
}

// The second member of every stereo request declares a different interface.
void tiled_mismatched_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    using namespace iv;
    bool const second = tiled_calls++ % 2 == 1;
    auto value = details::configure_concrete_node<Constant>(graph, Sample{0.25f});
    if (second) {
        auto extra = details::configure_concrete_node<Constant>(graph, Sample{0.5f});
        graph.outputs("out"_P = value, "other"_P = extra);
        return;
    }
    graph.outputs("out"_P = value);
}

DefinitionSpec failing_spec()
{
    return {
        .id = failing_id,
        .module_build = &failing_module,
        .signature = &zero_signature_callback,
    };
}

DefinitionSpec receiver_spec()
{
    return {
        .id = receiver_id,
        .module_build = &receiver_module,
        .signature = &node_reference_signature_callback,
    };
}

iv::ChannelLayout stereo_layout()
{
    return {
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
}

bool reads_from(iv::ConfiguredGraph const& graph, iv::NodeBundleHandle source)
{
    return std::ranges::any_of(
        graph.connections.configured_sample_connections(), [&](auto const& connection) {
            return std::ranges::any_of(connection.source_channels, [&](auto const& channel) {
                return channel.channel.bundle == source;
            });
        });
}

std::string const uuid_a = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
std::string const uuid_b = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
std::string const uuid_c = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
std::string const uuid_d = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
} // namespace

TEST(NodeInstancesConfiguration, CaughtNestedFailureStillFailsTheBatch)
{
    recovering_calls = 0;
    auto snapshot = make_snapshot(1, {
        leaf_spec(),
        annotated_spec(),
        failing_spec(),
        DefinitionSpec{
            .id = recovering_id,
            .module_build = &recovering_module,
            .signature = &zero_signature_callback,
        },
    });
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    std::array requests{request("recovering", recovering_id)};

    auto result = instances.configure_and_embed(snapshot, root, requests);

    // The module caught the nested failure, but its next builder call was
    // refused, so the batch reports the first failure.
    EXPECT_EQ(recovering_calls.load(), 1);
    EXPECT_TRUE(result.placements.empty());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_NE(result.diagnostics.front().message.find("module construction failed"),
              std::string::npos) << result.diagnostics.front().message;
    EXPECT_EQ(instances.configuration_cache_size(), 1u); // only the leaf
}

TEST(NodeInstancesConfiguration, TiledMemberFailureFailsTheBatch)
{
    tiled_calls = 0;
    counted_calls = 0;
    auto snapshot = make_snapshot(1, {
        counted_spec(),
        DefinitionSpec{
            .id = tiled_failing_id,
            .module_build = &tiled_failing_module,
            .signature = &zero_signature_callback,
        },
    });
    int value = 3;
    std::array good_arguments{int_argument(value)};
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    auto tiled = request("tiled", tiled_failing_id);
    tiled.tiled_layout = stereo_layout();
    std::array batch{tiled, request("good", counted_id, good_arguments)};

    auto result = instances.configure_and_embed(snapshot, root, batch);

    EXPECT_EQ(tiled_calls.load(), 2);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().instance_id, "tiled");
    EXPECT_TRUE(result.placements.empty());
    EXPECT_EQ(counted_calls.load(), 0);
    EXPECT_EQ(instances.configuration_cache_size(), 0u);
}

TEST(NodeInstancesConfiguration, TiledMembersWithDifferentInterfacesFailTheBatch)
{
    tiled_calls = 0;
    auto snapshot = make_snapshot(1, {
        DefinitionSpec{
            .id = tiled_mismatched_id,
            .module_build = &tiled_mismatched_module,
            .signature = &zero_signature_callback,
        },
    });
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    auto tiled = request("tiled", tiled_mismatched_id);
    tiled.tiled_layout = stereo_layout();
    std::array batch{tiled};

    auto result = instances.configure_and_embed(snapshot, root, batch);

    EXPECT_EQ(tiled_calls.load(), 2);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_TRUE(result.placements.empty());
    EXPECT_EQ(instances.configuration_cache_size(), 0u);
}

TEST(NodeInstancesConfiguration, ProjectNodeReferenceToAnUndeclaredInstanceFailsBeforeConstruction)
{
    auto snapshot = make_snapshot(1, {leaf_spec(), failing_spec(), receiver_spec()});
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    auto const empty = root.finish().node_bundles.size();
    std::array requests{
        // Would fail during construction, but construction never starts.
        request(uuid_a, failing_id),
        source_request(uuid_b, receiver_id, "&" + uuid_d),
    };

    auto result = instances.configure_and_embed(snapshot, root, requests);

    EXPECT_TRUE(result.placements.empty());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().instance_id, uuid_b);
    EXPECT_NE(result.diagnostics.front().message.find("does not declare"), std::string::npos)
        << result.diagnostics.front().message;
    EXPECT_EQ(root.finish().node_bundles.size(), empty);
}

TEST(NodeInstancesConfiguration, ProjectNodeReferenceChainFollowsReferencesNotRequestOrder)
{
    auto snapshot = make_snapshot(1, {leaf_spec(), receiver_spec()});
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    // c reads b, b reads a; the requests arrive in the opposite order.
    std::array requests{
        source_request(uuid_c, receiver_id, "&" + uuid_b),
        source_request(uuid_b, receiver_id, "&" + uuid_a),
        request(uuid_a, leaf_id),
    };

    auto result = instances.configure_and_embed(snapshot, root, requests);

    ASSERT_TRUE(result.diagnostics.empty()) << result.diagnostics.front().message;
    ASSERT_EQ(result.placements.size(), 3u);
    // Only the leaf is reusable; both receivers hold live refs.
    EXPECT_EQ(instances.configuration_cache_size(), 1u);
    EXPECT_FALSE(result.placements.at(uuid_b).configured->graph);
    EXPECT_FALSE(result.placements.at(uuid_c).configured->graph);
    auto const graph = root.finish();
    EXPECT_TRUE(reads_from(graph, result.placements.at(uuid_a).root));
}

TEST(NodeInstancesConfiguration, ProjectNodeSelfReferenceIsACycle)
{
    auto snapshot = make_snapshot(1, {receiver_spec()});
    iv::NodeInstances instances;
    iv::GraphBuilder root;
    std::array requests{source_request(uuid_a, receiver_id, "&" + uuid_a)};

    auto result = instances.configure_and_embed(snapshot, root, requests);

    EXPECT_TRUE(result.placements.empty());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_NE(result.diagnostics.front().message.find("cycle"), std::string::npos);
}

namespace {
iv::NodeInstancesProjectGraphRequest configure_interface_test(
    iv::NodeInstances& instances,
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> snapshot,
    std::vector<iv::ProjectNodeInstanceDeclaration> declarations,
    bool prepare = true)
{
    iv::GraphBuilder root;
    iv::NodeInstancesProjectGraphRequest transaction{
        .snapshot = std::move(snapshot),
        .root_builder = &root,
        .mutation = iv::NodeInstanceReplaceAllMutation{.instances = std::move(declarations)},
        .prepare_failure_interfaces = prepare,
    };
    in_event_source([&] { instances.handle_project_graph_transaction(transaction); });
    transaction.root_builder = nullptr;
    return transaction;
}
} // namespace

TEST(NodeInstancesConfiguration, FailureInterfacesKeepLiveConstructorReferencesInOneSession)
{
    leaf_calls = 0;
    iv::NodeInstances instances;
    auto transaction = configure_interface_test(instances,
        make_snapshot(1, {leaf_spec(), receiver_spec()}), {
            {.instance_id = uuid_c, .definition_id = receiver_id,
             .package_root = package_root_text, .configuration_source = "&" + uuid_b},
            {.instance_id = uuid_b, .definition_id = receiver_id,
             .package_root = package_root_text, .configuration_source = "&" + uuid_a},
            {.instance_id = uuid_a, .definition_id = leaf_id, .package_root = package_root_text},
            {.instance_id = uuid_d, .definition_id = "iv.test.missing", .package_root = package_root_text},
        });
    EXPECT_TRUE(transaction.result.placements.empty());
    ASSERT_TRUE(transaction.failure_interfaces);
    auto const& interfaces = transaction.failure_interfaces->interfaces;
    ASSERT_EQ(interfaces.size(), 3u);
    for (auto const& id : {uuid_a, uuid_b, uuid_c}) {
        EXPECT_EQ(interfaces.at(id).sample_outputs.size(), 1u);
    }
    EXPECT_EQ(leaf_calls.load(), 1);
}

TEST(NodeInstancesConfiguration, FailureInterfacesExcludeFailedConstructorAndItsDependents)
{
    iv::NodeInstances instances;
    auto transaction = configure_interface_test(instances,
        make_snapshot(1, {leaf_spec(), receiver_spec(), failing_spec()}), {
            {.instance_id = uuid_a, .definition_id = failing_id, .package_root = package_root_text},
            {.instance_id = uuid_b, .definition_id = receiver_id,
             .package_root = package_root_text, .configuration_source = "&" + uuid_a},
            {.instance_id = uuid_c, .definition_id = leaf_id, .package_root = package_root_text},
        });
    EXPECT_TRUE(transaction.result.placements.empty());
    ASSERT_TRUE(transaction.failure_interfaces);
    auto const& prepared = *transaction.failure_interfaces;
    ASSERT_EQ(prepared.interfaces.size(), 1u);
    EXPECT_EQ(prepared.interfaces.at(uuid_c).sample_outputs.size(), 1u);
    EXPECT_TRUE(std::ranges::any_of(prepared.diagnostics, [&](auto const& diagnostic) {
        return diagnostic.instance_id == uuid_b
            && diagnostic.message.find(uuid_a) != std::string::npos;
    }));
}

TEST(NodeInstancesConfiguration, SuccessfulBuildDoesNotPrepareFailureInterfaces)
{
    leaf_calls = 0;
    iv::NodeInstances instances;
    auto transaction = configure_interface_test(instances,
        make_snapshot(1, {leaf_spec()}), {
            {.instance_id = uuid_a, .definition_id = leaf_id, .package_root = package_root_text},
        });
    EXPECT_TRUE(transaction.result.diagnostics.empty());
    EXPECT_EQ(transaction.result.placements.size(), 1u);
    EXPECT_FALSE(transaction.failure_interfaces);
    EXPECT_EQ(leaf_calls.load(), 1);
}

TEST(NodeInstancesConfiguration, ProvisionalFailureCanLeaveInterfacePreparationDisabled)
{
    leaf_calls = 0;
    iv::NodeInstances instances;
    auto transaction = configure_interface_test(instances,
        make_snapshot(1, {leaf_spec()}), {
            {.instance_id = uuid_a, .definition_id = leaf_id, .package_root = package_root_text},
            {.instance_id = uuid_b, .definition_id = "iv.test.missing", .package_root = package_root_text},
        }, false);
    EXPECT_FALSE(transaction.result.diagnostics.empty());
    EXPECT_FALSE(transaction.failure_interfaces);
    EXPECT_EQ(leaf_calls.load(), 0);
}

TEST(NodeInstancesConfiguration, ProjectNodeReferenceMustSelectExactlyOneNode)
{
    auto snapshot = make_snapshot(1, {leaf_spec(), annotated_spec(), receiver_spec()});
    {
        iv::NodeInstances instances;
        iv::GraphBuilder root;
        std::array requests{
            request(uuid_a, annotated_id),
            // `osc` has two direct members.
            source_request(uuid_b, receiver_id, "&" + uuid_a + ".osc"),
        };
        auto result = instances.configure_and_embed(snapshot, root, requests);
        EXPECT_TRUE(result.placements.empty());
        ASSERT_EQ(result.diagnostics.size(), 1u);
        EXPECT_EQ(result.diagnostics.front().instance_id, uuid_b);
        EXPECT_NE(result.diagnostics.front().message.find("select one member"),
                  std::string::npos) << result.diagnostics.front().message;
    }

    iv::NodeInstances instances;
    iv::GraphBuilder root;
    std::array requests{
        request(uuid_a, annotated_id),
        source_request(uuid_c, receiver_id, "&" + uuid_a + ".osc[1]"),
    };
    auto result = instances.configure_and_embed(snapshot, root, requests);
    ASSERT_TRUE(result.diagnostics.empty()) << result.diagnostics.front().message;
    root.outputs();
    auto const graph = std::move(root).finish();
    std::array<iv::ProjectNodePathSelector, 2> const second_osc{
        iv::ProjectVirtualNodeSelector{.source_identity = "osc"},
        iv::ProjectVirtualMemberSelector{.index = 1},
    };
    auto const matches = iv::resolve_project_node_path(
        graph, result.placements, uuid_a, second_osc);
    ASSERT_EQ(matches.size(), 1u);
    ASSERT_EQ(matches.front().kind, iv::ProjectNodePathMatch::Kind::bundle);
    EXPECT_TRUE(reads_from(graph, matches.front().bundle));
}
