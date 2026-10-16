#include <intravenous/graph/builder.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/package_pipeline_types.h>

#include <gtest/gtest.h>

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

ConfigurationTypeIdentity const int_identity{
    .nominal_id = int_nominal,
    .nominal_id_size = sizeof(int_nominal) - 1,
    .definition_fingerprint = int_fingerprint,
    .definition_fingerprint_size = sizeof(int_fingerprint) - 1,
    .display_name = int_display,
    .display_name_size = sizeof(int_display) - 1,
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

TEST(NodeInstancesConfiguration, OneBadRequestDoesNotPoisonSiblingPlacement)
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

    ASSERT_EQ(result.placements.size(), 1u);
    EXPECT_TRUE(result.placements.contains("good"));
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().instance_id, "missing");
    EXPECT_EQ(counted_calls.load(), 1);
}

TEST(NodeInstancesConfiguration, DuplicateInstanceIdIsRejectedBeforeSecondConfiguration)
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

    ASSERT_EQ(result.placements.size(), 1u);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_NE(result.diagnostics.front().message.find("duplicate"), std::string::npos);
    EXPECT_EQ(counted_calls.load(), 1);
}

TEST(NodeInstancesConfiguration, SnapshotPublicationInvalidatesConservativeCache)
{
    counted_calls = 0;
    auto first_snapshot = make_snapshot(1, {counted_spec()});
    auto second_snapshot = make_snapshot(2, {counted_spec()});
    iv::NodeInstances instances;
    instances.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = first_snapshot});
    int value = 9;
    std::array arguments{int_argument(value)};
    std::array requests{request("first", counted_id, arguments)};
    iv::GraphBuilder first_root;
    auto first = instances.configure_and_embed(first_snapshot, first_root, requests);
    ASSERT_TRUE(first.diagnostics.empty());
    ASSERT_EQ(instances.configuration_cache_size(), 1u);

    instances.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = second_snapshot});
    EXPECT_EQ(instances.configuration_cache_size(), 0u);

    iv::GraphBuilder second_root;
    requests[0].instance_id = "second";
    auto second = instances.configure_and_embed(second_snapshot, second_root, requests);
    ASSERT_TRUE(second.diagnostics.empty());
    EXPECT_EQ(counted_calls.load(), 2);
    EXPECT_EQ(second.placements.at("second").configured->definitions_generation, 2u);
}


TEST(NodeInstancesConfiguration, OlderPinnedBatchDoesNotRepopulateCacheAfterNewerSnapshotInvalidation)
{
    counted_calls = 0;
    auto older_snapshot = make_snapshot(1, {counted_spec()});
    auto newer_snapshot = make_snapshot(2, {counted_spec()});
    iv::NodeInstances instances;
    instances.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = newer_snapshot});

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
