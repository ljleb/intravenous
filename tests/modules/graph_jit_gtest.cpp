#include "../module_test_utils.h"

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/graph_jit/connection_plan.h>
#include <intravenous/graph_jit/sample_physical_plan.h>
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
constexpr char graph_jit_configured_module_id[] = "iv.test.graph_jit.state_context.configured_module";
constexpr char graph_jit_pointer_configured_module_id[] = "iv.test.graph_jit.state_context.pointer_configured_module";
constexpr char graph_jit_multiple_module_id[] = "iv.test.graph_jit.state_context.multiple_module";
constexpr char graph_jit_skippable_pair_module_id[] = "iv.test.graph_jit.state_context.skippable_pair_module";
constexpr char graph_jit_limited_block_module_id[] = "iv.test.graph_jit.state_context.limited_block_module";
constexpr char graph_jit_ported_module_id[] = "iv.test.graph_jit.state_context.ported_module";
constexpr char graph_jit_direct_sample_module_id[] = "iv.test.graph_jit.state_context.direct_sample_module";
constexpr char graph_jit_transient_sample_module_id[] = "iv.test.graph_jit.state_context.transient_sample_module";
constexpr char graph_jit_reused_sample_slots_module_id[] = "iv.test.graph_jit.state_context.reused_sample_slots_module";

struct alignas(64) StatefulProbeStateMirror {
    std::uint64_t tick_calls = 0;
    std::uint64_t skip_calls = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
    std::uint64_t sample_rate = 0;
    std::uint64_t observed_state_extent = 0;
    std::uint64_t observed_compiled_extent = 0;
};

struct alignas(128) StatefulProbeCompiledStateMirror {
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

struct ConfiguredProbeStateMirror {
    std::uint64_t calls = 0;
    std::size_t first = 0;
    std::size_t second = 0;
};

struct PointerConfiguredProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t first_value = 0;
    std::uint64_t second_value = 0;
    std::uint64_t null_seen = 0;
    std::uint32_t marker = 0;
    std::uint16_t tag = 0;
};

struct LimitedBlockProbeStateMirror {
    std::uint64_t tick_calls = 0;
    std::uint64_t skip_calls = 0;
    std::array<std::uint64_t, 8> tick_indices{};
    std::array<std::uint64_t, 8> tick_sizes{};
    std::array<std::uint64_t, 8> skip_indices{};
    std::array<std::uint64_t, 8> skip_sizes{};
};

struct SampleConsumerProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
    float first = 0.0f;
    float last = 0.0f;
    float sum = 0.0f;
};

void expect_lowering_failure(
    iv::GraphJitCompileResult const& result,
    std::string_view message_fragment)
{
    EXPECT_TRUE(result.attempted);
    EXPECT_FALSE(result.succeeded());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().stage, iv::GraphJitDiagnosticStage::lowering);
    EXPECT_NE(
        result.diagnostics.front().message.find(message_fragment),
        std::string::npos)
        << result.diagnostics.front().message;
}

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

TEST(GraphJitSamplePhysicalPlan, ReusesOnlyNonOverlappingTransientProducerStorage)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::ChannelLayout const stereo{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    };
    auto group = [](iv::ChannelLayout layout,
                    iv::SampleConnectionImplementationKind implementation,
                    std::size_t begin,
                    std::size_t end) {
        SampleProducerGroupPlan result;
        result.canonical_source_layout = layout;
        result.has_realtime_connections = true;
        result.implementation = implementation;
        result.live_interval = ConnectionLiveIntervalPlan{
            .begin = begin,
            .end = end,
        };
        return result;
    };

    ConnectionAnalysisPlan connections;
    connections.sample_producer_groups.push_back(group(
        mono, iv::SampleConnectionImplementationKind::direct, 0, 1));
    connections.sample_producer_groups.push_back(group(
        mono,
        iv::SampleConnectionImplementationKind::transient_materialization,
        2,
        3));
    connections.sample_producer_groups.push_back(group(
        mono, iv::SampleConnectionImplementationKind::direct, 1, 2));
    connections.sample_producer_groups.push_back(group(
        stereo, iv::SampleConnectionImplementationKind::direct, 4, 5));

    auto physical = build_sample_physical_plan(connections, 64);
    ASSERT_TRUE(physical.has_value())
        << (physical ? std::string{} : physical.error());
    ASSERT_EQ(physical->producer_groups.size(), 4u);
    ASSERT_EQ(physical->transient_slots.size(), 2u);
    for (auto const& producer : physical->producer_groups) {
        ASSERT_TRUE(producer.has_value());
        ASSERT_NE(
            producer->canonical_representation, no_sample_representation);
        ASSERT_LT(
            producer->canonical_representation, physical->representations.size());
    }

    auto slot_for_group = [&](std::size_t group_index) {
        auto const representation =
            physical->producer_groups[group_index]->canonical_representation;
        return physical->representations[representation].transient_slot;
    };
    auto const slot0 = slot_for_group(0);
    auto const slot1 = slot_for_group(1);
    auto const overlapping = slot_for_group(2);
    auto const later_stereo = slot_for_group(3);
    EXPECT_EQ(slot0, slot1);
    EXPECT_EQ(slot0, later_stereo);
    EXPECT_NE(slot0, overlapping);
    ASSERT_LT(slot0, physical->transient_slots.size());
    ASSERT_LT(overlapping, physical->transient_slots.size());
    EXPECT_EQ(
        physical->transient_slots[slot0].size_bytes,
        64u * 2u * sizeof(iv::Sample));
    EXPECT_EQ(
        physical->transient_slots[overlapping].size_bytes,
        64u * sizeof(iv::Sample));

    iv::NodeLayoutBuilder builder(64);
    auto declared = declare_sample_physical_storage(builder, *physical);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_physical_storage(layout, *physical);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    ASSERT_TRUE(physical->transient_region.valid());
    ASSERT_LT(physical->transient_region.index, layout.regions.size());
    auto const& region = layout.regions[physical->transient_region.index];
    EXPECT_EQ(region.kind, iv::NodeLayout::Region::Kind::raw);
    EXPECT_EQ(region.size, 64u * 3u * sizeof(iv::Sample));
    EXPECT_EQ(
        physical->transient_slots[slot0].storage_offset,
        region.storage_offset
            + physical->transient_slots[slot0].region_relative_offset);
    EXPECT_EQ(
        physical->transient_slots[overlapping].storage_offset,
        region.storage_offset
            + physical->transient_slots[overlapping].region_relative_offset);
}

