#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/runtime/graph_connections.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
iv::ConfiguredGraph make_sample_passthrough(iv::ChannelTypeId type = iv::ChannelTypeId::mono)
{
    using namespace iv;
    GraphBuilder graph;
    if (type == ChannelTypeId::stereo) {
        auto input = graph.input<"main", stereo>();
        graph.outputs("main"_P = input);
    } else {
        auto input = graph.input<"main">();
        graph.outputs("main"_P = input);
    }
    return std::move(graph).finish();
}

iv::ConfiguredGraph make_sample_sink(iv::ChannelTypeId type = iv::ChannelTypeId::mono)
{
    using namespace iv;
    GraphBuilder graph;
    if (type == ChannelTypeId::stereo) {
        (void)graph.input<"main", stereo>();
    } else {
        (void)graph.input<"main">();
    }
    graph.outputs();
    return std::move(graph).finish();
}

iv::ConfiguredGraph make_event_passthrough()
{
    using namespace iv;
    GraphBuilder graph;
    auto trigger = graph.event_input<"trigger">(EventTypeId::trigger);
    graph.event_outputs("trigger"_P = trigger);
    graph.outputs();
    return std::move(graph).finish();
}

iv::ConfiguredGraph make_event_sink()
{
    using namespace iv;
    GraphBuilder graph;
    (void)graph.event_input<"trigger">(EventTypeId::trigger);
    graph.event_outputs();
    graph.outputs();
    return std::move(graph).finish();
}

iv::ConfiguredGraph make_virtual_source()
{
    using namespace iv;
    using Pass = Sum<mono, SampleStreamLayout::planar, 1>;
    GraphBuilder graph;
    auto input = graph.input<"in">();
    auto first = details::configure_concrete_node<Pass>(graph);
    first(input);
    first._annotate_source_info("virtual-source", "/tmp/virtual.cpp", 1, 2);
    auto second = details::configure_concrete_node<Pass>(graph);
    second(input);
    second._annotate_source_info("virtual-source", "/tmp/virtual.cpp", 3, 4);
    graph.outputs("main"_P = first);
    return std::move(graph).finish();
}

struct FrozenRootFixture {
    iv::ConfiguredGraph graph{};
    iv::NodeBundleHandle local_root = 0;
};

FrozenRootFixture make_tiled_source()
{
    using namespace iv;
    using Pass = Sum<mono, SampleStreamLayout::planar, 1>;
    GraphBuilder graph;
    auto tiled = details::configure_concrete_tiled_node<Pass, stereo>(graph);
    auto const local_root = tiled.node_bundle_handle();
    graph.outputs("main"_P = tiled);
    return {std::move(graph).finish(), local_root};
}

iv::ConfiguredGraph make_nested_subgraph_source()
{
    using namespace iv;
    using Pass = Sum<mono, SampleStreamLayout::planar, 1>;
    GraphBuilder graph;
    auto input = graph.input<"in">();
    auto nested = graph.subgraph([&](SubgraphBuilder& boundary) {
        auto pass = details::configure_concrete_node<Pass>(graph);
        pass(input);
        boundary.outputs("main"_P = pass);
    }, "inner");
    graph.outputs("main"_P = nested);
    return std::move(graph).finish();
}

iv::NodeInstancePlacement embed_as_instance(
    iv::GraphBuilder& root,
    iv::ConfiguredGraph const& graph,
    std::string_view kind)
{
    auto embedding = root.embed(graph, kind);
    auto const instance_root = embedding.root_scope;
    return iv::NodeInstancePlacement{
        .embedding = std::move(embedding),
        .root = instance_root,
    };
}

iv::ProjectNodePortMatcher named_matcher(
    std::string instance_id,
    std::string port_name,
    std::optional<std::size_t> channel = std::nullopt)
{
    return iv::ProjectNodePortMatcher{
        .instance_id = std::move(instance_id),
        .port = iv::ProjectPortMatcher{
            .name = std::move(port_name),
            .channel = channel,
        },
    };
}

iv::GraphConnectionsBatchResult apply(
    iv::GraphConnections& connections,
    iv::GraphBuilder& root,
    std::unordered_map<std::string, iv::NodeInstancePlacement> const& placements,
    iv::GraphConnectionsMutation mutation = std::monostate{})
{
    iv::GraphConnectionsProjectGraphRequest request{
        .root_builder = &root,
        .placements = &placements,
        .mutation = std::move(mutation),
    };
    connections.handle_project_graph_transaction(request);
    EXPECT_TRUE(request.handled);
    return std::move(request.result);
}
} // namespace

