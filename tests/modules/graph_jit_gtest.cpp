#include "../module_test_utils.h"

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/node/resources.h>
#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/project_graph_graph_connections_bridge.h>
#include <intravenous/runtime/project_graph_graph_jit_bridge.h>
#include <intravenous/runtime/project_graph_node_instances_bridge.h>
#include <intravenous/runtime/package_jit.h>
#include <intravenous/runtime/startup_config.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>


namespace {
constexpr char graph_jit_state_package_id[] = "iv.test.graph_jit.state_context.package";
constexpr char graph_jit_stateful_module_id[] = "iv.test.graph_jit.state_context.stateful_module";
constexpr char graph_jit_state_only_module_id[] = "iv.test.graph_jit.state_context.state_only_module";
constexpr char graph_jit_compiled_only_module_id[] = "iv.test.graph_jit.state_context.compiled_only_module";
constexpr char graph_jit_stateless_module_id[] = "iv.test.graph_jit.state_context.stateless_module";

struct StatefulProbeStateMirror {
    std::uint64_t tick_calls = 0;
    std::uint64_t skip_calls = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
    std::uint64_t sample_rate = 0;
    std::uint64_t observed_state_extent = 0;
    std::uint64_t observed_compiled_extent = 0;
};

struct StatefulProbeCompiledStateMirror {
    std::uint64_t tick_calls = 0;
    std::uint64_t skip_calls = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
    std::uint64_t observed_state_extent = 0;
    std::uint64_t observed_compiled_extent = 0;
};

struct SingleSpanProbeMirror {
    std::uint64_t calls = 0;
    std::uint64_t observed_state_extent = 0;
    std::uint64_t observed_compiled_extent = 0;
};

std::shared_ptr<iv::NodeDefinitionsSnapshot const> make_graph_jit_snapshot(
    std::shared_ptr<iv::PackageRevision const> revision,
    std::uint64_t generation)
{
    auto snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>();
    snapshot->generation = generation;
    snapshot->package_revisions.push_back(revision);
    for (auto const& leaf : revision->leaf_definitions) {
        snapshot->by_id.emplace(leaf.definition_id, iv::NodeDefinitionEntry{
            .definition_id = leaf.definition_id,
            .kind = iv::NodeDefinitionKind::leaf,
            .version = generation,
            .definition = iv::LeafNodeDefinition{
                .definition_id = leaf.definition_id,
                .package_id = leaf.package_id,
                .package_root = leaf.package_root,
                .provider = leaf.provider,
                .compiler_record = leaf.compiler_record,
                .module_refs = leaf.module_refs,
            },
        });
    }
    return snapshot;
}

std::shared_ptr<iv::ConfiguredGraph const> configured_module_graph(
    iv::PackageRevision const& revision,
    std::string_view definition_id)
{
    auto const found = std::ranges::find(
        revision.module_definitions,
        definition_id,
        &iv::PackageModuleDefinition::definition_id);
    if (found == revision.module_definitions.end()) return {};
    return found->configured_graph;
}

void expect_single_node_canonical_regions(
    iv::NodeLayout const& layout,
    std::size_t state_size,
    std::size_t compiled_state_size)
{
    ASSERT_EQ(layout.nodes.size(), 1u);
    auto const& node = layout.nodes.front();
    EXPECT_EQ(node.state_size, state_size);
    EXPECT_EQ(node.compiled_state_size, compiled_state_size);
    EXPECT_TRUE(layout.imported_arrays.empty());
    EXPECT_TRUE(layout.exported_arrays.empty());
    EXPECT_EQ(layout.regions.size(), compiled_state_size == 0 ? 1u : 2u);

    auto const state_region = std::ranges::find_if(
        layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::state;
        });
    ASSERT_NE(state_region, layout.regions.end());
    EXPECT_EQ(state_region->owner_node, 0u);
    EXPECT_EQ(state_region->size, state_size);
    if (state_size != 0) {
        ASSERT_GE(node.state_offset, 0);
        EXPECT_EQ(
            state_region->storage_offset,
            static_cast<std::size_t>(node.state_offset));
    }

    if (compiled_state_size == 0) {
        EXPECT_EQ(node.compiled_state_offset, -1);
        EXPECT_EQ(std::ranges::count_if(
            layout.regions,
            [](iv::NodeLayout::Region const& region) {
                return region.kind == iv::NodeLayout::Region::Kind::compiled_state;
            }), 0u);
        return;
    }

