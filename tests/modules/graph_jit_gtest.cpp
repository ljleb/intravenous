#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/project_graph_graph_connections_bridge.h>
#include <intravenous/runtime/project_graph_graph_jit_bridge.h>
#include <intravenous/runtime/project_graph_node_instances_bridge.h>

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>

TEST(GraphJit, SpecializationIsLatchedAtConstruction)
{
    iv::GraphJit jit(iv::GraphJitConfig{
        .sample_rate = 44100,
        .block_size = 512,
    });

    auto const& specialization = jit.specialization();
    EXPECT_EQ(specialization.sample_rate, 44100u);
    EXPECT_EQ(specialization.block_size, 512u);
    EXPECT_FALSE(specialization.target_triple.empty());
}

TEST(GraphJit, RejectsInvalidKernelConfiguration)
{
    EXPECT_THROW(
        iv::GraphJit(iv::GraphJitConfig{.sample_rate = 0, .block_size = 256}),
        std::invalid_argument);
    EXPECT_THROW(
        iv::GraphJit(iv::GraphJitConfig{.sample_rate = 48000, .block_size = 0}),
        std::invalid_argument);
}

TEST(GraphJit, EmptyGraphCompilesAndMaterializesRootOperations)
{
    iv::GraphJit jit;
    auto graph = std::make_shared<iv::ConfiguredGraph const>();
    auto definitions = std::make_shared<iv::NodeDefinitionsSnapshot>();
    definitions->generation = 12;

    auto const result = jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 34,
        .graph = std::move(graph),
        .definitions = std::move(definitions),
    });

    EXPECT_TRUE(result.attempted);
    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(result.diagnostics.empty());
    ASSERT_TRUE(result.compiled_graph);
    EXPECT_EQ(result.compiled_graph->project_generation, 34u);
    EXPECT_EQ(result.compiled_graph->definitions_generation, 12u);
    EXPECT_EQ(result.compiled_graph->node_layout.storage_size, 0u);
    EXPECT_EQ(result.compiled_graph->node_layout.max_block_size, 256u);
    EXPECT_TRUE(result.compiled_graph->node_layout.nodes.empty());
    EXPECT_TRUE(result.compiled_graph->root_operations.valid());
    EXPECT_TRUE(result.compiled_graph->root_operations.can_skip_block());

    EXPECT_NO_THROW(result.compiled_graph->root_operations.tick_block(nullptr, 0, 256));
    EXPECT_NO_THROW(result.compiled_graph->root_operations.skip_block(nullptr, 256, 256));
}

TEST(GraphJit, MissingPinnedInputsAreCompileDiagnostics)
{
    iv::GraphJit jit;
    auto const result = jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 1,
    });

    EXPECT_TRUE(result.attempted);
    EXPECT_FALSE(result.succeeded());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().stage, iv::GraphJitDiagnosticStage::input_capture);
}

TEST(GraphJitProjectGraphBridge, EmptyRootGenerationCompilesSuccessfully)
{
    iv::NodeInstances instances;
    iv::GraphConnections connections;
    iv::ProjectGraph project_graph;
    iv::GraphJit graph_jit;
    auto instances_scope = iv::project_graph_node_instances_bridge::bind(
        project_graph, instances);
    auto connections_scope = iv::project_graph_graph_connections_bridge::bind(
        project_graph, connections);
    auto graph_jit_scope = iv::project_graph_graph_jit_bridge::bind(
        project_graph, graph_jit);

    auto snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>();
    snapshot->generation = 1;
    EXPECT_NO_THROW(project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = snapshot}));

    auto const generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->generation, 1u);
    EXPECT_EQ(generation->definitions_generation, 1u);
    EXPECT_TRUE(generation->graph_jit_attempted);
    ASSERT_TRUE(generation->compiled_graph);
    EXPECT_TRUE(generation->graph_jit_diagnostics.empty());
    EXPECT_EQ(generation->compiled_graph->project_generation, generation->generation);
    EXPECT_EQ(
        generation->compiled_graph->definitions_generation,
        generation->definitions_generation);
    EXPECT_EQ(generation->compiled_graph->node_layout.storage_size, 0u);
    EXPECT_TRUE(generation->compiled_graph->root_operations.valid());
}

TEST(GraphJitProjectGraphBridge, UnboundSingletonReturnLeavesCompilationUnattempted)
{
    iv::NodeInstances instances;
    iv::GraphConnections connections;
    iv::ProjectGraph project_graph;
    auto instances_scope = iv::project_graph_node_instances_bridge::bind(
        project_graph, instances);
    auto connections_scope = iv::project_graph_graph_connections_bridge::bind(
        project_graph, connections);

    auto snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>();
    snapshot->generation = 1;
    EXPECT_NO_THROW(project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = snapshot}));

    auto const generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_FALSE(generation->graph_jit_attempted);
    EXPECT_FALSE(generation->compiled_graph);
    EXPECT_TRUE(generation->graph_jit_diagnostics.empty());
}