TEST(GraphConnections, AppliesNamedSampleConnectionAfterCompleteInstanceEmbedding)
{
    auto source_graph = make_sample_passthrough();
    auto sink_graph = make_sample_sink();
    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    placements.emplace("source", embed_as_instance(root, source_graph, "source"));
    placements.emplace("sink", embed_as_instance(root, sink_graph, "sink"));
    auto const before = root.finish().connections.configured_sample_connections().size();

    iv::GraphConnections connections;
    auto result = apply(connections, root, placements, iv::GraphConnectionUpsertMutation{
        .connection = iv::ProjectSampleConnection{
            .connection_id = "sample:main",
            .source_type = iv::ChannelTypeId::mono,
            .outputs = {named_matcher("source", "main")},
            .target_type = iv::ChannelTypeId::mono,
            .inputs = {named_matcher("sink", "main")},
        },
    });

    ASSERT_EQ(result.applied_connection_ids, std::vector<std::string>{"sample:main"});
    EXPECT_TRUE(result.diagnostics.empty());
    root.outputs();
    auto configured = std::move(root).finish();
    EXPECT_EQ(configured.connections.configured_sample_connections().size(), before + 1);
    ASSERT_EQ(connections.desired_connections().size(), 1u);
}

TEST(GraphConnections, SamplePortChannelSelectorRemainsDistinctFromNodePathSelection)
{
    auto source_graph = make_sample_passthrough(iv::ChannelTypeId::stereo);
    auto sink_graph = make_sample_sink(iv::ChannelTypeId::stereo);
    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    placements.emplace("source", embed_as_instance(root, source_graph, "source"));
    placements.emplace("sink", embed_as_instance(root, sink_graph, "sink"));
    auto const before = root.finish().connections.configured_sample_connections().size();

    iv::GraphConnections connections;
    auto result = apply(connections, root, placements, iv::GraphConnectionUpsertMutation{
        .connection = iv::ProjectSampleConnection{
            .connection_id = "sample:right",
            .source_type = iv::ChannelTypeId::mono,
            .outputs = {named_matcher("source", "main", 1)},
            .target_type = iv::ChannelTypeId::mono,
            .inputs = {named_matcher("sink", "main", 1)},
        },
    });

    EXPECT_EQ(result.applied_connection_ids.size(), 1u);
    EXPECT_TRUE(result.diagnostics.empty());
    root.outputs();
    auto configured = std::move(root).finish();
    auto const connections_after = configured.connections.configured_sample_connections();
    ASSERT_EQ(connections_after.size(), before + 1);
    auto const& project_connection = connections_after.back();
    EXPECT_EQ(project_connection.source_type, iv::ChannelTypeId::mono);
    EXPECT_EQ(project_connection.target_type, iv::ChannelTypeId::mono);
    ASSERT_EQ(project_connection.source_channels.size(), 1u);
    ASSERT_EQ(project_connection.target_channels.size(), 1u);
    EXPECT_EQ(project_connection.source_channels.front().channel, 1u);
    EXPECT_EQ(project_connection.target_channels.front().channel, 1u);
}

TEST(GraphConnections, DanglingDesiredConnectionSurvivesAndResolvesWhenInstanceAppears)
{
    auto source_graph = make_sample_passthrough();
    auto sink_graph = make_sample_sink();
    iv::GraphConnections connections;

    iv::GraphBuilder first_root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> first_placements;
    first_placements.emplace("source", embed_as_instance(first_root, source_graph, "source"));
    auto first = apply(connections, first_root, first_placements, iv::GraphConnectionUpsertMutation{
        .connection = iv::ProjectSampleConnection{
            .connection_id = "sample:dangling",
            .outputs = {named_matcher("source", "main")},
            .inputs = {named_matcher("sink", "main")},
        },
    });
    EXPECT_TRUE(first.applied_connection_ids.empty());
    ASSERT_EQ(first.diagnostics.size(), 1u);
    EXPECT_EQ(connections.desired_connections().size(), 1u);

    iv::GraphBuilder second_root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> second_placements;
    second_placements.emplace("source", embed_as_instance(second_root, source_graph, "source"));
    second_placements.emplace("sink", embed_as_instance(second_root, sink_graph, "sink"));
    auto second = apply(connections, second_root, second_placements);
    EXPECT_EQ(second.applied_connection_ids,
              std::vector<std::string>{"sample:dangling"});
    EXPECT_TRUE(second.diagnostics.empty());
}