    auto const compiled_region = std::ranges::find_if(
        layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::compiled_state;
        });
    ASSERT_NE(compiled_region, layout.regions.end());
    EXPECT_EQ(compiled_region->owner_node, 0u);
    EXPECT_EQ(compiled_region->size, compiled_state_size);
    ASSERT_GE(node.compiled_state_offset, 0);
    EXPECT_EQ(
        compiled_region->storage_offset,
        static_cast<std::size_t>(node.compiled_state_offset));
}
} // namespace

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


TEST(GraphJitReflectedAbi, ReflectedSpanRoundTripsPointerAndExactExtent)
{
    std::array<std::byte, 7> bytes{};
    iv::ReflectedSpan<std::byte> reflected{std::span<std::byte>{bytes}};

    EXPECT_EQ(reflected.data(), bytes.data());
    EXPECT_EQ(reflected.size(), bytes.size());
    EXPECT_FALSE(reflected.empty());

    std::span<std::byte> round_trip = reflected;
    EXPECT_EQ(round_trip.data(), bytes.data());
    EXPECT_EQ(round_trip.size(), bytes.size());

    iv::ReflectedSpan<std::byte> empty;
    EXPECT_EQ(empty.data(), nullptr);
    EXPECT_EQ(empty.size(), 0u);
    EXPECT_TRUE(empty.empty());
}

TEST(GraphJit, SinglePrimitiveStateContextsUseCanonicalExactSpans)
{
    auto const workspace = iv::test_support::make_inline_module_workspace(
        "graph_jit_single_primitive_state_context",
        R"cpp(
#include <intravenous/dsl.h>

#include <array>
#include <cstdint>

namespace {
struct StatefulProbe {
    struct State {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        std::uint64_t sample_rate = 0;
        std::uint64_t observed_state_extent = 0;
        std::uint64_t observed_compiled_extent = 0;
    };

    struct CompiledState {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        std::uint64_t observed_state_extent = 0;
        std::uint64_t observed_compiled_extent = 0;
    };

    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    bool can_skip_block() const { return true; }

    void tick_block(iv::TickBlockContext<StatefulProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto& compiled = ctx.compiled_state();
        ++state.tick_calls;
        state.last_index = ctx.index;
        state.last_block_size = ctx.block_size;
        state.sample_rate = ctx.sample_rate;
        state.observed_state_extent = ctx.buffer.size();
        state.observed_compiled_extent = ctx.compiled_state_storage.size();
        ++compiled.tick_calls;
        compiled.last_index = ctx.index;
        compiled.last_block_size = ctx.block_size;
        compiled.observed_state_extent = ctx.buffer.size();
        compiled.observed_compiled_extent = ctx.compiled_state_storage.size();
    }

    void skip_block(iv::SkipBlockContext<StatefulProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto& compiled = ctx.compiled_state();
        ++state.skip_calls;
        state.last_index = ctx.index;
        state.last_block_size = ctx.block_size;
        state.sample_rate = ctx.sample_rate;
        state.observed_state_extent = ctx.buffer.size();
        state.observed_compiled_extent = ctx.compiled_state_storage.size();
        ++compiled.skip_calls;
        compiled.last_index = ctx.index;
        compiled.last_block_size = ctx.block_size;
        compiled.observed_state_extent = ctx.buffer.size();
        compiled.observed_compiled_extent = ctx.compiled_state_storage.size();
    }
};

struct StateOnlyProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t observed_state_extent = 0;
        std::uint64_t observed_compiled_extent = 0;
    };

    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<StateOnlyProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.observed_state_extent = ctx.buffer.size();
        state.observed_compiled_extent = ctx.compiled_state_storage.size();
    }
};

struct CompiledOnlyProbe {
    struct CompiledState {
        std::uint64_t calls = 0;
        std::uint64_t observed_state_extent = 0;
        std::uint64_t observed_compiled_extent = 0;
    };

    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<CompiledOnlyProbe> const& ctx) const
    {
        auto& compiled = ctx.compiled_state();
        ++compiled.calls;
        compiled.observed_state_extent = ctx.buffer.size();
        compiled.observed_compiled_extent = ctx.compiled_state_storage.size();
    }
};

struct StatelessProbe {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<StatelessProbe> const&) const {}
};

void stateful_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.stateful">();
    graph.outputs();
}

void state_only_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.state_only">();
    graph.outputs();
}

void compiled_only_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.compiled_only">();
    graph.outputs();
}

void stateless_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.stateless">();
    graph.outputs();
}
} // namespace

