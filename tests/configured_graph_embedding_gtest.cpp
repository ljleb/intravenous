#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/configured_graph.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace {
using Pass = iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>;

iv::ConfiguredGraph make_nested_annotated_graph()
{
    using namespace iv;
    GraphBuilder graph;
    auto const input = graph.input<"in">();
    auto nested = graph.subgraph([&](SubgraphBuilder& boundary) {
        auto pass = details::configure_concrete_node<Pass>(graph);
        pass(input);
        pass._annotate_source_info(
            "shared-child-node", "/tmp/configured-child.cpp", 10, 20);
        boundary.outputs("out"_P = pass);
    }, "nested-child");
    graph.outputs("out"_P = nested);
    return std::move(graph).finish();
}

std::optional<iv::NodeBundleHandle> first_subgraph_handle(
    iv::ConfiguredGraph const& graph)
{
    std::optional<iv::NodeBundleHandle> result;
    iv::NodeBundleHandle handle = 0;
    graph.node_bundles.for_each_configured_bundle(
        [&](iv::ConfiguredNodeBundleView const& bundle) {
            if (!result && bundle.kind == iv::ConfiguredNodeBundleKind::subgraph) {
                result = handle;
            }
            ++handle;
        });
    return result;
}

iv::SampleOutputChannelId translate(
    iv::ConfiguredGraphEmbedding const& embedding,
    iv::SampleOutputChannelId value)
{
    value.bundle = embedding.node_bundle(value.bundle);
    return value;
}

iv::SampleInputChannelId translate(
    iv::ConfiguredGraphEmbedding const& embedding,
    iv::SampleInputChannelId value)
{
    value.bundle = embedding.node_bundle(value.bundle);
    return value;
}

void expect_connection_is_translation(
    iv::ConfiguredSampleConnection const& child,
    iv::ConfiguredSampleConnection const& parent,
    iv::ConfiguredGraphEmbedding const& embedding)
{
    EXPECT_EQ(parent.source_type, child.source_type);
    EXPECT_EQ(parent.target_type, child.target_type);
    ASSERT_EQ(parent.source_channels.size(), child.source_channels.size());
    ASSERT_EQ(parent.target_channels.size(), child.target_channels.size());
    for (std::size_t i = 0; i < child.source_channels.size(); ++i) {
        EXPECT_EQ(parent.source_channels[i], translate(embedding, child.source_channels[i]));
    }
    for (std::size_t i = 0; i < child.target_channels.size(); ++i) {
        EXPECT_EQ(parent.target_channels[i], translate(embedding, child.target_channels[i]));
    }
}

TEST(ConfiguredGraphEmbedding, RepeatedEmbeddingKeepsFrozenHandlesLocalAndReturnsIndependentTranslations)
{
    auto child = make_nested_annotated_graph();
    ASSERT_EQ(child.virtual_nodes.records().size(), 1u);
    auto const child_virtual_bundle =
        child.virtual_nodes.records().front().node_bundle_handles.front();
    auto const child_connections = child.connections.configured_sample_connections();
    ASSERT_FALSE(child_connections.empty());
    auto const child_first_source_bundle = child_connections.front().source_channels.front().bundle;

    iv::GraphBuilder parent;
    auto const first = parent.embed(child, "instance-a");
    auto const second = parent.embed(child, "instance-b");
    parent.outputs();
    auto configured = std::move(parent).finish();

    EXPECT_EQ(first.node_bundles.size(), child.node_bundles.size());
    EXPECT_EQ(second.node_bundles.size(), child.node_bundles.size());
    ASSERT_EQ(first.virtual_nodes.size(), 1u);
    ASSERT_EQ(second.virtual_nodes.size(), 1u);
    EXPECT_NE(first.root_scope, second.root_scope);
    EXPECT_NE(first.node_bundle(0), second.node_bundle(0));
    EXPECT_NE(first.virtual_node(0), second.virtual_node(0));

    // Embedding is a pure import of the child: local identities in the frozen
    // cacheable graph remain untouched even after multiple placements.
    EXPECT_EQ(
        child.virtual_nodes.records().front().node_bundle_handles.front(),
        child_virtual_bundle);
    EXPECT_EQ(
        child.connections.configured_sample_connections().front().source_channels.front().bundle,
        child_first_source_bundle);

    auto const& parent_virtuals = configured.virtual_nodes.records();
    ASSERT_EQ(parent_virtuals.size(), 2u);
    EXPECT_EQ(parent_virtuals[first.virtual_node(0)].source_identity, "shared-child-node");
    EXPECT_EQ(parent_virtuals[second.virtual_node(0)].source_identity, "shared-child-node");
    EXPECT_NE(
        parent_virtuals[first.virtual_node(0)].node_bundle_handles.front(),
        parent_virtuals[second.virtual_node(0)].node_bundle_handles.front());
}