TEST(GraphJitSamplePhysicalPlan, LeavesCompiledAccessBranchesUnresolved)
{
    using namespace iv::graph_jit::detail;

    ConnectionAnalysisPlan connections;
    SampleConnectionPlan realtime;
    realtime.access = PlannedConnectionAccess::realtime_to_realtime;
    connections.sample_connections.push_back(std::move(realtime));
    SampleConnectionPlan compiled;
    compiled.access = PlannedConnectionAccess::realtime_to_compiled;
    connections.sample_connections.push_back(std::move(compiled));

    SampleProducerGroupPlan group;
    group.canonical_source_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    group.connection_indices = {0, 1};
    group.has_realtime_connections = true;
    group.has_compiled_connections = true;
    group.implementation = iv::SampleConnectionImplementationKind::direct;
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
    };
    connections.sample_producer_groups.push_back(std::move(group));

    auto physical = build_sample_physical_plan(connections, 64);
    ASSERT_TRUE(physical.has_value())
        << (physical ? std::string{} : physical.error());
    ASSERT_EQ(physical->connection_representations.size(), 2u);
    ASSERT_TRUE(physical->connection_representations[0].has_value());
    EXPECT_FALSE(physical->connection_representations[1].has_value());
    ASSERT_EQ(physical->producer_groups.size(), 1u);
    ASSERT_TRUE(physical->producer_groups[0].has_value());
    EXPECT_EQ(
        *physical->connection_representations[0],
        physical->producer_groups[0]->canonical_representation);
}

TEST(GraphJitSamplePhysicalPlan, DefersConvertedBranchesToDerivedRepresentations)
{
    using namespace iv::graph_jit::detail;

    ConnectionAnalysisPlan connections;
    SampleConnectionPlan connection;
    connection.access = PlannedConnectionAccess::realtime_to_realtime;
    connection.requires_conversion = true;
    connections.sample_connections.push_back(std::move(connection));

    SampleProducerGroupPlan group;
    group.canonical_source_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    group.connection_indices.push_back(0);
    group.has_realtime_connections = true;
    group.implementation =
        iv::SampleConnectionImplementationKind::transient_materialization;
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
    };
    connections.sample_producer_groups.push_back(std::move(group));

    auto physical = build_sample_physical_plan(connections, 64);
    ASSERT_FALSE(physical.has_value());
    EXPECT_NE(
        physical.error().find("derived converted sample representations"),
        std::string::npos);
}

TEST(GraphJitSamplePhysicalPlan, RejectsTransientStorageThatCrossesKernelCalls)
{
    using namespace iv::graph_jit::detail;

    ConnectionAnalysisPlan connections;
    SampleProducerGroupPlan group;
    group.canonical_source_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    group.has_realtime_connections = true;
    group.implementation = iv::SampleConnectionImplementationKind::direct;
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(std::move(group));

    auto physical = build_sample_physical_plan(connections, 64);
    ASSERT_FALSE(physical.has_value());
    EXPECT_NE(
        physical.error().find("cross-kernel retained storage"),
        std::string::npos);
}