IV_NODE("iv.test.graph_jit.state_context.stateful", StatefulProbe);
IV_NODE("iv.test.graph_jit.state_context.state_only", StateOnlyProbe);
IV_NODE("iv.test.graph_jit.state_context.compiled_only", CompiledOnlyProbe);
IV_NODE("iv.test.graph_jit.state_context.stateless", StatelessProbe);
IV_MODULE("iv.test.graph_jit.state_context.stateful_module", stateful_module);
IV_MODULE("iv.test.graph_jit.state_context.state_only_module", state_only_module);
IV_MODULE("iv.test.graph_jit.state_context.compiled_only_module", compiled_only_module);
IV_MODULE("iv.test.graph_jit.state_context.stateless_module", stateless_module);
)cpp");

    auto const canonical_workspace = std::filesystem::weakly_canonical(workspace);
    iv::StartupConfig startup_config(canonical_workspace, iv::test::repo_root(), {});
    iv::PackageJit package_jit(
        startup_config.initialize(),
        iv::ModuleLoader::OptimizationLevel::O0);
    iv::PackageJitBatchRequest package_request{
        .declarations = {iv::IvPackageDeclaration{
            .package_id = graph_jit_state_package_id,
            .package_root = canonical_workspace,
        }},
    };
    package_jit.handle_build_request(package_request);

    if (!package_request.result.failed.empty()) {
        FAIL() << package_request.result.failed.front().message;
    }
    ASSERT_EQ(package_request.result.revisions.size(), 1u);
    auto revision = std::make_shared<iv::PackageRevision const>(
        std::move(package_request.result.revisions.front()));
    ASSERT_EQ(revision->leaf_definitions.size(), 4u);
    ASSERT_EQ(revision->module_definitions.size(), 4u);

    auto definitions = make_graph_jit_snapshot(revision, 91);
    iv::GraphJit jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
    });
    iv::ResourceContext resources;

    auto compile = [&](std::string_view module_id, std::uint64_t generation) {
        auto graph = configured_module_graph(*revision, module_id);
        EXPECT_NE(graph, nullptr);
        return jit.compile(iv::GraphJitCompileRequest{
            .project_generation = generation,
            .graph = std::move(graph),
            .definitions = definitions,
        });
    };

    auto stateful = compile(graph_jit_stateful_module_id, 100);
    ASSERT_TRUE(stateful.succeeded())
        << (stateful.diagnostics.empty() ? "" : stateful.diagnostics.front().message);
    ASSERT_TRUE(stateful.compiled_graph->root_operations.valid());
    ASSERT_TRUE(stateful.compiled_graph->root_operations.can_skip_block());
    expect_single_node_canonical_regions(
        stateful.compiled_graph->node_layout,
        sizeof(StatefulProbeStateMirror),
        sizeof(StatefulProbeCompiledStateMirror));
    ASSERT_EQ(stateful.compiled_graph->node_layout.nodes.size(), 1u);

    auto stateful_storage = stateful.compiled_graph->node_layout.create_storage(resources);
    stateful_storage.initialize();
    auto* state = static_cast<StatefulProbeStateMirror*>(stateful_storage.state_ptr(0));
    auto* compiled = static_cast<StatefulProbeCompiledStateMirror*>(
        stateful_storage.compiled_state_ptr(0));
    ASSERT_NE(state, nullptr);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(
        static_cast<std::byte*>(static_cast<void*>(state)),
        stateful_storage.buffer().data()
            + stateful.compiled_graph->node_layout.nodes.front().state_offset);
    EXPECT_EQ(
        static_cast<std::byte*>(static_cast<void*>(compiled)),
        stateful_storage.buffer().data()
            + stateful.compiled_graph->node_layout.nodes.front().compiled_state_offset);

    stateful.compiled_graph->root_operations.tick_block(
        stateful_storage.buffer().data(), 17, 32);
    EXPECT_EQ(state->tick_calls, 1u);
    EXPECT_EQ(state->skip_calls, 0u);
    EXPECT_EQ(state->last_index, 17u);
    EXPECT_EQ(state->last_block_size, 32u);
    EXPECT_EQ(state->sample_rate, 88200u);
    EXPECT_EQ(state->observed_state_extent, sizeof(StatefulProbeStateMirror));
    EXPECT_EQ(
        state->observed_compiled_extent,
        sizeof(StatefulProbeCompiledStateMirror));
    EXPECT_EQ(compiled->tick_calls, 1u);
    EXPECT_EQ(compiled->skip_calls, 0u);
    EXPECT_EQ(compiled->last_index, 17u);
    EXPECT_EQ(compiled->last_block_size, 32u);
    EXPECT_EQ(compiled->observed_state_extent, sizeof(StatefulProbeStateMirror));
    EXPECT_EQ(
        compiled->observed_compiled_extent,
        sizeof(StatefulProbeCompiledStateMirror));

    stateful.compiled_graph->root_operations.skip_block(
        stateful_storage.buffer().data(), 41, 16);
    EXPECT_EQ(state->tick_calls, 1u);
    EXPECT_EQ(state->skip_calls, 1u);
    EXPECT_EQ(state->last_index, 41u);
    EXPECT_EQ(state->last_block_size, 16u);
    EXPECT_EQ(state->sample_rate, 88200u);
    EXPECT_EQ(state->observed_state_extent, sizeof(StatefulProbeStateMirror));
    EXPECT_EQ(
        state->observed_compiled_extent,
        sizeof(StatefulProbeCompiledStateMirror));
    EXPECT_EQ(compiled->tick_calls, 1u);
    EXPECT_EQ(compiled->skip_calls, 1u);
    EXPECT_EQ(compiled->last_index, 41u);
    EXPECT_EQ(compiled->last_block_size, 16u);

    auto state_only = compile(graph_jit_state_only_module_id, 101);
    ASSERT_TRUE(state_only.succeeded())
        << (state_only.diagnostics.empty() ? "" : state_only.diagnostics.front().message);
    EXPECT_FALSE(state_only.compiled_graph->root_operations.can_skip_block());
    expect_single_node_canonical_regions(
        state_only.compiled_graph->node_layout,
        sizeof(SingleSpanProbeMirror),
        0);
    ASSERT_EQ(state_only.compiled_graph->node_layout.nodes.size(), 1u);
    auto state_only_storage = state_only.compiled_graph->node_layout.create_storage(resources);
    state_only_storage.initialize();
    auto* state_only_value = static_cast<SingleSpanProbeMirror*>(
        state_only_storage.state_ptr(0));
    state_only.compiled_graph->root_operations.tick_block(
        state_only_storage.buffer().data(), 3, 8);
    ASSERT_NE(state_only_value, nullptr);
    EXPECT_EQ(state_only_value->calls, 1u);
    EXPECT_EQ(state_only_value->observed_state_extent, sizeof(SingleSpanProbeMirror));
    EXPECT_EQ(state_only_value->observed_compiled_extent, 0u);

    auto compiled_only = compile(graph_jit_compiled_only_module_id, 102);
    ASSERT_TRUE(compiled_only.succeeded())
        << (compiled_only.diagnostics.empty() ? "" : compiled_only.diagnostics.front().message);
    EXPECT_FALSE(compiled_only.compiled_graph->root_operations.can_skip_block());
    expect_single_node_canonical_regions(
        compiled_only.compiled_graph->node_layout,
        0,
        sizeof(SingleSpanProbeMirror));
    ASSERT_EQ(compiled_only.compiled_graph->node_layout.nodes.size(), 1u);
    auto compiled_only_storage = compiled_only.compiled_graph->node_layout.create_storage(resources);
    compiled_only_storage.initialize();
    auto* compiled_only_value = static_cast<SingleSpanProbeMirror*>(
        compiled_only_storage.compiled_state_ptr(0));
    compiled_only.compiled_graph->root_operations.tick_block(
        compiled_only_storage.buffer().data(), 5, 16);
    ASSERT_NE(compiled_only_value, nullptr);
    EXPECT_EQ(compiled_only_value->calls, 1u);
    EXPECT_EQ(compiled_only_value->observed_state_extent, 0u);
    EXPECT_EQ(
        compiled_only_value->observed_compiled_extent,
        sizeof(SingleSpanProbeMirror));

    auto stateless = compile(graph_jit_stateless_module_id, 103);
    ASSERT_TRUE(stateless.succeeded())
        << (stateless.diagnostics.empty() ? "" : stateless.diagnostics.front().message);
    EXPECT_FALSE(stateless.compiled_graph->root_operations.can_skip_block());
    expect_single_node_canonical_regions(stateless.compiled_graph->node_layout, 0, 0);
    EXPECT_EQ(stateless.compiled_graph->node_layout.storage_size, 0u);
    EXPECT_NO_THROW(stateless.compiled_graph->root_operations.tick_block(nullptr, 9, 32));
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