TEST(GraphConnections, VirtualNodeThenOrderedMemberPathResolvesConcretePort)
{
    auto source_graph = make_virtual_source();
    auto sink_graph = make_sample_sink();
    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    placements.emplace("source", embed_as_instance(root, source_graph, "source"));
    placements.emplace("sink", embed_as_instance(root, sink_graph, "sink"));
    auto const before = root.finish().connections.configured_sample_connections().size();

    auto source_matcher = named_matcher("source", "", std::nullopt);
    source_matcher.port.name.reset();
    source_matcher.port.index = 0;
    source_matcher.path = {
        iv::ProjectVirtualNodeSelector{.source_identity = "virtual-source"},
        iv::ProjectVirtualMemberSelector{.index = 1},
    };

    iv::GraphConnections connections;
    auto result = apply(connections, root, placements, iv::GraphConnectionUpsertMutation{
        .connection = iv::ProjectSampleConnection{
            .connection_id = "sample:virtual-member",
            .outputs = {std::move(source_matcher)},
            .inputs = {named_matcher("sink", "main")},
        },
    });
    EXPECT_EQ(result.applied_connection_ids.size(), 1u);
    EXPECT_TRUE(result.diagnostics.empty());
    root.outputs();
    auto configured = std::move(root).finish();
    EXPECT_EQ(configured.connections.configured_sample_connections().size(), before + 1);
}


TEST(GraphConnections, SetValuedVirtualMatcherConnectsEveryResolvedMember)
{
    auto source_graph = make_virtual_source();
    auto sink_graph = make_sample_sink();
    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    placements.emplace("source", embed_as_instance(root, source_graph, "source"));
    placements.emplace("sink", embed_as_instance(root, sink_graph, "sink"));
    auto const before = root.finish().connections.configured_sample_connections().size();

    auto source_matcher = named_matcher("source", "");
    source_matcher.port.name.reset();
    source_matcher.port.index = 0;
    source_matcher.path = {
        iv::ProjectVirtualNodeSelector{.source_identity = "virtual-source"},
    };

    iv::GraphConnections connections;
    auto result = apply(connections, root, placements, iv::GraphConnectionUpsertMutation{
        .connection = iv::ProjectSampleConnection{
            .connection_id = "sample:virtual-set",
            .outputs = {std::move(source_matcher)},
            .inputs = {named_matcher("sink", "main")},
        },
    });

    EXPECT_EQ(result.applied_connection_ids,
              std::vector<std::string>{"sample:virtual-set"});
    EXPECT_TRUE(result.diagnostics.empty());
    root.outputs();
    auto configured = std::move(root).finish();
    EXPECT_EQ(configured.connections.configured_sample_connections().size(), before + 2);
}

TEST(GraphConnections, DeleteRemovesDesiredConnectionBeforeResolution)
{
    iv::GraphConnections connections;
    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    (void)apply(connections, root, placements, iv::GraphConnectionUpsertMutation{
        .connection = iv::ProjectSampleConnection{
            .connection_id = "sample:delete-me",
            .outputs = {named_matcher("missing-source", "main")},
            .inputs = {named_matcher("missing-sink", "main")},
        },
    });
    ASSERT_EQ(connections.desired_connections().size(), 1u);

    auto result = apply(connections, root, placements, iv::GraphConnectionDeleteMutation{
        .connection_id = "sample:delete-me",
    });

    EXPECT_TRUE(result.applied_connection_ids.empty());
    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_TRUE(connections.desired_connections().empty());
}

TEST(GraphConnections, AppliesEventConnectionWithStructuredMatchers)
{
    auto source_graph = make_event_passthrough();
    auto sink_graph = make_event_sink();
    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    placements.emplace("source", embed_as_instance(root, source_graph, "source"));
    placements.emplace("sink", embed_as_instance(root, sink_graph, "sink"));
    auto const before = root.finish().connections.configured_event_connections().size();

    iv::GraphConnections connections;
    auto result = apply(connections, root, placements, iv::GraphConnectionUpsertMutation{
        .connection = iv::ProjectEventConnection{
            .connection_id = "event:trigger",
            .source_type = iv::EventTypeId::trigger,
            .outputs = {named_matcher("source", "trigger")},
            .target_type = iv::EventTypeId::trigger,
            .inputs = {named_matcher("sink", "trigger")},
        },
    });
    EXPECT_EQ(result.applied_connection_ids.size(), 1u);
    EXPECT_TRUE(result.diagnostics.empty());
    root.outputs();
    auto configured = std::move(root).finish();
    EXPECT_EQ(configured.connections.configured_event_connections().size(), before + 1);
}

