#include <intravenous/basic_nodes/arithmetic.h>
#include "../event_source_test_support.h"
#include <intravenous/dsl.h>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/project/configuration_expression.h>
#include <intravenous/project/graph_connections.h>
#include <intravenous/project/project_node_path_resolver.h>

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
        auto input = iv::programmatic::declare_input<stereo>(graph, "main");
        graph.outputs("main"_P = input);
    } else {
        auto input = iv::programmatic::declare_input(graph, "main");
        graph.outputs("main"_P = input);
    }
    return std::move(graph).finish();
}

iv::ConfiguredGraph make_sample_sink(iv::ChannelTypeId type = iv::ChannelTypeId::mono)
{
    using namespace iv;
    GraphBuilder graph;
    if (type == ChannelTypeId::stereo) {
        (void)iv::programmatic::declare_input<stereo>(graph, "main");
    } else {
        (void)iv::programmatic::declare_input(graph, "main");
    }
    graph.outputs();
    return std::move(graph).finish();
}

iv::ConfiguredGraph make_event_passthrough()
{
    using namespace iv;
    GraphBuilder graph;
    auto trigger = iv::programmatic::declare_event_input(graph, "trigger", EventTypeId::trigger);
    graph.event_outputs("trigger"_P = trigger);
    graph.outputs();
    return std::move(graph).finish();
}

iv::ConfiguredGraph make_event_sink()
{
    using namespace iv;
    GraphBuilder graph;
    (void)iv::programmatic::declare_event_input(graph, "trigger", EventTypeId::trigger);
    graph.event_outputs();
    graph.outputs();
    return std::move(graph).finish();
}

iv::ConfiguredGraph make_symbolic_source()
{
    using namespace iv;
    using Pass = SumN<mono, SampleStreamLayout::planar, 1>;
    GraphBuilder graph;
    auto input = iv::programmatic::declare_input(graph, "in");
    auto first = details::configure_concrete_node<Pass>(graph);
    first(input);
    first._annotate_source_info("symbolic-source", "/tmp/symbolic.cpp", 1, 2);
    auto second = details::configure_concrete_node<Pass>(graph);
    second(input);
    second._annotate_source_info("symbolic-source", "/tmp/symbolic.cpp", 3, 4);
    graph.outputs("main"_P = first);
    return std::move(graph).finish();
}

iv::ConfiguredGraph make_set_valued_symbolic_source()
{
    using namespace iv;
    using First = SumN<mono, SampleStreamLayout::planar, 1>;
    using Second = ProductN<1>;
    GraphBuilder graph;
    auto input = iv::programmatic::declare_input(graph, "in");
    auto first = details::configure_concrete_node<First>(graph);
    first(input);
    first._annotate_source_info(
        "symbolic-source", "/tmp/symbolic.cpp", 1, 2);
    auto second = details::configure_concrete_node<Second>(graph);
    second(input);
    second._annotate_source_info(
        "symbolic-source", "/tmp/symbolic.cpp", 3, 4);
    graph.outputs("main"_P = first);
    return std::move(graph).finish();
}

iv::ConfiguredGraph make_scoped_symbolic_source()
{
    using namespace iv;
    using Pass = SumN<mono, SampleStreamLayout::planar, 1>;
    GraphBuilder graph;
    auto input = iv::programmatic::declare_input(graph, "in");
    auto make_processor = [&](std::string_view processor, std::size_t offset) {
        auto result = graph.subgraph([&](SubgraphBuilder& boundary) {
            auto filter = details::configure_concrete_node<Pass>(graph);
            filter(input);
            filter._annotate_source_info(
                "filter", "/tmp/scoped.cpp", offset + 1, offset + 2);
            boundary.outputs("main"_P = filter);
        }, "processor");
        result._annotate_source_info(
            processor, "/tmp/scoped.cpp", offset + 3, offset + 4);
        return result;
    };
    auto processor_a = make_processor("processorA", 0);
    (void)make_processor("processorB", 10);
    graph.outputs("main"_P = processor_a);
    return std::move(graph).finish();
}