TEST(ConfiguredGraphEmbedding, RemapsConnectionsAndNestedScopesThroughExplicitTranslation)
{
    auto child = make_nested_annotated_graph();
    auto const local_nested_scope = first_subgraph_handle(child);
    ASSERT_TRUE(local_nested_scope.has_value());

    iv::GraphBuilder parent;
    auto const embedding = parent.embed(child, "cached-instance");
    parent.outputs();
    auto configured = std::move(parent).finish();

    ASSERT_TRUE(embedding.root_scope < configured.node_bundles.size());
    EXPECT_TRUE(configured.node_bundles.bundle(embedding.root_scope).is_subgraph());
    auto const translated_nested = embedding.scope(*local_nested_scope);
    ASSERT_TRUE(translated_nested < configured.node_bundles.size());
    EXPECT_TRUE(configured.node_bundles.bundle(translated_nested).is_subgraph());
    EXPECT_NE(translated_nested, embedding.root_scope);

    auto const child_connections = child.connections.configured_sample_connections();
    auto const parent_connections = configured.connections.configured_sample_connections();
    ASSERT_EQ(parent_connections.size(), child_connections.size());
    for (std::size_t i = 0; i < child_connections.size(); ++i) {
        expect_connection_is_translation(
            child_connections[i], parent_connections[i], embedding);
    }
}

TEST(ConfiguredGraphEmbedding, PreservesScopedVirtualIdentityInsteadOfFlatteningEqualChildren)
{
    auto child = make_nested_annotated_graph();

    iv::GraphBuilder parent;
    auto const first = parent.embed(child, "left");
    auto const second = parent.embed(child, "right");
    parent.outputs();
    auto configured = std::move(parent).finish();

    auto const& records = configured.virtual_nodes.records();
    ASSERT_EQ(records.size(), 2u);
    auto const& left = records[first.virtual_node(0)];
    auto const& right = records[second.virtual_node(0)];

    // Source/type identity stays stable, while the introspection ID is
    // scope-qualified so lowering cannot flatten the two child placements.
    EXPECT_NE(left.id, right.id);
    EXPECT_TRUE(left.id.starts_with("shared-child-node#type:"));
    EXPECT_TRUE(right.id.starts_with("shared-child-node#type:"));
    EXPECT_EQ(left.source_identity, right.source_identity);
    EXPECT_EQ(left.type_identity, right.type_identity);
    ASSERT_EQ(left.node_bundle_handles.size(), 1u);
    ASSERT_EQ(right.node_bundle_handles.size(), 1u);
    EXPECT_NE(left.node_bundle_handles.front(), right.node_bundle_handles.front());

    auto const& left_inverse = configured.node_bundles
        .bundle(left.node_bundle_handles.front()).virtual_node_handles();
    auto const& right_inverse = configured.node_bundles
        .bundle(right.node_bundle_handles.front()).virtual_node_handles();
    EXPECT_TRUE(std::ranges::contains(left_inverse, first.virtual_node(0)));
    EXPECT_TRUE(std::ranges::contains(right_inverse, second.virtual_node(0)));
    EXPECT_FALSE(std::ranges::contains(left_inverse, second.virtual_node(0)));
    EXPECT_FALSE(std::ranges::contains(right_inverse, first.virtual_node(0)));
}

TEST(ConfiguredGraphEmbedding, RejectsUnfinishedFrozenGraphWithoutMutatingParent)
{
    iv::GraphBuilder unfinished;
    auto frozen = std::move(unfinished).finish();

    iv::GraphBuilder parent;
    EXPECT_THROW(parent.embed(frozen), std::logic_error);
    parent.outputs();
    auto configured = std::move(parent).finish();
    EXPECT_EQ(configured.node_bundles.size(), 1u); // root boundary only
}

TEST(ConfiguredGraphEmbedding, TranslationRejectsForeignLocalHandles)
{
    auto child = make_nested_annotated_graph();
    iv::GraphBuilder parent;
    auto const embedding = parent.embed(child);

    EXPECT_THROW((void)embedding.node_bundle(child.node_bundles.size()), std::out_of_range);
    EXPECT_THROW((void)embedding.virtual_node(child.virtual_nodes.records().size()), std::out_of_range);
}
} // namespace