TEST(GraphConnections, TypeMismatchIsDiagnosticAndDoesNotDeleteDesiredConnection)
{
    auto source_graph = make_sample_passthrough(iv::ChannelTypeId::stereo);
    auto sink_graph = make_sample_sink(iv::ChannelTypeId::stereo);
    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    placements.emplace("source", embed_as_instance(root, source_graph, "source"));
    placements.emplace("sink", embed_as_instance(root, sink_graph, "sink"));
    auto const before = root.finish().connections.configured_sample_connections().size();

    iv::GraphConnections connections;
    auto result = apply(connections, root, placements, iv::GraphConnectionUpsertMutation{
        .connection = iv::ProjectSampleConnection{
            .connection_id = "sample:type-mismatch",
            .source_type = iv::ChannelTypeId::mono,
            .outputs = {named_matcher("source", "main")},
            .target_type = iv::ChannelTypeId::stereo,
            .inputs = {named_matcher("sink", "main")},
        },
    });
    EXPECT_TRUE(result.applied_connection_ids.empty());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(connections.desired_connections().size(), 1u);
    root.outputs();
    auto configured = std::move(root).finish();
    EXPECT_EQ(configured.connections.configured_sample_connections().size(), before);
}

TEST(GraphConnections, InvalidReplaceAllBatchHasStrongExceptionSafety)
{
    iv::GraphConnections connections;
    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    auto valid = iv::ProjectSampleConnection{
        .connection_id = "valid",
        .outputs = {named_matcher("missing-source", "main")},
        .inputs = {named_matcher("missing-sink", "main")},
    };
    (void)apply(connections, root, placements, iv::GraphConnectionUpsertMutation{
        .connection = valid,
    });
    ASSERT_EQ(connections.desired_connections().size(), 1u);

    iv::GraphConnectionsProjectGraphRequest request{
        .root_builder = &root,
        .placements = &placements,
        .mutation = iv::GraphConnectionReplaceAllMutation{
            .connections = {valid, valid},
        },
    };
    EXPECT_THROW(connections.handle_project_graph_transaction(request), std::invalid_argument);
    auto desired = connections.desired_connections();
    ASSERT_EQ(desired.size(), 1u);
    EXPECT_EQ(iv::project_connection_id(desired.front()), "valid");
}


TEST(GraphConnections, TiledChildPathSelectorChoosesNodeBeforePortResolution)
{
    auto source_fixture = make_tiled_source();
    auto sink_graph = make_sample_sink();
    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    auto source_embedding = root.embed(source_fixture.graph, "source");
    auto const source_root = source_embedding.node_bundle(source_fixture.local_root);
    placements.emplace("source", iv::NodeInstancePlacement{
        .embedding = std::move(source_embedding),
        .root = source_root,
    });
    placements.emplace("sink", embed_as_instance(root, sink_graph, "sink"));

    auto source_matcher = named_matcher("source", "");
    source_matcher.port.name.reset();
    source_matcher.port.index = 0;
    source_matcher.path = {iv::ProjectTiledChildSelector{.index = 1}};

    iv::GraphConnections connections;
    auto result = apply(connections, root, placements, iv::GraphConnectionUpsertMutation{
        .connection = iv::ProjectSampleConnection{
            .connection_id = "sample:tiled-child",
            .source_type = iv::ChannelTypeId::mono,
            .outputs = {std::move(source_matcher)},
            .inputs = {named_matcher("sink", "main")},
        },
    });
    EXPECT_EQ(result.applied_connection_ids.size(), 1u);
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST(GraphConnections, SubgraphPathSelectorDescendsByKindAndStableIndex)
{
    auto source_graph = make_nested_subgraph_source();
    auto sink_graph = make_sample_sink();
    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    placements.emplace("source", embed_as_instance(root, source_graph, "source"));
    placements.emplace("sink", embed_as_instance(root, sink_graph, "sink"));

    auto source_matcher = named_matcher("source", "main");
    source_matcher.path = {iv::ProjectSubgraphSelector{
        .kind = "inner",
        .index = 0,
    }};

    iv::GraphConnections connections;
    auto result = apply(connections, root, placements, iv::GraphConnectionUpsertMutation{
        .connection = iv::ProjectSampleConnection{
            .connection_id = "sample:subgraph",
            .outputs = {std::move(source_matcher)},
            .inputs = {named_matcher("sink", "main")},
        },
    });
    EXPECT_EQ(result.applied_connection_ids.size(), 1u);
    EXPECT_TRUE(result.diagnostics.empty());
}