struct FrozenRootFixture {
    iv::ConfiguredGraph graph{};
    iv::NodeBundleHandle local_root = 0;
};

FrozenRootFixture make_tiled_source()
{
    using namespace iv;
    using Pass = SumN<mono, SampleStreamLayout::planar, 1>;
    GraphBuilder graph;
    auto tiled = details::configure_concrete_tiled_node<Pass, stereo>(graph);
    auto const local_root = tiled.node_bundle_handle();
    graph.outputs("main"_P = tiled);
    return {std::move(graph).finish(), local_root};
}

iv::NodeInstancePlacement embed_as_instance(
    iv::GraphBuilder& root,
    iv::ConfiguredGraph const& graph,
    std::string_view kind)
{
    auto embedding = root.embed(graph, kind);
    auto const instance_root = embedding.root_scope;
    return iv::NodeInstancePlacement{
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
    in_event_source([&] { return connections.handle_project_graph_transaction(request); });
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
    EXPECT_EQ(project_connection.source_channels.front().channel.channel, 1u);
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

TEST(GraphConnections, SymbolicNodeThenOrderedMemberPathResolvesConcretePort)
{
    auto source_graph = make_symbolic_source();
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
        iv::ProjectSymbolicNodeSelector{.source_identity = "symbolic-source"},
        iv::ProjectSymbolicMemberSelector{.index = 1},
    };

    iv::GraphConnections connections;
    auto result = apply(connections, root, placements, iv::GraphConnectionUpsertMutation{
        .connection = iv::ProjectSampleConnection{
            .connection_id = "sample:symbolic-member",
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


TEST(GraphConnections, SetValuedSymbolicMatcherKeepsAllMembersInOneConnection)
{
    auto source_graph = make_set_valued_symbolic_source();
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
        iv::ProjectSymbolicNodeSelector{.source_identity = "symbolic-source"},
    };

    iv::GraphConnections connections;
    auto result = apply(connections, root, placements, iv::GraphConnectionUpsertMutation{
        .connection = iv::ProjectSampleConnection{
            .connection_id = "sample:symbolic-set",
            .outputs = {std::move(source_matcher)},
            .inputs = {named_matcher("sink", "main")},
        },
    });

    EXPECT_EQ(result.applied_connection_ids,
              std::vector<std::string>{"sample:symbolic-set"});
    EXPECT_TRUE(result.diagnostics.empty());
    root.outputs();
    auto configured = std::move(root).finish();
    auto const records = configured.connections.configured_sample_connections();
    ASSERT_EQ(records.size(), before + 1);
    auto const& record = records.back();
    ASSERT_EQ(record.source_channels.size(), 2u);
    EXPECT_NE(record.source_channels[0].channel, record.source_channels[1].channel);
    EXPECT_EQ(record.target_channels.size(), 1u);
}

TEST(GraphConnections, SingleNodeConsumerRejectsASetValuedPathResult)
{
    auto source_graph = make_set_valued_symbolic_source();
    iv::GraphBuilder root;
    auto placement = embed_as_instance(root, source_graph, "source");
    auto configured = root.finish();
    auto matches = iv::resolve_project_node_path(
        configured,
        placement.root,
        std::array<iv::ProjectNodePathSelector, 1>{
            iv::ProjectSymbolicNodeSelector{.source_identity = "symbolic-source"},
        });
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_THROW(
        (void)iv::require_single_project_node_path_match(
            matches, "configuration node reference"),
        std::invalid_argument);
}

TEST(GraphConnections, SymbolicLookupDoesNotFindDescendantOnlyNodes)
{
    auto source_graph = make_scoped_symbolic_source();
    iv::GraphBuilder root;
    auto placement = embed_as_instance(root, source_graph, "source");
    auto configured = root.finish();
    auto matches = iv::resolve_project_node_path(
        configured,
        placement.root,
        std::array<iv::ProjectNodePathSelector, 1>{
            iv::ProjectSymbolicNodeSelector{.source_identity = "filter"},
        });
    EXPECT_TRUE(matches.empty());
}

TEST(GraphConnections, SelectedSymbolicSubgraphSuppliesTheNextLookupScope)
{
    auto source_graph = make_scoped_symbolic_source();
    iv::GraphBuilder root;
    auto placement = embed_as_instance(root, source_graph, "source");
    auto configured = root.finish();
    auto matches = iv::resolve_project_node_path(
        configured,
        placement.root,
        std::array<iv::ProjectNodePathSelector, 2>{
            iv::ProjectSymbolicNodeSelector{.source_identity = "processorA"},
            iv::ProjectSymbolicNodeSelector{.source_identity = "filter"},
        });
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front().kind, iv::ProjectNodePathMatch::Kind::symbolic_node);
    ASSERT_EQ(matches.front().symbolic_member_indices.size(), 1u);

    auto processor_a_member = iv::resolve_project_node_path(
        configured,
        placement.root,
        std::array<iv::ProjectNodePathSelector, 3>{
            iv::ProjectSymbolicNodeSelector{.source_identity = "processorA"},
            iv::ProjectSymbolicNodeSelector{.source_identity = "filter"},
            iv::ProjectSymbolicMemberSelector{.index = 0},
        });
    auto processor_b_member = iv::resolve_project_node_path(
        configured,
        placement.root,
        std::array<iv::ProjectNodePathSelector, 3>{
            iv::ProjectSymbolicNodeSelector{.source_identity = "processorB"},
            iv::ProjectSymbolicNodeSelector{.source_identity = "filter"},
            iv::ProjectSymbolicMemberSelector{.index = 0},
        });
    ASSERT_EQ(processor_a_member.size(), 1u);
    ASSERT_EQ(processor_b_member.size(), 1u);
    EXPECT_NE(processor_a_member.front().bundle, processor_b_member.front().bundle);
    EXPECT_TRUE(iv::resolve_project_node_path(
        configured,
        placement.root,
        std::array<iv::ProjectNodePathSelector, 3>{
            iv::ProjectSymbolicNodeSelector{.source_identity = "processorA"},
            iv::ProjectSymbolicNodeSelector{.source_identity = "filter"},
            iv::ProjectSymbolicMemberSelector{.index = 1},
        }).empty());
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
    EXPECT_THROW(in_event_source([&] { return connections.handle_project_graph_transaction(request); }), std::invalid_argument);
    auto desired = connections.desired_connections();
    ASSERT_EQ(desired.size(), 1u);
    EXPECT_EQ(iv::project_connection_id(desired.front()), "valid");
}

TEST(GraphConnections, EventConnectionRejectsSampleChannelSelector)
{
    iv::GraphConnections connections;
    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    iv::GraphConnectionsProjectGraphRequest request{
        .root_builder = &root,
        .placements = &placements,
        .mutation = iv::GraphConnectionUpsertMutation{
            .connection = iv::ProjectEventConnection{
                .connection_id = "event:invalid-channel",
                .source_type = iv::EventTypeId::trigger,
                .outputs = {named_matcher("source", "events", 0)},
                .target_type = iv::EventTypeId::trigger,
                .inputs = {named_matcher("target", "events")},
            },
        },
    };

    EXPECT_THROW(
        in_event_source([&] { return connections.handle_project_graph_transaction(request); }),
        std::invalid_argument);
    EXPECT_TRUE(connections.desired_connections().empty());
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
        .root = source_root,
    });
    placements.emplace("sink", embed_as_instance(root, sink_graph, "sink"));

    auto source_matcher = named_matcher("source", "");
    source_matcher.port.name.reset();
    source_matcher.port.index = 0;
    source_matcher.path = {iv::ProjectTiledChildSelector{.name = "right"}};
    auto const children = iv::project_child_nodes(source_fixture.graph,
        {.kind = iv::ProjectNodePathMatch::Kind::bundle, .bundle = source_fixture.local_root});
    ASSERT_EQ(children.size(), 2u);
    EXPECT_EQ(children[1].path, source_matcher.path);
    EXPECT_EQ(iv::project_member_path(source_fixture.graph, source_fixture.local_root, children[1].bundle),
        children[1].path);

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

TEST(GraphConnections, ConfigurationReferenceAndConnectionUseTheSamePathResolver)
{
    constexpr std::string_view source_id =
        "550e8400-e29b-41d4-a716-446655440000";
    auto source_fixture = make_tiled_source();
    auto sink_graph = make_sample_sink();
    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    auto source_embedding = root.embed(source_fixture.graph, "source");
    auto const source_root = source_embedding.node_bundle(source_fixture.local_root);
    placements.emplace(source_id, iv::NodeInstancePlacement{
        .root = source_root,
    });
    placements.emplace("sink", embed_as_instance(root, sink_graph, "sink"));

    auto parsed = iv::parse_configuration_expression_list(
        std::string("&") + std::string(source_id) + "#right");
    auto const* reference = std::get_if<iv::ConfigurationNodeReferenceExpression>(
        &parsed.arguments.front()->value);
    ASSERT_NE(reference, nullptr);
    auto snapshot = root.finish();
    auto matches = iv::resolve_project_node_path(
        snapshot, placements, reference->instance_id, reference->path);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front().kind, iv::ProjectNodePathMatch::Kind::bundle);

    auto source_matcher = named_matcher(reference->instance_id, "");
    source_matcher.port.name.reset();
    source_matcher.port.index = 0;
    source_matcher.path = reference->path;
    iv::GraphConnections connections;
    auto result = apply(
        connections,
        root,
        placements,
        iv::GraphConnectionUpsertMutation{
            .connection = iv::ProjectSampleConnection{
                .connection_id = "sample:shared-reference-path",
                .outputs = {std::move(source_matcher)},
                .inputs = {named_matcher("sink", "main")},
            },
        });
    EXPECT_EQ(
        result.applied_connection_ids,
        std::vector<std::string>{"sample:shared-reference-path"});
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST(GraphConnections, ProjectNodePathRejectsUnknownSubjectsAndSelectors)
{
    auto source_graph = make_symbolic_source();
    auto tiled_fixture = make_tiled_source();
    iv::GraphBuilder root;
    std::unordered_map<std::string, iv::NodeInstancePlacement> placements;
    placements.emplace("source", embed_as_instance(root, source_graph, "source"));
    auto tiled_embedding = root.embed(tiled_fixture.graph, "tiled");
    auto const tiled_root = tiled_embedding.node_bundle(tiled_fixture.local_root);
    placements.emplace("tiled", iv::NodeInstancePlacement{
        .root = tiled_root,
    });
    auto configured = root.finish();

    EXPECT_TRUE(iv::resolve_project_node_path(
        configured,
        placements,
        "unknown",
        std::span<iv::ProjectNodePathSelector const>{}).empty());
    EXPECT_TRUE(iv::resolve_project_node_path(
        configured,
        placements,
        "source",
        std::array<iv::ProjectNodePathSelector, 2>{
            iv::ProjectSymbolicNodeSelector{.source_identity = "symbolic-source"},
            iv::ProjectSymbolicMemberSelector{.index = 99},
        }).empty());
    EXPECT_TRUE(iv::resolve_project_node_path(
        configured,
        placements,
        "tiled",
        std::array<iv::ProjectNodePathSelector, 1>{
            iv::ProjectTiledChildSelector{.name = "unknown"},
        }).empty());
}