TEST(GraphJit, ZeroPortPrimitiveStateContextsAndMultiNodeExecution)
{
    auto const workspace = iv::test_support::make_inline_module_workspace(
        "graph_jit_single_primitive_state_context",
        R"cpp(
#include <intravenous/dsl.h>

#include <array>
#include <cstdint>

namespace {
struct StatefulProbe {
    struct alignas(64) State {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        std::uint64_t sample_rate = 0;
        std::uint64_t observed_state_extent = 0;
        std::uint64_t observed_compiled_extent = 0;
    };

    struct alignas(128) CompiledState {
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

struct ConfiguredProbe {
    std::size_t first = 0;
    std::size_t second = 0;

    struct State {
        std::uint64_t calls = 0;
        std::size_t first = 0;
        std::size_t second = 0;
    };

    constexpr ConfiguredProbe(std::size_t first_, std::size_t second_)
        : first(first_), second(second_)
    {}

    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<ConfiguredProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.first = first;
        state.second = second;
    }
};

inline constexpr std::array<std::uint64_t, 4> pointer_probe_values{
    0x1111111111111111ull,
    0x2222222222222222ull,
    0x3333333333333333ull,
    0x4444444444444444ull,
};

struct PointerConfiguredProbe {
    std::uint32_t marker = 0;
    std::uint64_t const* first = nullptr;
    std::uint64_t const* second = nullptr;
    std::uint64_t const* optional = nullptr;
    std::uint16_t tag = 0;

    struct State {
        std::uint64_t calls = 0;
        std::uint64_t first_value = 0;
        std::uint64_t second_value = 0;
        std::uint64_t null_seen = 0;
        std::uint32_t marker = 0;
        std::uint16_t tag = 0;
    };

    constexpr PointerConfiguredProbe(
        std::uint32_t marker_,
        std::uint64_t const* first_,
        std::uint64_t const* second_,
        std::uint64_t const* optional_,
        std::uint16_t tag_)
        : marker(marker_), first(first_), second(second_), optional(optional_), tag(tag_)
    {}

    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<PointerConfiguredProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.first_value = *first;
        state.second_value = *second;
        state.null_seen = optional == nullptr ? 1u : 0u;
        state.marker = marker;
        state.tag = tag;
    }
};

struct LimitedBlockProbe {
    struct State {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
        std::array<std::uint64_t, 8> tick_indices{};
        std::array<std::uint64_t, 8> tick_sizes{};
        std::array<std::uint64_t, 8> skip_indices{};
        std::array<std::uint64_t, 8> skip_sizes{};
    };

    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    std::size_t max_block_size() const { return 16; }
    bool can_skip_block() const { return true; }

    void tick_block(iv::TickBlockContext<LimitedBlockProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const slot = state.tick_calls++;
        if (slot < state.tick_indices.size()) {
            state.tick_indices[slot] = ctx.index;
            state.tick_sizes[slot] = ctx.block_size;
        }
    }

    void skip_block(iv::SkipBlockContext<LimitedBlockProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const slot = state.skip_calls++;
        if (slot < state.skip_indices.size()) {
            state.skip_indices[slot] = ctx.index;
            state.skip_sizes[slot] = ctx.block_size;
        }
    }
};

struct SampleRampSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<SampleRampSource> const& ctx) const
    {
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            ctx.outputs[0].push(static_cast<iv::Sample>(ctx.index + i));
        }
    }
};

struct LimitedSampleRampSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output("out")};
    }

    std::size_t max_block_size() const { return 16; }

    void tick_block(iv::TickBlockContext<LimitedSampleRampSource> const& ctx) const
    {
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            ctx.outputs[0].push(static_cast<iv::Sample>(ctx.index + i));
        }
    }
};

struct SampleConsumerProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        float first = 0.0f;
        float last = 0.0f;
        float sum = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<SampleConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const block = ctx.inputs[0].get_block(ctx.block_size);
        ++state.calls;
        state.last_index = ctx.index;
        state.last_block_size = ctx.block_size;
        state.first = block.empty() ? iv::Sample{0} : block[0];
        state.last = block.empty() ? iv::Sample{0} : block[block.size() - 1];
        state.sum = 0.0f;
        for (auto const sample : block) {
            state.sum += sample;
        }
    }
};

struct PortedProbe {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{};
    }

    void tick(iv::TickSampleContext<PortedProbe> const& ctx) const
    {
        ctx.outputs[0].push(iv::Sample{0.25f});
    }
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

void configured_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.configured">(
        std::size_t{0x12345678u}, std::size_t{0xabcdef01u});
    graph.outputs();
}

void pointer_configured_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.pointer_configured">(
        std::uint32_t{0x89abcdefu},
        pointer_probe_values.data() + 1,
        pointer_probe_values.data() + 3,
        static_cast<std::uint64_t const*>(nullptr),
        std::uint16_t{0x4567u});
    graph.outputs();
}

void multiple_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.stateful">();
    (void)graph.node<"iv.test.graph_jit.state_context.configured">(
        std::size_t{1}, std::size_t{2});
    graph.outputs();
}

void skippable_pair_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.stateful">();
    (void)graph.node<"iv.test.graph_jit.state_context.stateful">();
    graph.outputs();
}

void limited_block_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.limited_block">();
    graph.outputs();
}

void direct_sample_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink(source);
    graph.outputs();
}

void transient_sample_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.limited_sample_ramp_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink(source);
    graph.outputs();
}

void reused_sample_slots_module(iv::GraphBuilder& graph)
{
    auto source_a = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto sink_a = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink_a(source_a);
    auto source_b = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto sink_b = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink_b(source_b);
    graph.outputs();
}

void ported_module(iv::GraphBuilder& graph)
{
    graph.outputs(graph.node<"iv.test.graph_jit.state_context.ported">());
}
} // namespace

IV_NODE("iv.test.graph_jit.state_context.stateful", StatefulProbe);
IV_NODE("iv.test.graph_jit.state_context.state_only", StateOnlyProbe);
IV_NODE("iv.test.graph_jit.state_context.compiled_only", CompiledOnlyProbe);
IV_NODE("iv.test.graph_jit.state_context.stateless", StatelessProbe);
IV_NODE("iv.test.graph_jit.state_context.configured", ConfiguredProbe);
IV_NODE("iv.test.graph_jit.state_context.pointer_configured", PointerConfiguredProbe);
IV_NODE("iv.test.graph_jit.state_context.limited_block", LimitedBlockProbe);
IV_NODE("iv.test.graph_jit.state_context.sample_ramp_source", SampleRampSource);
IV_NODE("iv.test.graph_jit.state_context.limited_sample_ramp_source", LimitedSampleRampSource);
IV_NODE("iv.test.graph_jit.state_context.sample_consumer", SampleConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.ported", PortedProbe);
IV_MODULE("iv.test.graph_jit.state_context.stateful_module", stateful_module);
IV_MODULE("iv.test.graph_jit.state_context.state_only_module", state_only_module);
IV_MODULE("iv.test.graph_jit.state_context.compiled_only_module", compiled_only_module);
IV_MODULE("iv.test.graph_jit.state_context.stateless_module", stateless_module);
IV_MODULE("iv.test.graph_jit.state_context.configured_module", configured_module);
IV_MODULE("iv.test.graph_jit.state_context.pointer_configured_module", pointer_configured_module);
IV_MODULE("iv.test.graph_jit.state_context.multiple_module", multiple_module);
IV_MODULE("iv.test.graph_jit.state_context.skippable_pair_module", skippable_pair_module);
IV_MODULE("iv.test.graph_jit.state_context.limited_block_module", limited_block_module);
IV_MODULE("iv.test.graph_jit.state_context.direct_sample_module", direct_sample_module);
IV_MODULE("iv.test.graph_jit.state_context.transient_sample_module", transient_sample_module);
IV_MODULE("iv.test.graph_jit.state_context.reused_sample_slots_module", reused_sample_slots_module);
IV_MODULE("iv.test.graph_jit.state_context.ported_module", ported_module);
)cpp");

    auto const canonical_workspace = std::filesystem::weakly_canonical(workspace);
    std::shared_ptr<iv::PackageRevision const> revision;
    {
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
        revision = std::make_shared<iv::PackageRevision const>(
            std::move(package_request.result.revisions.front()));
    }
    ASSERT_TRUE(revision);
    ASSERT_EQ(revision->leaf_definitions.size(), 11u);
    ASSERT_EQ(revision->module_definitions.size(), 13u);

    auto revision_weak = std::weak_ptr<iv::PackageRevision const>{revision};
    auto definitions = make_graph_jit_snapshot(revision, 91);
    auto jit = std::make_unique<iv::GraphJit>(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
    });
    iv::ResourceContext resources;

    auto compile_graph = [&](
                             std::shared_ptr<iv::ConfiguredGraph const> graph,
                             std::uint64_t generation) {
        EXPECT_NE(graph, nullptr);
        return jit->compile(iv::GraphJitCompileRequest{
            .project_generation = generation,
            .graph = std::move(graph),
            .definitions = definitions,
        });
    };
    auto compile = [&](std::string_view module_id, std::uint64_t generation) {
        return compile_graph(
            configured_module_graph(*revision, module_id), generation);
    };

    auto stateful = compile(graph_jit_stateful_module_id, 100);
    ASSERT_TRUE(stateful.succeeded())
        << (stateful.diagnostics.empty() ? "" : stateful.diagnostics.front().message);
    ASSERT_TRUE(stateful.compiled_graph->root_operations.valid());
    ASSERT_TRUE(stateful.compiled_graph->root_operations.can_skip_block());
    auto stateful_survivor = stateful.compiled_graph;
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
    auto const& stateful_node = stateful.compiled_graph->node_layout.nodes.front();
    EXPECT_EQ(stateful_node.state_alignment, alignof(StatefulProbeStateMirror));
    EXPECT_EQ(
        stateful_node.compiled_state_alignment,
        alignof(StatefulProbeCompiledStateMirror));
    EXPECT_EQ(
        reinterpret_cast<std::uintptr_t>(state) % alignof(StatefulProbeStateMirror),
        0u);
    EXPECT_EQ(
        reinterpret_cast<std::uintptr_t>(compiled)
            % alignof(StatefulProbeCompiledStateMirror),
        0u);
    EXPECT_EQ(
        static_cast<std::byte*>(static_cast<void*>(state)),
        stateful_storage.buffer().data()
            + stateful_node.state_offset);
    EXPECT_EQ(
        static_cast<std::byte*>(static_cast<void*>(compiled)),
        stateful_storage.buffer().data()
            + stateful_node.compiled_state_offset);

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

    stateful.compiled_graph->root_operations.tick_block(
        stateful_storage.buffer().data(), 73, 64);
    EXPECT_EQ(state->tick_calls, 2u);
    EXPECT_EQ(state->skip_calls, 1u);
    EXPECT_EQ(state->last_index, 73u);
    EXPECT_EQ(state->last_block_size, 64u);
    EXPECT_EQ(compiled->tick_calls, 2u);
    EXPECT_EQ(compiled->skip_calls, 1u);
    EXPECT_EQ(compiled->last_index, 73u);
    EXPECT_EQ(compiled->last_block_size, 64u);

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

    auto configured_graph = configured_module_graph(*revision, graph_jit_configured_module_id);
    ASSERT_TRUE(configured_graph);
    auto configured = jit->compile(iv::GraphJitCompileRequest{
        .project_generation = 104,
        .graph = configured_graph,
        .definitions = definitions,
    });
    ASSERT_TRUE(configured.succeeded())
        << (configured.diagnostics.empty() ? "" : configured.diagnostics.front().message);
    EXPECT_EQ(configured.compiled_graph->project_generation, 104u);
    EXPECT_EQ(configured.compiled_graph->definitions_generation, 91u);
    EXPECT_EQ(configured.compiled_graph->configured_graph, configured_graph);
    ASSERT_EQ(configured.compiled_graph->package_revisions.size(), 1u);
    EXPECT_EQ(configured.compiled_graph->package_revisions.front(), revision);
    expect_single_node_canonical_regions(
        configured.compiled_graph->node_layout,
        sizeof(ConfiguredProbeStateMirror),
        0);
    auto configured_storage = configured.compiled_graph->node_layout.create_storage(resources);
    configured_storage.initialize();
    auto* configured_state = static_cast<ConfiguredProbeStateMirror*>(
        configured_storage.state_ptr(0));
    ASSERT_NE(configured_state, nullptr);
    configured.compiled_graph->root_operations.tick_block(
        configured_storage.buffer().data(), 11, 32);
    EXPECT_EQ(configured_state->calls, 1u);
    EXPECT_EQ(configured_state->first, std::size_t{0x12345678u});
    EXPECT_EQ(configured_state->second, std::size_t{0xabcdef01u});

    auto pointer_graph = configured_module_graph(
        *revision, graph_jit_pointer_configured_module_id);
    ASSERT_TRUE(pointer_graph);
    std::size_t pointer_relocation_count = 0;
    pointer_graph->node_bundles.for_each_configured_bundle(
        [&](iv::ConfiguredNodeBundleView const& view) {
            if (view.kind != iv::ConfiguredNodeBundleKind::concrete
                || !view.config_relocations) {
                return;
            }
            pointer_relocation_count += view.config_relocations->size();
        });
    ASSERT_EQ(pointer_relocation_count, 3u);
    auto pointer_configured = compile_graph(pointer_graph, 105);
    ASSERT_TRUE(pointer_configured.succeeded())
        << (pointer_configured.diagnostics.empty()
                ? ""
                : pointer_configured.diagnostics.front().message);
    expect_single_node_canonical_regions(
        pointer_configured.compiled_graph->node_layout,
        sizeof(PointerConfiguredProbeStateMirror),
        0);
    auto pointer_storage =
        pointer_configured.compiled_graph->node_layout.create_storage(resources);
    pointer_storage.initialize();
    auto* pointer_state = static_cast<PointerConfiguredProbeStateMirror*>(
        pointer_storage.state_ptr(0));
    ASSERT_NE(pointer_state, nullptr);
    pointer_configured.compiled_graph->root_operations.tick_block(
        pointer_storage.buffer().data(), 13, 16);
    EXPECT_EQ(pointer_state->calls, 1u);
    EXPECT_EQ(pointer_state->first_value, 0x2222222222222222ull);
    EXPECT_EQ(pointer_state->second_value, 0x4444444444444444ull);
    EXPECT_EQ(pointer_state->null_seen, 1u);
    EXPECT_EQ(pointer_state->marker, 0x89abcdefu);
    EXPECT_EQ(pointer_state->tag, 0x4567u);
    auto pointer_survivor = pointer_configured.compiled_graph;

    auto multiple_graph = configured_module_graph(
        *revision, graph_jit_multiple_module_id);
    ASSERT_TRUE(multiple_graph);
    auto disconnected_multiple_graph =
        std::make_shared<iv::ConfiguredGraph>(*multiple_graph);
    disconnected_multiple_graph->connections = {};
    auto multiple = compile_graph(disconnected_multiple_graph, 106);
    ASSERT_TRUE(multiple.succeeded())
        << (multiple.diagnostics.empty() ? "" : multiple.diagnostics.front().message);
    EXPECT_FALSE(multiple.compiled_graph->root_operations.can_skip_block());
    ASSERT_EQ(multiple.compiled_graph->node_layout.nodes.size(), 2u);
    EXPECT_EQ(
        multiple.compiled_graph->node_layout.nodes[0].state_size,
        sizeof(StatefulProbeStateMirror));
    EXPECT_EQ(
        multiple.compiled_graph->node_layout.nodes[0].compiled_state_size,
        sizeof(StatefulProbeCompiledStateMirror));
    EXPECT_EQ(
        multiple.compiled_graph->node_layout.nodes[1].state_size,
        sizeof(ConfiguredProbeStateMirror));
    auto multiple_storage = multiple.compiled_graph->node_layout.create_storage(resources);
    multiple_storage.initialize();
    auto* multiple_stateful_state = static_cast<StatefulProbeStateMirror*>(
        multiple_storage.state_ptr(0));
    auto* multiple_configured_state = static_cast<ConfiguredProbeStateMirror*>(
        multiple_storage.state_ptr(1));
    ASSERT_NE(multiple_stateful_state, nullptr);
    ASSERT_NE(multiple_configured_state, nullptr);
    multiple.compiled_graph->root_operations.tick_block(
        multiple_storage.buffer().data(), 19, 32);
    EXPECT_EQ(multiple_stateful_state->tick_calls, 1u);
    EXPECT_EQ(multiple_stateful_state->last_index, 19u);
    EXPECT_EQ(multiple_stateful_state->last_block_size, 32u);
    EXPECT_EQ(multiple_configured_state->calls, 1u);
    EXPECT_EQ(multiple_configured_state->first, 1u);
    EXPECT_EQ(multiple_configured_state->second, 2u);

    auto skippable_pair_graph = configured_module_graph(
        *revision, graph_jit_skippable_pair_module_id);
    ASSERT_TRUE(skippable_pair_graph);
    auto disconnected_skippable_pair_graph =
        std::make_shared<iv::ConfiguredGraph>(*skippable_pair_graph);
    disconnected_skippable_pair_graph->connections = {};
    auto skippable_pair = compile_graph(disconnected_skippable_pair_graph, 107);
    ASSERT_TRUE(skippable_pair.succeeded())
        << (skippable_pair.diagnostics.empty()
                ? ""
                : skippable_pair.diagnostics.front().message);
    ASSERT_TRUE(skippable_pair.compiled_graph->root_operations.can_skip_block());
    ASSERT_EQ(skippable_pair.compiled_graph->node_layout.nodes.size(), 2u);
    auto skippable_pair_storage =
        skippable_pair.compiled_graph->node_layout.create_storage(resources);
    skippable_pair_storage.initialize();
    auto* pair_state_0 = static_cast<StatefulProbeStateMirror*>(
        skippable_pair_storage.state_ptr(0));
    auto* pair_state_1 = static_cast<StatefulProbeStateMirror*>(
        skippable_pair_storage.state_ptr(1));
    ASSERT_NE(pair_state_0, nullptr);
    ASSERT_NE(pair_state_1, nullptr);
    skippable_pair.compiled_graph->root_operations.tick_block(
        skippable_pair_storage.buffer().data(), 23, 32);
    EXPECT_EQ(pair_state_0->tick_calls, 1u);
    EXPECT_EQ(pair_state_1->tick_calls, 1u);
    skippable_pair.compiled_graph->root_operations.skip_block(
        skippable_pair_storage.buffer().data(), 55, 16);
    EXPECT_EQ(pair_state_0->skip_calls, 1u);
    EXPECT_EQ(pair_state_1->skip_calls, 1u);

    auto limited = compile(graph_jit_limited_block_module_id, 108);
    ASSERT_TRUE(limited.succeeded())
        << (limited.diagnostics.empty() ? "" : limited.diagnostics.front().message);
    ASSERT_TRUE(limited.compiled_graph->root_operations.can_skip_block());
    expect_single_node_canonical_regions(
        limited.compiled_graph->node_layout,
        sizeof(LimitedBlockProbeStateMirror),
        0);
    auto limited_storage = limited.compiled_graph->node_layout.create_storage(resources);
    limited_storage.initialize();
    auto* limited_state = static_cast<LimitedBlockProbeStateMirror*>(
        limited_storage.state_ptr(0));
    ASSERT_NE(limited_state, nullptr);

    limited.compiled_graph->root_operations.tick_block(
        limited_storage.buffer().data(), 200, 64);
    EXPECT_EQ(limited_state->tick_calls, 4u);
    EXPECT_EQ(
        limited_state->tick_indices,
        (std::array<std::uint64_t, 8>{200, 216, 232, 248, 0, 0, 0, 0}));
    EXPECT_EQ(
        limited_state->tick_sizes,
        (std::array<std::uint64_t, 8>{16, 16, 16, 16, 0, 0, 0, 0}));

    limited.compiled_graph->root_operations.tick_block(
        limited_storage.buffer().data(), 300, 8);
    EXPECT_EQ(limited_state->tick_calls, 5u);
    EXPECT_EQ(limited_state->tick_indices[4], 300u);
    EXPECT_EQ(limited_state->tick_sizes[4], 8u);

    limited.compiled_graph->root_operations.skip_block(
        limited_storage.buffer().data(), 400, 32);
    EXPECT_EQ(limited_state->skip_calls, 2u);
    EXPECT_EQ(
        limited_state->skip_indices,
        (std::array<std::uint64_t, 8>{400, 416, 0, 0, 0, 0, 0, 0}));
    EXPECT_EQ(
        limited_state->skip_sizes,
        (std::array<std::uint64_t, 8>{16, 16, 0, 0, 0, 0, 0, 0}));

    auto find_consumer_state = [](iv::CompiledGraph const& graph, iv::NodeStorage& storage) {
        for (std::size_t i = 0; i < graph.node_layout.nodes.size(); ++i) {
            if (graph.node_layout.nodes[i].state_size
                == sizeof(SampleConsumerProbeStateMirror)) {
                return static_cast<SampleConsumerProbeStateMirror*>(
                    storage.state_ptr(i));
            }
        }
        return static_cast<SampleConsumerProbeStateMirror*>(nullptr);
    };
    auto count_raw_regions = [](iv::NodeLayout const& layout) {
        return static_cast<std::size_t>(std::ranges::count_if(
            layout.regions,
            [](iv::NodeLayout::Region const& region) {
                return region.kind == iv::NodeLayout::Region::Kind::raw;
            }));
    };

    auto direct_graph = configured_module_graph(
        *revision, graph_jit_direct_sample_module_id);
    ASSERT_TRUE(direct_graph);
    auto direct_analysis = iv::graph_jit::detail::build_connection_analysis_plan(
        *direct_graph, 64);
    ASSERT_TRUE(direct_analysis.has_value())
        << (direct_analysis ? std::string{} : direct_analysis.error());
    ASSERT_EQ(direct_analysis->sample_producer_groups.size(), 1u);
    ASSERT_TRUE(direct_analysis->sample_producer_groups[0].implementation.has_value());
    EXPECT_EQ(
        *direct_analysis->sample_producer_groups[0].implementation,
        iv::SampleConnectionImplementationKind::direct);
    auto direct_physical = iv::graph_jit::detail::build_sample_physical_plan(
        *direct_analysis, 64);
    ASSERT_TRUE(direct_physical.has_value())
        << (direct_physical ? std::string{} : direct_physical.error());
    ASSERT_EQ(direct_physical->producer_groups.size(), 1u);
    ASSERT_TRUE(direct_physical->producer_groups[0].has_value());
    ASSERT_EQ(direct_physical->connection_representations.size(), 1u);
    ASSERT_TRUE(direct_physical->connection_representations[0].has_value());
    EXPECT_EQ(
        *direct_physical->connection_representations[0],
        direct_physical->producer_groups[0]->canonical_representation);

    auto direct = compile_graph(direct_graph, 109);
    ASSERT_TRUE(direct.succeeded())
        << (direct.diagnostics.empty() ? "" : direct.diagnostics.front().message);
    ASSERT_EQ(direct.compiled_graph->node_layout.nodes.size(), 2u);
    ASSERT_EQ(count_raw_regions(direct.compiled_graph->node_layout), 1u);
    auto const direct_raw = std::ranges::find_if(
        direct.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw;
        });
    ASSERT_NE(direct_raw, direct.compiled_graph->node_layout.regions.end());
    EXPECT_EQ(
        direct_raw->size,
        64u * sizeof(iv::Sample));

    auto direct_storage = direct.compiled_graph->node_layout.create_storage(resources);
    direct_storage.initialize();
    auto* direct_state = find_consumer_state(*direct.compiled_graph, direct_storage);
    ASSERT_NE(direct_state, nullptr);
    direct.compiled_graph->root_operations.tick_block(
        direct_storage.buffer().data(), 100, 64);
    EXPECT_EQ(direct_state->calls, 1u);
    EXPECT_EQ(direct_state->last_index, 100u);
    EXPECT_EQ(direct_state->last_block_size, 64u);
    EXPECT_FLOAT_EQ(direct_state->first, 100.0f);
    EXPECT_FLOAT_EQ(direct_state->last, 163.0f);
    EXPECT_FLOAT_EQ(direct_state->sum, 8416.0f);
    direct.compiled_graph->root_operations.tick_block(
        direct_storage.buffer().data(), 200, 16);
    EXPECT_EQ(direct_state->calls, 2u);
    EXPECT_EQ(direct_state->last_index, 200u);
    EXPECT_EQ(direct_state->last_block_size, 16u);
    EXPECT_FLOAT_EQ(direct_state->first, 200.0f);
    EXPECT_FLOAT_EQ(direct_state->last, 215.0f);
    EXPECT_FLOAT_EQ(direct_state->sum, 3320.0f);

    auto transient_graph = configured_module_graph(
        *revision, graph_jit_transient_sample_module_id);
    ASSERT_TRUE(transient_graph);
    auto transient_analysis = iv::graph_jit::detail::build_connection_analysis_plan(
        *transient_graph, 64);
    ASSERT_TRUE(transient_analysis.has_value())
        << (transient_analysis ? std::string{} : transient_analysis.error());
    ASSERT_EQ(transient_analysis->sample_producer_groups.size(), 1u);
    ASSERT_TRUE(
        transient_analysis->sample_producer_groups[0].implementation.has_value());
    EXPECT_EQ(
        *transient_analysis->sample_producer_groups[0].implementation,
        iv::SampleConnectionImplementationKind::transient_materialization);
    auto transient_physical =
        iv::graph_jit::detail::build_sample_physical_plan(
            *transient_analysis, 64);
    ASSERT_TRUE(transient_physical.has_value())
        << (transient_physical
                ? std::string{}
                : transient_physical.error());
    ASSERT_EQ(transient_physical->producer_groups.size(), 1u);
    ASSERT_TRUE(transient_physical->producer_groups[0].has_value());
    ASSERT_EQ(transient_physical->connection_representations.size(), 1u);
    ASSERT_TRUE(transient_physical->connection_representations[0].has_value());
    EXPECT_EQ(
        *transient_physical->connection_representations[0],
        transient_physical->producer_groups[0]->canonical_representation);

    auto transient = compile_graph(transient_graph, 110);
    ASSERT_TRUE(transient.succeeded())
        << (transient.diagnostics.empty()
                ? ""
                : transient.diagnostics.front().message);
    ASSERT_EQ(transient.compiled_graph->node_layout.nodes.size(), 2u);
    ASSERT_EQ(count_raw_regions(transient.compiled_graph->node_layout), 1u);
    auto const transient_raw = std::ranges::find_if(
        transient.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw;
        });
    ASSERT_NE(transient_raw, transient.compiled_graph->node_layout.regions.end());
    EXPECT_EQ(
        transient_raw->size,
        64u * sizeof(iv::Sample));
    auto transient_storage =
        transient.compiled_graph->node_layout.create_storage(resources);
    transient_storage.initialize();
    auto* transient_state = find_consumer_state(
        *transient.compiled_graph, transient_storage);
    ASSERT_NE(transient_state, nullptr);

    // The limited producer executes as four 16-frame slices. Each imported
    // invocation reconstructs an ephemeral façade at its absolute sample index,
    // while all slices target the same planned current-block representation.
    // The 64-frame consumer therefore observes one contiguous logical block.
    transient.compiled_graph->root_operations.tick_block(
        transient_storage.buffer().data(), 300, 64);
    EXPECT_EQ(transient_state->calls, 1u);
    EXPECT_EQ(transient_state->last_index, 300u);
    EXPECT_EQ(transient_state->last_block_size, 64u);
    EXPECT_FLOAT_EQ(transient_state->first, 300.0f);
    EXPECT_FLOAT_EQ(transient_state->last, 363.0f);
    EXPECT_FLOAT_EQ(transient_state->sum, 21216.0f);
    transient.compiled_graph->root_operations.tick_block(
        transient_storage.buffer().data(), 500, 32);
    EXPECT_EQ(transient_state->calls, 2u);
    EXPECT_EQ(transient_state->last_index, 500u);
    EXPECT_EQ(transient_state->last_block_size, 32u);
    EXPECT_FLOAT_EQ(transient_state->first, 500.0f);
    EXPECT_FLOAT_EQ(transient_state->last, 531.0f);
    EXPECT_FLOAT_EQ(transient_state->sum, 16496.0f);

    auto reused_slots_graph = configured_module_graph(
        *revision, graph_jit_reused_sample_slots_module_id);
    ASSERT_TRUE(reused_slots_graph);
    auto reused_slots_analysis =
        iv::graph_jit::detail::build_connection_analysis_plan(
            *reused_slots_graph, 64);
    ASSERT_TRUE(reused_slots_analysis.has_value())
        << (reused_slots_analysis
                ? std::string{}
                : reused_slots_analysis.error());
    auto reused_slots_physical =
        iv::graph_jit::detail::build_sample_physical_plan(
            *reused_slots_analysis, 64);
    ASSERT_TRUE(reused_slots_physical.has_value())
        << (reused_slots_physical
                ? std::string{}
                : reused_slots_physical.error());
    ASSERT_EQ(reused_slots_physical->representations.size(), 2u);
    ASSERT_EQ(reused_slots_physical->transient_slots.size(), 1u);

    auto reused_slots = compile_graph(reused_slots_graph, 111);
    ASSERT_TRUE(reused_slots.succeeded())
        << (reused_slots.diagnostics.empty()
                ? ""
                : reused_slots.diagnostics.front().message);
    ASSERT_EQ(reused_slots.compiled_graph->node_layout.nodes.size(), 4u);
    ASSERT_EQ(count_raw_regions(reused_slots.compiled_graph->node_layout), 1u);
    auto const reused_raw = std::ranges::find_if(
        reused_slots.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw;
        });
    ASSERT_NE(reused_raw, reused_slots.compiled_graph->node_layout.regions.end());
    EXPECT_EQ(reused_raw->size, 64u * sizeof(iv::Sample));

    auto reused_storage =
        reused_slots.compiled_graph->node_layout.create_storage(resources);
    reused_storage.initialize();
    std::vector<SampleConsumerProbeStateMirror*> reused_consumer_states;
    for (std::size_t i = 0;
         i < reused_slots.compiled_graph->node_layout.nodes.size(); ++i) {
        if (reused_slots.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(SampleConsumerProbeStateMirror)) {
            reused_consumer_states.push_back(
                static_cast<SampleConsumerProbeStateMirror*>(
                    reused_storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(reused_consumer_states.size(), 2u);
    reused_slots.compiled_graph->root_operations.tick_block(
        reused_storage.buffer().data(), 700, 64);
    for (auto const* state : reused_consumer_states) {
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->calls, 1u);
        EXPECT_EQ(state->last_index, 700u);
        EXPECT_EQ(state->last_block_size, 64u);
        EXPECT_FLOAT_EQ(state->first, 700.0f);
        EXPECT_FLOAT_EQ(state->last, 763.0f);
        EXPECT_FLOAT_EQ(state->sum, 46816.0f);
    }

    auto ported_graph = configured_module_graph(*revision, graph_jit_ported_module_id);
    ASSERT_TRUE(ported_graph);
    auto ported = compile_graph(ported_graph, 112);
    expect_lowering_failure(ported, "does not yet support external sample boundaries");

    auto disconnected_ported_graph =
        std::make_shared<iv::ConfiguredGraph>(*ported_graph);
    disconnected_ported_graph->connections = {};
    auto disconnected_ported = compile_graph(disconnected_ported_graph, 113);
    expect_lowering_failure(
        disconnected_ported, "does not yet support external sample boundaries");

    // A CompiledGraph must keep both the project ORC domain and its package
    // revision alive independently of GraphJit, the definitions snapshot, and
    // newer generations. Drop every external owner and execute the oldest
    // generation again through the storage/layout it owns.
    state_only_storage = iv::NodeStorage{};
    compiled_only_storage = iv::NodeStorage{};
    configured_storage = iv::NodeStorage{};
    pointer_storage = iv::NodeStorage{};
    multiple_storage = iv::NodeStorage{};
    skippable_pair_storage = iv::NodeStorage{};
    limited_storage = iv::NodeStorage{};
    direct_storage = iv::NodeStorage{};
    transient_storage = iv::NodeStorage{};
    reused_storage = iv::NodeStorage{};
    stateful = {};
    state_only = {};
    compiled_only = {};
    stateless = {};
    configured = {};
    pointer_configured = {};
    multiple = {};
    skippable_pair = {};
    limited = {};
    direct = {};
    transient = {};
    reused_slots = {};
    ported = {};
    disconnected_ported = {};
    definitions.reset();
    revision.reset();
    jit.reset();
    EXPECT_FALSE(revision_weak.expired());
    ASSERT_EQ(stateful_survivor->package_revisions.size(), 1u);

    stateful_survivor->root_operations.tick_block(
        stateful_storage.buffer().data(), 137, 32);
    EXPECT_EQ(state->tick_calls, 3u);
    EXPECT_EQ(state->skip_calls, 1u);
    EXPECT_EQ(state->last_index, 137u);
    EXPECT_EQ(state->last_block_size, 32u);
    EXPECT_EQ(compiled->tick_calls, 3u);
    EXPECT_EQ(compiled->skip_calls, 1u);
    EXPECT_EQ(compiled->last_index, 137u);
    EXPECT_EQ(compiled->last_block_size, 32u);

    auto pointer_survivor_storage =
        pointer_survivor->node_layout.create_storage(resources);
    pointer_survivor_storage.initialize();
    auto* pointer_survivor_state = static_cast<PointerConfiguredProbeStateMirror*>(
        pointer_survivor_storage.state_ptr(0));
    ASSERT_NE(pointer_survivor_state, nullptr);
    pointer_survivor->root_operations.tick_block(
        pointer_survivor_storage.buffer().data(), 149, 32);
    EXPECT_EQ(pointer_survivor_state->calls, 1u);
    EXPECT_EQ(pointer_survivor_state->first_value, 0x2222222222222222ull);
    EXPECT_EQ(pointer_survivor_state->second_value, 0x4444444444444444ull);
    EXPECT_EQ(pointer_survivor_state->null_seen, 1u);
    EXPECT_EQ(pointer_survivor_state->marker, 0x89abcdefu);
    EXPECT_EQ(pointer_survivor_state->tag, 0x4567u);
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
