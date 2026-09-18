#include "../module_test_utils.h"

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/graph_jit/connection_plan.h>
#include <intravenous/graph_jit/event_retention_runtime.h>
#include <intravenous/graph_jit/sample_physical_plan.h>
#include <intravenous/graph_jit/transient_arena_plan.h>
#include <intravenous/node/resources.h>
#include <intravenous/module/builder_session.h>
#include <intravenous/module/package_definitions.h>
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
constexpr char graph_jit_reused_sample_arena_module_id[] = "iv.test.graph_jit.state_context.reused_sample_arena_module";
constexpr char graph_jit_sample_fanout_conversion_module_id[] = "iv.test.graph_jit.state_context.sample_fanout_conversion_module";
constexpr char graph_jit_stereo_conversion_module_id[] = "iv.test.graph_jit.state_context.stereo_conversion_module";
constexpr char graph_jit_history_fanout_module_id[] = "iv.test.graph_jit.state_context.history_fanout_module";
constexpr char graph_jit_persistent_history_module_id[] = "iv.test.graph_jit.state_context.persistent_history_module";
constexpr char graph_jit_latency_compensation_module_id[] = "iv.test.graph_jit.state_context.latency_compensation_module";
constexpr char graph_jit_latency_conversion_fanout_module_id[] = "iv.test.graph_jit.state_context.latency_conversion_fanout_module";
constexpr char graph_jit_composed_latency_module_id[] = "iv.test.graph_jit.state_context.composed_latency_module";
constexpr char graph_jit_composed_history_module_id[] = "iv.test.graph_jit.state_context.composed_history_module";
constexpr char graph_jit_projected_composition_module_id[] = "iv.test.graph_jit.state_context.projected_composition_module";
constexpr char graph_jit_direct_event_module_id[] = "iv.test.graph_jit.state_context.direct_event_module";
constexpr char graph_jit_transient_event_module_id[] = "iv.test.graph_jit.state_context.transient_event_module";
constexpr char graph_jit_converted_event_fanout_module_id[] = "iv.test.graph_jit.state_context.converted_event_fanout_module";
constexpr char graph_jit_retained_event_module_id[] = "iv.test.graph_jit.state_context.retained_event_module";
constexpr char graph_jit_persistent_event_ring_module_id[] = "iv.test.graph_jit.state_context.persistent_event_ring_module";
constexpr char graph_jit_retained_converted_event_fanout_module_id[] = "iv.test.graph_jit.state_context.retained_converted_event_fanout_module";
constexpr char graph_jit_event_feedback_a_id[] = "iv.test.graph_jit.state_context.event_feedback_a";
constexpr char graph_jit_event_feedback_b_id[] = "iv.test.graph_jit.state_context.event_feedback_b";
constexpr char graph_jit_event_feedback_burst_a_id[] = "iv.test.graph_jit.state_context.event_feedback_burst_a";
constexpr char graph_jit_event_feedback_fanout_a_id[] = "iv.test.graph_jit.state_context.event_feedback_fanout_a";

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

struct StereoSampleConsumerProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
    float first_left = 0.0f;
    float first_right = 0.0f;
    float last_left = 0.0f;
    float last_right = 0.0f;
    float sum_left = 0.0f;
    float sum_right = 0.0f;
};

struct HistoryRampSourceStateMirror {
    std::uint64_t calls = 0;
    float previous_output = 0.0f;
    std::uint32_t marker = 0;
};

struct HistoryConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    float current = 0.0f;
    float history_1 = 0.0f;
    float history_5 = 0.0f;
    std::uint32_t marker = 0;
};

struct StereoHistoryConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    float current_left = 0.0f;
    float current_right = 0.0f;
    float history_5_left = 0.0f;
    float history_5_right = 0.0f;
    std::uint64_t marker = 0;
};

struct LargeHistoryConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    float current = 0.0f;
    float history_1 = 0.0f;
    float history_5000 = 0.0f;
};

struct LatencyCompensationProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
    std::uint64_t mismatches = 0;
    float fast_first = 0.0f;
    float slow_first = 0.0f;
    float fast_last = 0.0f;
    float slow_last = 0.0f;
    float max_abs_difference = 0.0f;
    std::uint32_t marker = 0;
};

struct EventConsumerProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
    std::uint64_t event_count = 0;
    std::uint64_t trigger_count = 0;
    std::uint64_t first_time = 0;
    std::uint64_t last_time = 0;
    std::uint32_t marker = 0;
};

struct EventFeedbackAStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t scc_feedback_latency = 0;
    std::array<std::uint64_t, 24> indices{};
    std::array<std::uint64_t, 24> block_sizes{};
    std::array<std::uint64_t, 24> input_counts{};
    std::array<std::uint64_t, 24> first_input_times{};
    std::uint32_t marker = 0;
};

struct EventFeedbackBStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t scc_feedback_latency = 0;
    std::array<std::uint64_t, 24> indices{};
    std::array<std::uint64_t, 24> block_sizes{};
    std::array<std::uint64_t, 24> input_counts{};
    std::array<std::uint64_t, 24> first_input_times{};
    std::uint64_t marker = 0;
    std::uint64_t distinct_padding = 0;
};

struct SlicedEventConsumerProbeStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 4> indices{};
    std::array<std::uint64_t, 4> block_sizes{};
    std::array<std::uint64_t, 4> event_counts{};
    std::array<std::uint64_t, 4> trigger_counts{};
    std::array<std::uint64_t, 4> first_times{};
    std::array<std::uint64_t, 4> last_times{};
};

struct RetainedEventConsumerProbeStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 3> indices{};
    std::array<std::uint64_t, 3> event_counts{};
    std::array<std::uint64_t, 3> first_times{};
    std::array<std::uint64_t, 3> second_times{};
    std::array<std::uint64_t, 3> last_times{};
};

struct PersistentEventRingConsumerProbeStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 5> indices{};
    std::array<std::uint64_t, 5> event_counts{};
    std::array<std::uint64_t, 5> first_times{};
    std::array<std::uint64_t, 5> last_times{};
};

struct RetainedMidiConsumerProbeStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 4> indices{};
    std::array<std::uint64_t, 4> event_counts{};
    std::array<std::uint64_t, 4> midi_counts{};
    std::array<std::uint64_t, 4> first_times{};
    std::array<std::uint64_t, 4> last_times{};
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

TEST(GraphJitEventFeedbackRuntime, AppendsEachSourceSuffixExactlyOnce)
{
    std::array<iv::TimedEvent, 4> const source{
        iv::TimedEvent{.time = 0, .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = 1, .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = 4, .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = 5, .value = iv::TriggerEvent{}},
    };
    std::array<iv::TimedEvent, 8> ring{};
    std::size_t read_index = 0;
    std::size_t write_index = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback(
        source.data(), 0, 2, 0, 5,
        ring.data(), ring.size(), &read_index, &write_index);
    iv::graph_jit::detail::iv_graph_jit_append_event_feedback(
        source.data(), 2, source.size(), 4, 5,
        ring.data(), ring.size(), &read_index, &write_index);

    EXPECT_EQ(read_index, 0u);
    ASSERT_EQ(write_index, source.size());
    EXPECT_EQ(ring[0].time, 5u);
    EXPECT_EQ(ring[1].time, 6u);
    EXPECT_EQ(ring[2].time, 9u);
    EXPECT_EQ(ring[3].time, 10u);
}

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

TEST(GraphJitTransientArenaPlan, PartitionsOneDeadLargeRangeAmongLaterSmallRanges)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{
            .size_bytes = 1024,
            .alignment = 64,
            .live_interval = {.begin = 0, .end = 0},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 256,
            .alignment = 64,
            .live_interval = {.begin = 1, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 256,
            .alignment = 64,
            .live_interval = {.begin = 1, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 256,
            .alignment = 64,
            .live_interval = {.begin = 1, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 256,
            .alignment = 64,
            .live_interval = {.begin = 1, .end = 2},
        },
    };

    auto plan = plan_transient_arena(requests);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->allocations.size(), requests.size());
    EXPECT_EQ(plan->allocations[0].offset, 0u);
    EXPECT_EQ(plan->allocations[1].offset, 0u);
    EXPECT_EQ(plan->allocations[2].offset, 256u);
    EXPECT_EQ(plan->allocations[3].offset, 512u);
    EXPECT_EQ(plan->allocations[4].offset, 768u);
    // The old 1024-byte range is partitioned among the four later values;
    // historical slot width does not add another 3 * 256 bytes.
    EXPECT_EQ(plan->size_bytes, 1024u);
}

TEST(GraphJitTransientArenaPlan, ReusesAlignedHoleBetweenStillLiveRanges)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{
            .size_bytes = 16,
            .alignment = 64,
            .live_interval = {.begin = 0, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 16,
            .alignment = 64,
            .live_interval = {.begin = 0, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 32,
            .alignment = 16,
            .live_interval = {.begin = 1, .end = 1},
        },
    };

    auto plan = plan_transient_arena(requests);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->allocations.size(), requests.size());
    EXPECT_EQ(plan->allocations[0].offset, 0u);
    EXPECT_EQ(plan->allocations[1].offset, 64u);
    // The third allocation fits in the alignment hole [16, 64) while both
    // surrounding ranges are still live; a whole-slot allocator would append.
    EXPECT_EQ(plan->allocations[2].offset, 16u);
    EXPECT_EQ(plan->size_bytes, 80u);
    EXPECT_EQ(plan->alignment, 64u);
}

TEST(GraphJitTransientArenaPlan, CoalescesAllExpiredByteRangesImplicitly)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{
            .size_bytes = 64,
            .alignment = 64,
            .live_interval = {.begin = 0, .end = 0},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 64,
            .alignment = 64,
            .live_interval = {.begin = 0, .end = 0},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 128,
            .alignment = 64,
            .live_interval = {.begin = 1, .end = 1},
        },
    };

    auto plan = plan_transient_arena(requests);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->allocations.size(), requests.size());
    EXPECT_EQ(plan->allocations[0].offset, 0u);
    EXPECT_EQ(plan->allocations[1].offset, 64u);
    // Both adjacent ranges have expired, so the later 128-byte allocation
    // reuses their combined [0, 128) range without explicit free-list merging.
    EXPECT_EQ(plan->allocations[2].offset, 0u);
    EXPECT_EQ(plan->size_bytes, 128u);
}

TEST(GraphJitTransientArenaPlan, InclusiveLifetimeEndpointsNeverAlias)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{
            .size_bytes = 64,
            .alignment = 16,
            .live_interval = {.begin = 0, .end = 1},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 64,
            .alignment = 16,
            .live_interval = {.begin = 1, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 64,
            .alignment = 16,
            .live_interval = {.begin = 2, .end = 3},
        },
    };

    auto plan = plan_transient_arena(requests);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    EXPECT_EQ(plan->allocations[0].offset, 0u);
    EXPECT_EQ(plan->allocations[1].offset, 64u);
    // Request 0 is dead by begin=2 and can be reused, while request 1 still
    // overlaps request 2 at schedule position 2.
    EXPECT_EQ(plan->allocations[2].offset, 0u);
    EXPECT_EQ(plan->size_bytes, 128u);
}

TEST(GraphJitTransientArenaPlan, EqualStartRequestsPlaceLargestFirst)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{
            .size_bytes = 16,
            .alignment = 16,
            .live_interval = {.begin = 0, .end = 1},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 64,
            .alignment = 64,
            .live_interval = {.begin = 0, .end = 1},
        },
    };

    auto plan = plan_transient_arena(requests);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    // Size/alignment-first ordering avoids placing the 64-byte range at offset
    // 64 after a small allocation and therefore keeps the high-water mark 80.
    EXPECT_EQ(plan->allocations[1].offset, 0u);
    EXPECT_EQ(plan->allocations[0].offset, 64u);
    EXPECT_EQ(plan->size_bytes, 80u);
}

TEST(GraphJitTransientArenaPlan, IsDeterministicAndRejectsPersistentLifetime)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{
            .size_bytes = 96,
            .alignment = 32,
            .live_interval = {.begin = 3, .end = 5},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 24,
            .alignment = 8,
            .live_interval = {.begin = 0, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 40,
            .alignment = 16,
            .live_interval = {.begin = 3, .end = 4},
        },
    };
    auto first = plan_transient_arena(requests);
    auto second = plan_transient_arena(requests);
    ASSERT_TRUE(first.has_value()) << (first ? std::string{} : first.error());
    ASSERT_TRUE(second.has_value()) << (second ? std::string{} : second.error());
    EXPECT_EQ(*first, *second);

    requests[0].live_interval.crosses_kernel_invocations = true;
    auto persistent = plan_transient_arena(requests);
    ASSERT_FALSE(persistent.has_value());
    EXPECT_NE(persistent.error().find("cross-kernel"), std::string::npos);
}

TEST(GraphJitTransientArenaPlan, RejectsMalformedRequestsAndAcceptsEmptyArena)
{
    using namespace iv::graph_jit::detail;

    std::array<TransientArenaAllocationRequest, 0> empty_requests{};
    auto empty = plan_transient_arena(empty_requests);
    ASSERT_TRUE(empty.has_value()) << (empty ? std::string{} : empty.error());
    EXPECT_TRUE(empty->allocations.empty());
    EXPECT_EQ(empty->size_bytes, 0u);
    EXPECT_EQ(empty->alignment, 1u);

    std::array malformed{
        TransientArenaAllocationRequest{
            .size_bytes = 0,
            .alignment = 8,
            .live_interval = {.begin = 0, .end = 0},
        },
    };
    EXPECT_FALSE(plan_transient_arena(malformed).has_value());

    malformed[0].size_bytes = 8;
    malformed[0].alignment = 3;
    EXPECT_FALSE(plan_transient_arena(malformed).has_value());

    malformed[0].alignment = 8;
    malformed[0].live_interval = {.begin = 2, .end = 1};
    EXPECT_FALSE(plan_transient_arena(malformed).has_value());
}

TEST(GraphJitTransientArenaPlan, ComplexPackingPreservesAllSafetyInvariants)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{64, 64, {.begin = 0, .end = 2}},
        TransientArenaAllocationRequest{24, 8, {.begin = 0, .end = 0}},
        TransientArenaAllocationRequest{48, 16, {.begin = 1, .end = 3}},
        TransientArenaAllocationRequest{80, 32, {.begin = 3, .end = 4}},
        TransientArenaAllocationRequest{16, 16, {.begin = 1, .end = 1}},
        TransientArenaAllocationRequest{96, 32, {.begin = 5, .end = 6}},
        TransientArenaAllocationRequest{32, 8, {.begin = 4, .end = 5}},
    };

    auto plan = plan_transient_arena(requests);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->allocations.size(), requests.size());

    std::size_t observed_high_water = 0;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        auto const& allocation = plan->allocations[i];
        EXPECT_EQ(allocation.size_bytes, requests[i].size_bytes);
        EXPECT_EQ(allocation.alignment, requests[i].alignment);
        EXPECT_EQ(allocation.offset % allocation.alignment, 0u);
        ASSERT_LE(allocation.offset, plan->size_bytes);
        ASSERT_LE(allocation.size_bytes, plan->size_bytes - allocation.offset);
        observed_high_water = std::max(
            observed_high_water, allocation.offset + allocation.size_bytes);

        for (std::size_t j = i + 1; j < requests.size(); ++j) {
            auto const& a_live = requests[i].live_interval;
            auto const& b_live = requests[j].live_interval;
            auto const lifetimes_overlap =
                !(a_live.end < b_live.begin || b_live.end < a_live.begin);
            if (!lifetimes_overlap) continue;

            auto const& other = plan->allocations[j];
            auto const byte_ranges_overlap =
                !(allocation.offset + allocation.size_bytes <= other.offset
                    || other.offset + other.size_bytes <= allocation.offset);
            EXPECT_FALSE(byte_ranges_overlap)
                << "overlapping lifetimes " << i << " and " << j
                << " were assigned overlapping arena bytes";
        }
    }
    EXPECT_EQ(plan->size_bytes, observed_high_water);
}

TEST(GraphJitSamplePhysicalPlan, PacksExactTransientByteRangesAcrossLifetimes)
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
                    std::size_t begin,
                    std::size_t end) {
        SampleProducerGroupPlan result;
        result.canonical_source_layout = layout;
        result.has_realtime_connections = true;
        result.implementation = iv::SampleConnectionImplementationKind::direct;
        result.live_interval = ConnectionLiveIntervalPlan{
            .begin = begin,
            .end = end,
        };
        return result;
    };

    ConnectionAnalysisPlan connections;
    // A large early value, followed by two simultaneous half-sized values. The
    // latter should split the dead large range instead of reserving one large
    // historical slot plus a second small slot.
    connections.sample_producer_groups.push_back(group(stereo, 0, 0));
    connections.sample_producer_groups.push_back(group(mono, 1, 2));
    connections.sample_producer_groups.push_back(group(mono, 1, 2));

    auto physical = build_sample_physical_plan(connections, 64);
    ASSERT_TRUE(physical.has_value())
        << (physical ? std::string{} : physical.error());
    ASSERT_EQ(physical->representations.size(), 3u);
    ASSERT_EQ(physical->transient_allocations.size(), 3u);

    for (auto const& producer : physical->producer_groups) {
        ASSERT_TRUE(producer.has_value());
        ASSERT_LT(
            producer->canonical_representation,
            physical->representations.size());
        auto const allocation = physical->representations[
            producer->canonical_representation].transient_allocation;
        ASSERT_NE(allocation, no_sample_transient_allocation);
        ASSERT_LT(allocation, physical->transient_allocations.size());
    }
    auto allocation_for_group = [&](std::size_t group_index)
        -> SampleTransientAllocationPlan const& {
        auto const representation =
            physical->producer_groups[group_index]->canonical_representation;
        auto const allocation =
            physical->representations[representation].transient_allocation;
        return physical->transient_allocations[allocation];
    };
    auto const& large = allocation_for_group(0);
    auto const& small_a = allocation_for_group(1);
    auto const& small_b = allocation_for_group(2);
    auto const mono_bytes = 64u * sizeof(iv::Sample);
    auto const stereo_bytes = 2u * mono_bytes;

    EXPECT_EQ(large.size_bytes, stereo_bytes);
    EXPECT_EQ(large.region_relative_offset, 0u);
    EXPECT_EQ(small_a.size_bytes, mono_bytes);
    EXPECT_EQ(small_b.size_bytes, mono_bytes);
    EXPECT_EQ(small_a.region_relative_offset, 0u);
    EXPECT_EQ(small_b.region_relative_offset, mono_bytes);
    EXPECT_EQ(physical->transient_arena_size, stereo_bytes);

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
    EXPECT_EQ(region.size, stereo_bytes);
    EXPECT_EQ(large.storage_offset, region.storage_offset);
    EXPECT_EQ(small_a.storage_offset, region.storage_offset);
    EXPECT_EQ(small_b.storage_offset, region.storage_offset + mono_bytes);
}

TEST(GraphJitSamplePhysicalPlan, LeavesCompiledAccessBranchesUnresolved)
{
    using namespace iv::graph_jit::detail;

    ConnectionAnalysisPlan connections;
    SampleConnectionPlan realtime;
    realtime.access = PlannedConnectionAccess::realtime_to_realtime;
    realtime.target_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
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

TEST(GraphJitSamplePhysicalPlan, BuildsAndDeduplicatesDerivedConvertedFanout)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::ChannelLayout const stereo_interleaved{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    };

    ConnectionAnalysisPlan connections;
    SampleConnectionPlan identity;
    identity.access = PlannedConnectionAccess::realtime_to_realtime;
    identity.canonical_source_layout = mono;
    identity.target_layout = mono;
    identity.target_port = iv::NodeBundlePortId{2, iv::PortKind::sample, 0};
    connections.sample_connections.push_back(identity);

    SampleConnectionPlan converted_a;
    converted_a.access = PlannedConnectionAccess::realtime_to_realtime;
    converted_a.canonical_source_layout = mono;
    converted_a.target_layout = stereo_interleaved;
    converted_a.target_port = iv::NodeBundlePortId{3, iv::PortKind::sample, 0};
    converted_a.requires_conversion = true;
    connections.sample_connections.push_back(converted_a);

    auto converted_b = converted_a;
    converted_b.target_port = iv::NodeBundlePortId{4, iv::PortKind::sample, 0};
    connections.sample_connections.push_back(converted_b);

    connections.schedule.bundle_execution_position.resize(5);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;
    connections.schedule.bundle_execution_position[3] = 2;
    connections.schedule.bundle_execution_position[4] = 3;

    SampleProducerGroupPlan group;
    group.canonical_source_layout = mono;
    group.connection_indices = {0, 1, 2};
    group.has_realtime_connections = true;
    group.implementation =
        iv::SampleConnectionImplementationKind::transient_materialization;
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 3,
    };
    connections.sample_producer_groups.push_back(std::move(group));

    auto physical = build_sample_physical_plan(connections, 64);
    ASSERT_TRUE(physical.has_value())
        << (physical ? std::string{} : physical.error());
    ASSERT_EQ(physical->representations.size(), 2u);
    ASSERT_EQ(physical->materializations.size(), 1u);
    ASSERT_EQ(physical->connection_representations.size(), 3u);
    ASSERT_TRUE(physical->producer_groups[0].has_value());
    auto const canonical =
        physical->producer_groups[0]->canonical_representation;
    ASSERT_TRUE(physical->connection_representations[0].has_value());
    ASSERT_TRUE(physical->connection_representations[1].has_value());
    ASSERT_TRUE(physical->connection_representations[2].has_value());
    auto const derived = *physical->connection_representations[1];
    EXPECT_EQ(*physical->connection_representations[0], canonical);
    EXPECT_EQ(*physical->connection_representations[2], derived);
    EXPECT_NE(derived, canonical);
    EXPECT_TRUE(physical->representations[canonical]
                    .canonical_producer_representation);
    EXPECT_FALSE(physical->representations[derived]
                     .canonical_producer_representation);
    EXPECT_EQ(physical->representations[canonical].channel_layout, mono);
    EXPECT_EQ(
        physical->representations[derived].channel_layout,
        stereo_interleaved);
    EXPECT_EQ(physical->representations[canonical].live_interval.begin, 0u);
    EXPECT_EQ(physical->representations[canonical].live_interval.end, 1u);
    EXPECT_EQ(physical->representations[derived].live_interval.begin, 0u);
    EXPECT_EQ(physical->representations[derived].live_interval.end, 3u);

    auto const& materialization = physical->materializations.front();
    EXPECT_EQ(materialization.source_representation, canonical);
    EXPECT_EQ(materialization.target_representation, derived);
    EXPECT_EQ(materialization.after_execution_position, 0u);
    EXPECT_EQ(materialization.source_layout, mono);
    EXPECT_EQ(materialization.target_layout, stereo_interleaved);

    // Source and derived values overlap at the conversion point and therefore
    // must occupy distinct byte ranges. Two consumers of the same converted
    // layout share the one derived representation rather than duplicating it.
    auto const canonical_allocation = physical->representations[canonical]
        .transient_allocation;
    auto const derived_allocation = physical->representations[derived]
        .transient_allocation;
    ASSERT_LT(canonical_allocation, physical->transient_allocations.size());
    ASSERT_LT(derived_allocation, physical->transient_allocations.size());
    auto const& canonical_range =
        physical->transient_allocations[canonical_allocation];
    auto const& derived_range =
        physical->transient_allocations[derived_allocation];
    auto const canonical_end = canonical_range.region_relative_offset
        + canonical_range.size_bytes;
    auto const derived_end = derived_range.region_relative_offset
        + derived_range.size_bytes;
    EXPECT_TRUE(canonical_end <= derived_range.region_relative_offset
        || derived_end <= canonical_range.region_relative_offset);
    EXPECT_EQ(
        physical->transient_arena_size,
        3u * 64u * sizeof(iv::Sample));
}

TEST(GraphJitSamplePhysicalPlan, RealizesCompactPersistentCarryExactly)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    ConnectionAnalysisPlan connections;
    SampleProducerGroupPlan group;
    group.canonical_source_layout = mono;
    group.has_realtime_connections = true;
    group.requirements.retained_frames = 7;
    group.requirements.channel_count = 1;
    group.requirements.value_size_bytes = sizeof(iv::Sample);
    group.implementation = iv::SampleConnectionImplementationKind::compact_persistent_carry;
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto physical = build_sample_physical_plan(connections, 64);
    ASSERT_TRUE(physical.has_value())
        << (physical ? std::string{} : physical.error());
    ASSERT_EQ(physical->representations.size(), 1u);
    ASSERT_EQ(physical->transient_allocations.size(), 1u);
    ASSERT_EQ(physical->persistent_allocations.size(), 1u);
    ASSERT_EQ(physical->carry_operations.size(), 1u);
    ASSERT_TRUE(physical->producer_groups[0].has_value());
    auto const representation_index =
        physical->producer_groups[0]->canonical_representation;
    auto const& representation = physical->representations[representation_index];
    EXPECT_EQ(representation.frame_capacity, 128u);
    EXPECT_NE(
        representation.transient_allocation,
        no_sample_transient_allocation);
    EXPECT_NE(
        representation.persistent_allocation,
        no_sample_persistent_allocation);

    auto const& persistent = physical->persistent_allocations[
        representation.persistent_allocation];
    EXPECT_EQ(persistent.kind, SamplePersistentStorageKind::compact_carry);
    EXPECT_EQ(persistent.retained_frames, 7u);
    EXPECT_EQ(persistent.frame_capacity, 128u);
    EXPECT_EQ(persistent.size_bytes, 7u * sizeof(iv::Sample));
    EXPECT_FALSE(persistent.migration_identity.empty());
    EXPECT_EQ(
        physical->carry_operations[0].persistent_allocation,
        representation.persistent_allocation);
    EXPECT_EQ(physical->carry_operations[0].retained_frames, 7u);

    iv::NodeLayoutBuilder builder(64);
    auto declared = declare_sample_physical_storage(builder, *physical);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_physical_storage(layout, *physical);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    ASSERT_EQ(layout.regions.size(), 2u);
    auto const& transient_region = layout.regions[physical->transient_region.index];
    auto const& persistent_region = layout.regions[persistent.region.index];
    EXPECT_TRUE(transient_region.migration_identity.empty());
    EXPECT_EQ(
        persistent_region.migration_identity,
        persistent.migration_identity);
    EXPECT_EQ(persistent_region.size, 7u * sizeof(iv::Sample));
}

TEST(GraphJitSamplePhysicalPlan, RealizesLargeRetentionAsPersistentRing)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    ConnectionAnalysisPlan connections;
    SampleProducerGroupPlan group;
    group.canonical_source_layout = mono;
    group.has_realtime_connections = true;
    group.requirements.retained_frames = 5000;
    group.requirements.channel_count = 1;
    group.requirements.value_size_bytes = sizeof(iv::Sample);
    group.implementation = iv::SampleConnectionImplementationKind::persistent_ring;
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto first = build_sample_physical_plan(connections, 64);
    auto second = build_sample_physical_plan(connections, 64);
    ASSERT_TRUE(first.has_value())
        << (first ? std::string{} : first.error());
    ASSERT_TRUE(second.has_value())
        << (second ? std::string{} : second.error());
    ASSERT_EQ(first->representations.size(), 1u);
    ASSERT_TRUE(first->transient_allocations.empty());
    ASSERT_EQ(first->persistent_allocations.size(), 1u);
    EXPECT_TRUE(first->carry_operations.empty());
    auto const representation_index =
        first->producer_groups[0]->canonical_representation;
    auto const& representation = first->representations[representation_index];
    EXPECT_EQ(representation.frame_capacity, 8192u);
    EXPECT_EQ(
        representation.transient_allocation,
        no_sample_transient_allocation);
    ASSERT_NE(
        representation.persistent_allocation,
        no_sample_persistent_allocation);
    auto const& persistent = first->persistent_allocations[
        representation.persistent_allocation];
    EXPECT_EQ(persistent.kind, SamplePersistentStorageKind::ring);
    EXPECT_EQ(persistent.retained_frames, 5000u);
    EXPECT_EQ(persistent.frame_capacity, 8192u);
    EXPECT_EQ(persistent.size_bytes, 8192u * sizeof(iv::Sample));
    EXPECT_EQ(
        persistent.migration_identity,
        second->persistent_allocations[0].migration_identity);

    iv::NodeLayoutBuilder builder(64);
    auto declared = declare_sample_physical_storage(builder, *first);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_physical_storage(layout, *first);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    ASSERT_EQ(layout.regions.size(), 1u);
    EXPECT_EQ(layout.regions[0].size, 8192u * sizeof(iv::Sample));
    EXPECT_FALSE(layout.regions[0].migration_identity.empty());
}

TEST(GraphJitSamplePhysicalPlan, ConvertedRetentionMaterializesHistoricalWindow)
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

    ConnectionAnalysisPlan connections;
    SampleConnectionPlan converted;
    converted.access = PlannedConnectionAccess::realtime_to_realtime;
    converted.canonical_source_layout = mono;
    converted.target_layout = stereo;
    converted.target_port = iv::NodeBundlePortId{2, iv::PortKind::sample, 0};
    converted.source_latency = 2;
    converted.read_latency = 2;
    converted.target_history = 5;
    converted.requires_conversion = true;
    connections.sample_connections.push_back(converted);
    connections.schedule.bundle_execution_position.resize(3);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;

    SampleProducerGroupPlan group;
    group.canonical_source_layout = mono;
    group.connection_indices = {0};
    group.has_realtime_connections = true;
    group.requirements.retained_frames = 7;
    group.requirements.channel_count = 1;
    group.requirements.value_size_bytes = sizeof(iv::Sample);
    group.implementation = iv::SampleConnectionImplementationKind::compact_persistent_carry;
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto physical = build_sample_physical_plan(connections, 64);
    ASSERT_TRUE(physical.has_value())
        << (physical ? std::string{} : physical.error());
    ASSERT_EQ(physical->representations.size(), 2u);
    ASSERT_EQ(physical->materializations.size(), 1u);
    ASSERT_EQ(physical->carry_operations.size(), 1u);
    auto const canonical =
        physical->producer_groups[0]->canonical_representation;
    auto const derived = *physical->connection_representations[0];
    ASSERT_NE(canonical, derived);
    EXPECT_EQ(physical->representations[canonical].frame_capacity, 128u);
    EXPECT_EQ(physical->representations[derived].frame_capacity, 128u);
    auto const& materialization = physical->materializations[0];
    EXPECT_EQ(materialization.source_representation, canonical);
    EXPECT_EQ(materialization.target_representation, derived);
    EXPECT_EQ(materialization.retained_before, 7u);
    EXPECT_EQ(materialization.latest_read_latency, 2u);

    auto const canonical_allocation = physical->representations[canonical]
        .transient_allocation;
    auto const derived_allocation = physical->representations[derived]
        .transient_allocation;
    ASSERT_LT(canonical_allocation, physical->transient_allocations.size());
    ASSERT_LT(derived_allocation, physical->transient_allocations.size());
    auto const& source_range = physical->transient_allocations[canonical_allocation];
    auto const& target_range = physical->transient_allocations[derived_allocation];
    auto const source_end = source_range.region_relative_offset + source_range.size_bytes;
    auto const target_end = target_range.region_relative_offset + target_range.size_bytes;
    EXPECT_TRUE(source_end <= target_range.region_relative_offset
        || target_end <= source_range.region_relative_offset);
}

TEST(GraphJitSamplePhysicalPlan, SharedConvertedFanoutMaterializesUnionOfReadWindows)
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

    ConnectionAnalysisPlan connections;
    SampleConnectionPlan compensated;
    compensated.access = PlannedConnectionAccess::realtime_to_realtime;
    compensated.canonical_source_layout = mono;
    compensated.target_layout = stereo;
    compensated.target_port = iv::NodeBundlePortId{2, iv::PortKind::sample, 0};
    compensated.read_latency = 7;
    compensated.requires_conversion = true;
    connections.sample_connections.push_back(compensated);

    SampleConnectionPlan current = compensated;
    current.target_port = iv::NodeBundlePortId{3, iv::PortKind::sample, 0};
    current.read_latency = 0;
    connections.sample_connections.push_back(current);

    connections.schedule.bundle_execution_position.resize(4);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;
    connections.schedule.bundle_execution_position[3] = 2;

    SampleProducerGroupPlan group;
    group.canonical_source_layout = mono;
    group.connection_indices = {0, 1};
    group.has_realtime_connections = true;
    group.requirements.retained_frames = 7;
    group.requirements.channel_count = 1;
    group.requirements.value_size_bytes = sizeof(iv::Sample);
    group.implementation =
        iv::SampleConnectionImplementationKind::compact_persistent_carry;
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 2,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto physical = build_sample_physical_plan(connections, 64);
    ASSERT_TRUE(physical.has_value())
        << (physical ? std::string{} : physical.error());
    ASSERT_EQ(physical->representations.size(), 2u);
    ASSERT_EQ(physical->materializations.size(), 1u);
    ASSERT_EQ(physical->connection_representations.size(), 2u);
    ASSERT_TRUE(physical->connection_representations[0].has_value());
    ASSERT_TRUE(physical->connection_representations[1].has_value());
    EXPECT_EQ(
        *physical->connection_representations[0],
        *physical->connection_representations[1]);

    auto const& materialization = physical->materializations[0];
    EXPECT_EQ(materialization.retained_before, 7u);
    EXPECT_EQ(materialization.latest_read_latency, 0u);
    EXPECT_EQ(
        physical->representations[materialization.target_representation].frame_capacity,
        128u);
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

namespace {
constexpr char graph_jit_runtime_fixture_name[] = "graph_jit_runtime_shared_package";

std::string_view graph_jit_runtime_package_source()
{
    return R"cpp(
#include <intravenous/dsl.h>

#include <algorithm>
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

struct MonoInterleavedConsumerProbe {
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
        return std::array{iv::realtime_sample_input(
            "in",
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<MonoInterleavedConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const block = ctx.inputs[0].get_block(ctx.block_size);
        ++state.calls;
        state.last_index = ctx.index;
        state.last_block_size = ctx.block_size;
        state.first = block.empty() ? 0.0f : static_cast<float>(block[0]);
        state.last = block.empty()
            ? 0.0f
            : static_cast<float>(block[block.size() - 1]);
        state.sum = 0.0f;
        for (auto const sample : block) state.sum += sample;
    }
};

struct StereoRampSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output(
            "out",
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    void tick_block(iv::TickBlockContext<StereoRampSource> const& ctx) const
    {
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            std::array<iv::Sample, 2> frame{
                static_cast<iv::Sample>(ctx.index + i),
                static_cast<iv::Sample>(ctx.index + i + 1000),
            };
            ctx.outputs[0].push_frame(frame);
        }
    }
};

struct StereoPlanarConsumerProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        float first_left = 0.0f;
        float first_right = 0.0f;
        float last_left = 0.0f;
        float last_right = 0.0f;
        float sum_left = 0.0f;
        float sum_right = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input(
            "in",
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            }})};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<StereoPlanarConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index;
        state.last_block_size = ctx.block_size;
        state.sum_left = 0.0f;
        state.sum_right = 0.0f;
        if (ctx.block_size != 0) {
            state.first_left = static_cast<float>(ctx.inputs[0].get_frame(0, 0));
            state.first_right = static_cast<float>(ctx.inputs[0].get_frame(0, 1));
            state.last_left = static_cast<float>(
                ctx.inputs[0].get_frame(ctx.block_size - 1, 0));
            state.last_right = static_cast<float>(
                ctx.inputs[0].get_frame(ctx.block_size - 1, 1));
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            state.sum_left += static_cast<float>(ctx.inputs[0].get_frame(i, 0));
            state.sum_right += static_cast<float>(ctx.inputs[0].get_frame(i, 1));
        }
    }
};

struct StereoSampleConsumerProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        float first_left = 0.0f;
        float first_right = 0.0f;
        float last_left = 0.0f;
        float last_right = 0.0f;
        float sum_left = 0.0f;
        float sum_right = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input(
            "in",
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<StereoSampleConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index;
        state.last_block_size = ctx.block_size;
        state.sum_left = 0.0f;
        state.sum_right = 0.0f;
        if (ctx.block_size != 0) {
            state.first_left = static_cast<float>(ctx.inputs[0].get_frame(0, 0));
            state.first_right = static_cast<float>(ctx.inputs[0].get_frame(0, 1));
            state.last_left = static_cast<float>(
                ctx.inputs[0].get_frame(ctx.block_size - 1, 0));
            state.last_right = static_cast<float>(
                ctx.inputs[0].get_frame(ctx.block_size - 1, 1));
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            state.sum_left += static_cast<float>(ctx.inputs[0].get_frame(i, 0));
            state.sum_right += static_cast<float>(ctx.inputs[0].get_frame(i, 1));
        }
    }
};

struct HistoryRampSource {
    struct State {
        std::uint64_t calls = 0;
        float previous_output = 0.0f;
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output(
            "out", {}, {.history = 3, .latency = 2})};
    }

    void tick_block(iv::TickBlockContext<HistoryRampSource> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.previous_output = static_cast<float>(ctx.outputs[0].get());
        state.marker = 0x91a2b3c4u;
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            ctx.outputs[0].push(static_cast<iv::Sample>(ctx.index + i));
        }
    }
};

struct HistoryConsumerProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        float current = 0.0f;
        float history_1 = 0.0f;
        float history_5 = 0.0f;
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input(
            "in", {}, {.history = 5})};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<HistoryConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index;
        state.current = static_cast<float>(ctx.inputs[0].get(0));
        state.history_1 = static_cast<float>(ctx.inputs[0].get(1));
        state.history_5 = static_cast<float>(ctx.inputs[0].get(5));
        state.marker = 0x5e6f7788u;
    }
};

struct StereoHistoryConsumerProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        float current_left = 0.0f;
        float current_right = 0.0f;
        float history_5_left = 0.0f;
        float history_5_right = 0.0f;
        std::uint64_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input(
            "in",
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }},
            {.history = 5})};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<StereoHistoryConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index;
        state.current_left = static_cast<float>(ctx.inputs[0].get(0, 0));
        state.current_right = static_cast<float>(ctx.inputs[0].get(0, 1));
        state.history_5_left = static_cast<float>(ctx.inputs[0].get(5, 0));
        state.history_5_right = static_cast<float>(ctx.inputs[0].get(5, 1));
        state.marker = 0xa1b2c3d4e5f60718ull;
    }
};

struct LargeHistoryRampSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<LargeHistoryRampSource> const& ctx) const
    {
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            ctx.outputs[0].push(static_cast<iv::Sample>(ctx.index + i));
        }
    }
};

struct LargeHistoryConsumerProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        float current = 0.0f;
        float history_1 = 0.0f;
        float history_5000 = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input(
            "in", {}, {.history = 5000})};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<LargeHistoryConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index;
        state.current = static_cast<float>(ctx.inputs[0].get(0));
        state.history_1 = static_cast<float>(ctx.inputs[0].get(1));
        state.history_5000 = static_cast<float>(ctx.inputs[0].get(5000));
    }
};

struct FiveSampleDelay {
    struct State {
        std::array<iv::Sample, 8> memory{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output(
            "out", {}, {.latency = 2})};
    }

    constexpr std::size_t internal_latency() const { return 5; }

    void tick_block(iv::TickBlockContext<FiveSampleDelay> const& ctx) const
    {
        auto const input = ctx.inputs[0].get_block(ctx.block_size);
        auto& memory = ctx.state().memory;
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const index = static_cast<std::size_t>(ctx.index + i);
            auto const delayed = memory[index & 7u];
            memory[(index + 5u) & 7u] = input[i];
            ctx.outputs[0].push(delayed);
        }
    }
};

struct LatencyCompensationProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        std::uint64_t mismatches = 0;
        float fast_first = 0.0f;
        float slow_first = 0.0f;
        float fast_last = 0.0f;
        float slow_last = 0.0f;
        float max_abs_difference = 0.0f;
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_sample_input("fast"),
            iv::realtime_sample_input("slow"),
        };
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<LatencyCompensationProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const fast = ctx.inputs[0].get_block(ctx.block_size);
        auto const slow = ctx.inputs[1].get_block(ctx.block_size);
        ++state.calls;
        state.last_index = ctx.index;
        state.last_block_size = ctx.block_size;
        state.marker = 0x71ac0deu;
        if (ctx.block_size != 0) {
            state.fast_first = fast[0];
            state.slow_first = slow[0];
            state.fast_last = fast[ctx.block_size - 1];
            state.slow_last = slow[ctx.block_size - 1];
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto difference = static_cast<float>(fast[i] - slow[i]);
            if (difference < 0.0f) difference = -difference;
            if (difference != 0.0f) ++state.mismatches;
            if (difference > state.max_abs_difference) {
                state.max_abs_difference = difference;
            }
        }
    }
};

struct InterleavedLatencyCompensationProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        std::uint64_t mismatches = 0;
        float fast_first = 0.0f;
        float slow_first = 0.0f;
        float fast_last = 0.0f;
        float slow_last = 0.0f;
        float max_abs_difference = 0.0f;
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_sample_input(
                "fast",
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::interleaved,
                }}),
            iv::realtime_sample_input(
                "slow",
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::interleaved,
                }}),
        };
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(
        iv::TickBlockContext<InterleavedLatencyCompensationProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const fast = ctx.inputs[0].get_block(ctx.block_size);
        auto const slow = ctx.inputs[1].get_block(ctx.block_size);
        ++state.calls;
        state.last_index = ctx.index;
        state.last_block_size = ctx.block_size;
        state.marker = 0x1a7e2e0u;
        if (ctx.block_size != 0) {
            state.fast_first = fast[0];
            state.slow_first = slow[0];
            state.fast_last = fast[ctx.block_size - 1];
            state.slow_last = slow[ctx.block_size - 1];
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto difference = static_cast<float>(fast[i] - slow[i]);
            if (difference < 0.0f) difference = -difference;
            if (difference != 0.0f) ++state.mismatches;
            if (difference > state.max_abs_difference) {
                state.max_abs_difference = difference;
            }
        }
    }
};

struct TriggerEventSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::realtime_event_output("trigger", iv::EventTypeId::trigger),
        };
    }

    void tick_block(iv::TickBlockContext<TriggerEventSource> const& ctx) const
    {
        if (ctx.block_size == 0) return;
        ctx.event_outputs[0].push(
            iv::TriggerEvent{}, 3, ctx.index, ctx.block_size);
        ctx.event_outputs[0].push(
            iv::TriggerEvent{}, ctx.block_size - 1, ctx.index, ctx.block_size);
    }
};

struct MidiEventSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::realtime_event_output("midi", iv::EventTypeId::midi),
        };
    }

    void tick_block(iv::TickBlockContext<MidiEventSource> const& ctx) const
    {
        if (ctx.block_size < 18) return;

        iv::MidiEvent note_on_a{};
        note_on_a.bytes = {0x90, 60, 100};
        note_on_a.size = 3;
        iv::MidiEvent note_off{};
        note_off.bytes = {0x80, 60, 0};
        note_off.size = 3;
        iv::MidiEvent note_on_b{};
        note_on_b.bytes = {0x90, 64, 96};
        note_on_b.size = 3;

        ctx.event_outputs[0].push(
            note_on_a, 5, ctx.index, ctx.block_size);
        ctx.event_outputs[0].push(
            note_off, 9, ctx.index, ctx.block_size);
        ctx.event_outputs[0].push(
            note_on_b, 17, ctx.index, ctx.block_size);
    }
};

struct LimitedTriggerEventSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::realtime_event_output("trigger", iv::EventTypeId::trigger),
        };
    }

    std::size_t max_block_size() const { return 16; }

    void tick_block(iv::TickBlockContext<LimitedTriggerEventSource> const& ctx) const
    {
        if (ctx.block_size == 0) return;
        ctx.event_outputs[0].push(
            iv::TriggerEvent{}, 3, ctx.index, ctx.block_size);
        ctx.event_outputs[0].push(
            iv::TriggerEvent{}, ctx.block_size - 1, ctx.index, ctx.block_size);
    }
};

struct EventFeedbackA {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<std::uint64_t, 24> input_counts{};
        std::array<std::uint64_t, 24> first_input_times{};
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_event_input("in", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_event_output(
            "out",
            iv::EventOutputProperties{
                .type = iv::EventTypeId::trigger,
                .max_events_per_sample = 0.25,
            })};
    }

    void tick_block(iv::TickBlockContext<EventFeedbackA> const& ctx) const
    {
        auto& state = ctx.state();
        auto const slot = static_cast<std::size_t>(state.calls);
        auto const events = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
        if (slot < state.indices.size()) {
            state.indices[slot] = ctx.index;
            state.block_sizes[slot] = ctx.block_size;
            state.input_counts[slot] = events.size();
            state.first_input_times[slot] = events.empty() ? 0 : events[0].time;
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xa11ce001u;
        if (ctx.block_size != 0) {
            auto const offset = std::min<std::size_t>(1, ctx.block_size - 1);
            ctx.event_outputs[0].push(
                iv::TriggerEvent{}, offset, ctx.index, ctx.block_size);
        }
    }
};

struct EventFeedbackBurstA {
    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_event_input("in", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_event_output(
            "out",
            iv::EventOutputProperties{
                .type = iv::EventTypeId::trigger,
                .max_events_per_sample = 0.25,
            })};
    }

    void tick_block(iv::TickBlockContext<EventFeedbackBurstA> const& ctx) const
    {
        if (ctx.block_size != 1) return;
        // max_events_per_sample is a static sizing rate, not a runtime density
        // limit. Fill the entire 64-frame-specialization sequence at one sample
        // to stress persistent feedback capacity across tiny root calls.
        for (std::size_t i = 0; i < 16; ++i) {
            ctx.event_outputs[0].push(
                iv::TriggerEvent{}, 0, ctx.index, ctx.block_size);
        }
    }
};

struct EventFeedbackFanoutA {
    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_event_input("in_a", iv::EventTypeId::trigger),
            iv::realtime_event_input("in_b", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_event_output(
            "out",
            iv::EventOutputProperties{
                .type = iv::EventTypeId::trigger,
                .max_events_per_sample = 0.25,
            })};
    }

    void tick_block(iv::TickBlockContext<EventFeedbackFanoutA> const& ctx) const
    {
        // Force both explicit feed-forward dependencies to remain observable to
        // the package/JIT interfaces even though this probe only needs to emit.
        (void)ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
        (void)ctx.event_inputs[1].get_block(ctx.index, ctx.block_size);
        if (ctx.block_size == 0) return;
        auto const offset = std::min<std::size_t>(1, ctx.block_size - 1);
        ctx.event_outputs[0].push(
            iv::TriggerEvent{}, offset, ctx.index, ctx.block_size);
    }
};

struct EventFeedbackB {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<std::uint64_t, 24> input_counts{};
        std::array<std::uint64_t, 24> first_input_times{};
        std::uint64_t marker = 0;
        std::uint64_t distinct_padding = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_event_input("in", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_event_output(
            "out",
            iv::EventOutputProperties{
                .type = iv::EventTypeId::trigger,
                .max_events_per_sample = 0.25,
            })};
    }

    void tick_block(iv::TickBlockContext<EventFeedbackB> const& ctx) const
    {
        auto& state = ctx.state();
        auto const slot = static_cast<std::size_t>(state.calls);
        auto const events = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
        if (slot < state.indices.size()) {
            state.indices[slot] = ctx.index;
            state.block_sizes[slot] = ctx.block_size;
            state.input_counts[slot] = events.size();
            state.first_input_times[slot] = events.empty() ? 0 : events[0].time;
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xb22ce002ull;
        if (ctx.block_size != 0) {
            auto const offset = std::min<std::size_t>(2, ctx.block_size - 1);
            ctx.event_outputs[0].push(
                iv::TriggerEvent{}, offset, ctx.index, ctx.block_size);
        }
    }
};

struct TriggerEventConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        std::uint64_t event_count = 0;
        std::uint64_t trigger_count = 0;
        std::uint64_t first_time = 0;
        std::uint64_t last_time = 0;
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_event_input("trigger", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<TriggerEventConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const events = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
        ++state.calls;
        state.last_index = ctx.index;
        state.last_block_size = ctx.block_size;
        state.event_count = events.size();
        state.trigger_count = 0;
        state.marker = 0xe71e17u;
        if (!events.empty()) {
            state.first_time = events[0].time;
            state.last_time = events[events.size() - 1].time;
        }
        for (auto const& event : events) {
            if (std::holds_alternative<iv::TriggerEvent>(event.value)) {
                ++state.trigger_count;
            }
        }
    }
};

struct RetainedMidiEventConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 4> indices{};
        std::array<std::uint64_t, 4> event_counts{};
        std::array<std::uint64_t, 4> midi_counts{};
        std::array<std::uint64_t, 4> first_times{};
        std::array<std::uint64_t, 4> last_times{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_event_input(
                "midi",
                iv::EventTypeId::midi,
                iv::RealtimeInputConfig{.history = 160}),
        };
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<RetainedMidiEventConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const slot = state.calls++;
        if (slot >= state.indices.size()) return;
        auto const history = ctx.index < 160 ? ctx.index : std::size_t{160};
        auto const events = ctx.event_inputs[0].get_block(
            ctx.index - history,
            ctx.block_size + history);
        state.indices[slot] = ctx.index;
        state.event_counts[slot] = events.size();
        state.midi_counts[slot] = 0;
        if (!events.empty()) {
            state.first_times[slot] = events[0].time;
            state.last_times[slot] = events[events.size() - 1].time;
        }
        for (auto const& event : events) {
            if (std::holds_alternative<iv::MidiEvent>(event.value)) {
                ++state.midi_counts[slot];
            }
        }
    }
};

struct LimitedTriggerEventConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 4> indices{};
        std::array<std::uint64_t, 4> block_sizes{};
        std::array<std::uint64_t, 4> event_counts{};
        std::array<std::uint64_t, 4> trigger_counts{};
        std::array<std::uint64_t, 4> first_times{};
        std::array<std::uint64_t, 4> last_times{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_event_input("trigger", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    std::size_t max_block_size() const { return 32; }

    void tick_block(iv::TickBlockContext<LimitedTriggerEventConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const slot = state.calls++;
        if (slot >= state.indices.size()) return;
        auto const events = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
        state.indices[slot] = ctx.index;
        state.block_sizes[slot] = ctx.block_size;
        state.event_counts[slot] = events.size();
        state.trigger_counts[slot] = 0;
        if (!events.empty()) {
            state.first_times[slot] = events[0].time;
            state.last_times[slot] = events[events.size() - 1].time;
        }
        for (auto const& event : events) {
            if (std::holds_alternative<iv::TriggerEvent>(event.value)) {
                ++state.trigger_counts[slot];
            }
        }
    }
};

struct RetainedTriggerEventSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::realtime_event_output(
                "trigger",
                iv::EventOutputProperties{
                    .type = iv::EventTypeId::trigger,
                    .max_events_per_sample = 0.5,
                },
                iv::RealtimeOutputConfig{.latency = 8}),
        };
    }

    void tick_block(iv::TickBlockContext<RetainedTriggerEventSource> const& ctx) const
    {
        if (ctx.block_size < 8) return;
        ctx.event_outputs[0].push(
            iv::TriggerEvent{}, 5, ctx.index, ctx.block_size);
        ctx.event_outputs[0].push(
            iv::TriggerEvent{}, ctx.block_size - 3, ctx.index, ctx.block_size);
        // Authored output latency permits publishing a future event that must
        // remain visible to the next root invocation.
        ctx.event_outputs[0].push(
            iv::TriggerEvent{}, ctx.block_size + 3, ctx.index, ctx.block_size);
    }
};

struct RetainedTriggerEventConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 3> indices{};
        std::array<std::uint64_t, 3> event_counts{};
        std::array<std::uint64_t, 3> first_times{};
        std::array<std::uint64_t, 3> second_times{};
        std::array<std::uint64_t, 3> last_times{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_event_input(
                "trigger",
                iv::EventTypeId::trigger,
                iv::RealtimeInputConfig{.history = 8}),
        };
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<RetainedTriggerEventConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const slot = state.calls++;
        if (slot >= state.indices.size()) return;
        auto const history = ctx.index < 8 ? ctx.index : std::size_t{8};
        auto const events = ctx.event_inputs[0].get_block(
            ctx.index - history,
            ctx.block_size + history);
        state.indices[slot] = ctx.index;
        state.event_counts[slot] = events.size();
        if (!events.empty()) {
            state.first_times[slot] = events[0].time;
            state.last_times[slot] = events[events.size() - 1].time;
        }
        if (events.size() > 1) {
            state.second_times[slot] = events[1].time;
        }
    }
};

struct PersistentEventRingSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::realtime_event_output(
                "trigger",
                iv::EventOutputProperties{
                    .type = iv::EventTypeId::trigger,
                    .max_events_per_sample = 0.5,
                }),
        };
    }

    void tick_block(iv::TickBlockContext<PersistentEventRingSource> const& ctx) const
    {
        if (ctx.block_size < 8) return;
        // Concentrating all events at one timestamp also proves the sizing-rate
        // field is not interpreted as a per-sample runtime limiter.
        for (std::size_t i = 0; i < 32; ++i) {
            ctx.event_outputs[0].push(
                iv::TriggerEvent{}, 5, ctx.index, ctx.block_size);
        }
    }
};

struct PersistentEventRingConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 5> indices{};
        std::array<std::uint64_t, 5> event_counts{};
        std::array<std::uint64_t, 5> first_times{};
        std::array<std::uint64_t, 5> last_times{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_event_input(
                "trigger",
                iv::EventTypeId::trigger,
                iv::RealtimeInputConfig{.history = 160}),
        };
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<PersistentEventRingConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const slot = state.calls++;
        if (slot >= state.indices.size()) return;
        auto const history = ctx.index < 160 ? ctx.index : std::size_t{160};
        auto const events = ctx.event_inputs[0].get_block(
            ctx.index - history,
            ctx.block_size + history);
        state.indices[slot] = ctx.index;
        state.event_counts[slot] = events.size();
        if (!events.empty()) {
            state.first_times[slot] = events[0].time;
            state.last_times[slot] = events[events.size() - 1].time;
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

void reused_sample_arena_module(iv::GraphBuilder& graph)
{
    auto source_a = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto sink_a = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink_a(source_a);
    auto source_b = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto sink_b = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink_b(source_b);
    graph.outputs();
}

void sample_fanout_conversion_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto mono_sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    auto mono_interleaved_sink = graph.node<"iv.test.graph_jit.state_context.mono_interleaved_consumer">();
    auto stereo_sink_a = graph.node<"iv.test.graph_jit.state_context.stereo_sample_consumer">();
    auto stereo_sink_b = graph.node<"iv.test.graph_jit.state_context.stereo_sample_consumer">();
    mono_sink(source);
    mono_interleaved_sink(source);
    stereo_sink_a(source);
    stereo_sink_b(source);
    graph.outputs();
}

void stereo_conversion_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.stereo_ramp_source">();
    auto mono_sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    auto stereo_planar_sink = graph.node<"iv.test.graph_jit.state_context.stereo_planar_consumer">();
    mono_sink(source);
    stereo_planar_sink(source);
    graph.outputs();
}

void history_fanout_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.history_ramp_source">();
    auto mono_sink = graph.node<"iv.test.graph_jit.state_context.history_consumer">();
    auto stereo_sink = graph.node<"iv.test.graph_jit.state_context.stereo_history_consumer">();
    mono_sink(source);
    stereo_sink(source);
    graph.outputs();
}

void persistent_history_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.large_history_ramp_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    sink(source);
    graph.outputs();
}

void latency_compensation_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.latency_compensation_probe">();
    delayed(source);
    sink(source, delayed);
    graph.outputs();
}

void latency_conversion_fanout_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.interleaved_latency_compensation_probe">();
    auto observer = graph.node<"iv.test.graph_jit.state_context.mono_interleaved_consumer">();
    delayed(source);
    sink(source, delayed);
    observer(source);
    graph.outputs();
}

void composed_latency_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.stereo_sample_consumer">();
    delayed(source);
    sink(graph.tile<iv::stereo>(source, delayed));
    graph.outputs();
}

void composed_history_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.stereo_history_consumer">();
    delayed(source);
    sink(graph.tile<iv::stereo>(source, delayed));
    graph.outputs();
}

void projected_composition_module(iv::GraphBuilder& graph)
{
    auto stereo_source = graph.node<"iv.test.graph_jit.state_context.stereo_ramp_source">();
    auto mono_source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.stereo_sample_consumer">();
    delayed(mono_source);
    sink(graph.tile<iv::stereo>(stereo_source[iv::stereo::right], delayed));
    graph.outputs();
}

void direct_event_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.trigger_event_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.trigger_event_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void transient_event_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.limited_trigger_event_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.limited_trigger_event_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void converted_event_fanout_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.midi_event_source">();
    auto sink_a = graph.node<"iv.test.graph_jit.state_context.trigger_event_consumer">();
    auto sink_b = graph.node<"iv.test.graph_jit.state_context.trigger_event_consumer">();
    sink_a.connect_event_input(0, source.event_port());
    sink_b.connect_event_input(0, source.event_port());
    graph.outputs();
}

void retained_event_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.retained_trigger_event_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.retained_trigger_event_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void persistent_event_ring_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.persistent_event_ring_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.persistent_event_ring_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void retained_converted_event_fanout_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.midi_event_source">();
    auto retained = graph.node<"iv.test.graph_jit.state_context.retained_midi_event_consumer">();
    auto trigger_a = graph.node<"iv.test.graph_jit.state_context.trigger_event_consumer">();
    auto trigger_b = graph.node<"iv.test.graph_jit.state_context.limited_trigger_event_consumer">();
    retained.connect_event_input(0, source.event_port());
    trigger_a.connect_event_input(0, source.event_port());
    trigger_b.connect_event_input(0, source.event_port());
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
IV_NODE("iv.test.graph_jit.state_context.mono_interleaved_consumer", MonoInterleavedConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.stereo_ramp_source", StereoRampSource);
IV_NODE("iv.test.graph_jit.state_context.stereo_planar_consumer", StereoPlanarConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.stereo_sample_consumer", StereoSampleConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.history_ramp_source", HistoryRampSource);
IV_NODE("iv.test.graph_jit.state_context.history_consumer", HistoryConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.stereo_history_consumer", StereoHistoryConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.large_history_ramp_source", LargeHistoryRampSource);
IV_NODE("iv.test.graph_jit.state_context.large_history_consumer", LargeHistoryConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.five_sample_delay", FiveSampleDelay);
IV_NODE("iv.test.graph_jit.state_context.latency_compensation_probe", LatencyCompensationProbe);
IV_NODE("iv.test.graph_jit.state_context.interleaved_latency_compensation_probe", InterleavedLatencyCompensationProbe);
IV_NODE("iv.test.graph_jit.state_context.trigger_event_source", TriggerEventSource);
IV_NODE("iv.test.graph_jit.state_context.midi_event_source", MidiEventSource);
IV_NODE("iv.test.graph_jit.state_context.limited_trigger_event_source", LimitedTriggerEventSource);
IV_NODE("iv.test.graph_jit.state_context.event_feedback_a", EventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.event_feedback_b", EventFeedbackB);
IV_NODE("iv.test.graph_jit.state_context.event_feedback_burst_a", EventFeedbackBurstA);
IV_NODE("iv.test.graph_jit.state_context.event_feedback_fanout_a", EventFeedbackFanoutA);
IV_NODE("iv.test.graph_jit.state_context.trigger_event_consumer", TriggerEventConsumer);
IV_NODE("iv.test.graph_jit.state_context.limited_trigger_event_consumer", LimitedTriggerEventConsumer);
IV_NODE("iv.test.graph_jit.state_context.retained_trigger_event_source", RetainedTriggerEventSource);
IV_NODE("iv.test.graph_jit.state_context.retained_trigger_event_consumer", RetainedTriggerEventConsumer);
IV_NODE("iv.test.graph_jit.state_context.persistent_event_ring_source", PersistentEventRingSource);
IV_NODE("iv.test.graph_jit.state_context.persistent_event_ring_consumer", PersistentEventRingConsumer);
IV_NODE("iv.test.graph_jit.state_context.retained_midi_event_consumer", RetainedMidiEventConsumer);
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
IV_MODULE("iv.test.graph_jit.state_context.reused_sample_arena_module", reused_sample_arena_module);
IV_MODULE("iv.test.graph_jit.state_context.sample_fanout_conversion_module", sample_fanout_conversion_module);
IV_MODULE("iv.test.graph_jit.state_context.stereo_conversion_module", stereo_conversion_module);
IV_MODULE("iv.test.graph_jit.state_context.history_fanout_module", history_fanout_module);
IV_MODULE("iv.test.graph_jit.state_context.persistent_history_module", persistent_history_module);
IV_MODULE("iv.test.graph_jit.state_context.latency_compensation_module", latency_compensation_module);
IV_MODULE("iv.test.graph_jit.state_context.latency_conversion_fanout_module", latency_conversion_fanout_module);
IV_MODULE("iv.test.graph_jit.state_context.composed_latency_module", composed_latency_module);
IV_MODULE("iv.test.graph_jit.state_context.composed_history_module", composed_history_module);
IV_MODULE("iv.test.graph_jit.state_context.projected_composition_module", projected_composition_module);
IV_MODULE("iv.test.graph_jit.state_context.direct_event_module", direct_event_module);
IV_MODULE("iv.test.graph_jit.state_context.transient_event_module", transient_event_module);
IV_MODULE("iv.test.graph_jit.state_context.converted_event_fanout_module", converted_event_fanout_module);
IV_MODULE("iv.test.graph_jit.state_context.retained_event_module", retained_event_module);
IV_MODULE("iv.test.graph_jit.state_context.persistent_event_ring_module", persistent_event_ring_module);
IV_MODULE("iv.test.graph_jit.state_context.retained_converted_event_fanout_module", retained_converted_event_fanout_module);
IV_MODULE("iv.test.graph_jit.state_context.ported_module", ported_module);
)cpp";
}

void write_graph_jit_fixture_file_if_changed(
    std::filesystem::path const& path,
    std::string_view contents)
{
    if (std::filesystem::exists(path)
        && iv::test::read_text(path) == contents) {
        return;
    }
    iv::test::write_text(path, std::string(contents));
}

std::filesystem::path ensure_graph_jit_runtime_workspace()
{
    auto const root = iv::test::shared_test_fixtures_root();
    auto const token = iv::test::sanitize_test_token(graph_jit_runtime_fixture_name);
    auto const workspace = root / token;
    auto const lock = iv::test::ScopedFileLock(root / (token + ".lock"));
    std::filesystem::create_directories(workspace);
    if (!std::filesystem::exists(workspace / "iv_project.jsonl")) {
        iv::test::write_text(workspace / "iv_project.jsonl", "");
    }
    write_graph_jit_fixture_file_if_changed(
        workspace / "iv_package.json",
        "{\n  \"schema\": 2,\n  \"entry\": \"module.cpp\"\n}\n");
    write_graph_jit_fixture_file_if_changed(
        workspace / "module.cpp",
        graph_jit_runtime_package_source());
    return workspace;
}

std::shared_ptr<iv::PackageRevision const> load_graph_jit_runtime_revision()
{
    auto const workspace = std::filesystem::weakly_canonical(
        ensure_graph_jit_runtime_workspace());
    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    iv::PackageJit package_jit(
        startup_config.initialize(),
        iv::ModuleLoader::OptimizationLevel::O0);
    iv::PackageJitBatchRequest request{
        .declarations = {iv::IvPackageDeclaration{
            .package_id = graph_jit_state_package_id,
            .package_root = workspace,
        }},
    };
    package_jit.handle_build_request(request);
    if (!request.result.failed.empty()) {
        throw std::runtime_error(request.result.failed.front().message);
    }
    if (request.result.revisions.size() != 1u) {
        throw std::runtime_error("GraphJit shared runtime fixture did not produce one package revision");
    }
    return std::make_shared<iv::PackageRevision const>(
        std::move(request.result.revisions.front()));
}


std::shared_ptr<iv::ConfiguredGraph const> configured_event_feedback_graph(
    iv::PackageRevision const& revision,
    std::string_view first_definition = graph_jit_event_feedback_a_id)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit event-feedback builder session");
    }
    auto const package_root = revision.package_root.generic_string();
    std::array packages{iv::details::BuilderPackageView{
        .package_root = package_root,
        .definitions = revision.provider_definitions,
        .config_pointer_fields = revision.config_pointer_fields,
        .retained_globals = revision.retained_globals,
        .node_state_structures = revision.node_state_structures,
    }};
    iv::details::set_builder_packages(session.get(), packages);

    iv::GraphBuilder graph(session.get());
    auto first = iv::details::configure_package_definition_provider(
        graph, first_definition, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, graph_jit_event_feedback_b_id, std::nullopt, {});
    first.connect_event_input(0, second.event_port());
    second.connect_event_input(0, first.event_port().detach(10));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const> configured_event_feedback_fanout_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit event-feedback fanout builder session");
    }
    auto const package_root = revision.package_root.generic_string();
    std::array packages{iv::details::BuilderPackageView{
        .package_root = package_root,
        .definitions = revision.provider_definitions,
        .config_pointer_fields = revision.config_pointer_fields,
        .retained_globals = revision.retained_globals,
        .node_state_structures = revision.node_state_structures,
    }};
    iv::details::set_builder_packages(session.get(), packages);

    iv::GraphBuilder graph(session.get());
    auto source = iv::details::configure_package_definition_provider(
        graph, graph_jit_event_feedback_fanout_a_id, std::nullopt, {});
    auto first = iv::details::configure_package_definition_provider(
        graph, graph_jit_event_feedback_b_id, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, graph_jit_event_feedback_b_id, std::nullopt, {});
    source.connect_event_input(0, first.event_port());
    source.connect_event_input(1, second.event_port());
    auto detached = source.event_port().detach(10);
    first.connect_event_input(0, detached);
    second.connect_event_input(0, detached);
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

SampleConsumerProbeStateMirror* find_consumer_state(
    iv::CompiledGraph const& graph,
    iv::NodeStorage& storage)
{
    for (std::size_t i = 0; i < graph.node_layout.nodes.size(); ++i) {
        if (graph.node_layout.nodes[i].state_size
            == sizeof(SampleConsumerProbeStateMirror)) {
            return static_cast<SampleConsumerProbeStateMirror*>(storage.state_ptr(i));
        }
    }
    return nullptr;
}

std::size_t count_raw_regions(iv::NodeLayout const& layout)
{
    return static_cast<std::size_t>(std::ranges::count_if(
        layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw;
        }));
}

class GraphJitRuntimeFixture : public ::testing::Test {
protected:
    std::shared_ptr<iv::PackageRevision const> revision;
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> definitions;
    std::unique_ptr<iv::GraphJit> jit;
    iv::ResourceContext resources;

    void SetUp() override
    {
        ASSERT_NO_THROW(revision = load_graph_jit_runtime_revision());
        ASSERT_TRUE(revision);
        definitions = make_graph_jit_snapshot(revision, 91);
        jit = std::make_unique<iv::GraphJit>(iv::GraphJitConfig{
            .sample_rate = 88200,
            .block_size = 64,
        });
    }

    iv::GraphJitCompileResult compile_graph(
        std::shared_ptr<iv::ConfiguredGraph const> graph,
        std::uint64_t generation)
    {
        EXPECT_NE(graph, nullptr);
        return jit->compile(iv::GraphJitCompileRequest{
            .project_generation = generation,
            .graph = std::move(graph),
            .definitions = definitions,
        });
    }

    iv::GraphJitCompileResult compile(
        std::string_view module_id,
        std::uint64_t generation)
    {
        return compile_graph(
            configured_module_graph(*revision, module_id), generation);
    }
};
} // namespace

TEST(GraphJitSharedRuntimeFixture, BuildPackage)
{
    std::shared_ptr<iv::PackageRevision const> revision;
    ASSERT_NO_THROW(revision = load_graph_jit_runtime_revision());
    ASSERT_TRUE(revision);
    auto has_leaf_definition = [&](std::string_view definition_id) {
        return std::ranges::any_of(
            revision->leaf_definitions,
            [&](auto const& definition) {
                return definition.definition_id == definition_id;
            });
    };
    auto has_module_definition = [&](std::string_view definition_id) {
        return std::ranges::any_of(
            revision->module_definitions,
            [&](auto const& definition) {
                return definition.definition_id == definition_id;
            });
    };
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.mono_interleaved_consumer"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.stereo_ramp_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.stereo_planar_consumer"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.stereo_sample_consumer"));
    EXPECT_TRUE(has_module_definition(graph_jit_sample_fanout_conversion_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_stereo_conversion_module_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.history_ramp_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.history_consumer"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.stereo_history_consumer"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.large_history_ramp_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.large_history_consumer"));
    EXPECT_TRUE(has_module_definition(graph_jit_history_fanout_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_persistent_history_module_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.five_sample_delay"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.latency_compensation_probe"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.interleaved_latency_compensation_probe"));
    EXPECT_TRUE(has_module_definition(graph_jit_latency_compensation_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_latency_conversion_fanout_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_composed_latency_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_composed_history_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_projected_composition_module_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.trigger_event_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.limited_trigger_event_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.trigger_event_consumer"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.limited_trigger_event_consumer"));
    EXPECT_TRUE(has_module_definition(graph_jit_direct_event_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_transient_event_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_converted_event_fanout_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_retained_event_module_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.persistent_event_ring_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.persistent_event_ring_consumer"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.retained_midi_event_consumer"));
    EXPECT_TRUE(has_module_definition(graph_jit_persistent_event_ring_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_retained_converted_event_fanout_module_id));
}

TEST_F(GraphJitRuntimeFixture, StateAndCompiledStateContexts)
{
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

}

TEST_F(GraphJitRuntimeFixture, ConfiguredValuesAndPointerRelocations)
{
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

}

TEST_F(GraphJitRuntimeFixture, MultipleNodesSkipAndBlockSlicing)
{
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

}

TEST_F(GraphJitRuntimeFixture, DirectSampleStorage)
{
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

}

TEST_F(GraphJitRuntimeFixture, TransientSampleStorage)
{
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

}

TEST_F(GraphJitRuntimeFixture, TransientArenaReuse)
{
    auto reused_arena_graph = configured_module_graph(
        *revision, graph_jit_reused_sample_arena_module_id);
    ASSERT_TRUE(reused_arena_graph);
    auto reused_arena_analysis =
        iv::graph_jit::detail::build_connection_analysis_plan(
            *reused_arena_graph, 64);
    ASSERT_TRUE(reused_arena_analysis.has_value())
        << (reused_arena_analysis
                ? std::string{}
                : reused_arena_analysis.error());
    auto reused_arena_physical =
        iv::graph_jit::detail::build_sample_physical_plan(
            *reused_arena_analysis, 64);
    ASSERT_TRUE(reused_arena_physical.has_value())
        << (reused_arena_physical
                ? std::string{}
                : reused_arena_physical.error());
    ASSERT_EQ(reused_arena_physical->representations.size(), 2u);
    ASSERT_EQ(reused_arena_physical->transient_allocations.size(), 2u);
    auto const first_arena_allocation =
        reused_arena_physical->representations[0].transient_allocation;
    auto const second_arena_allocation =
        reused_arena_physical->representations[1].transient_allocation;
    ASSERT_LT(
        first_arena_allocation,
        reused_arena_physical->transient_allocations.size());
    ASSERT_LT(
        second_arena_allocation,
        reused_arena_physical->transient_allocations.size());
    EXPECT_EQ(
        reused_arena_physical->transient_allocations[first_arena_allocation]
            .region_relative_offset,
        reused_arena_physical->transient_allocations[second_arena_allocation]
            .region_relative_offset);
    EXPECT_EQ(
        reused_arena_physical->transient_arena_size,
        64u * sizeof(iv::Sample));

    auto reused_arena = compile_graph(reused_arena_graph, 111);
    ASSERT_TRUE(reused_arena.succeeded())
        << (reused_arena.diagnostics.empty()
                ? ""
                : reused_arena.diagnostics.front().message);
    ASSERT_EQ(reused_arena.compiled_graph->node_layout.nodes.size(), 4u);
    ASSERT_EQ(count_raw_regions(reused_arena.compiled_graph->node_layout), 1u);
    auto const reused_raw = std::ranges::find_if(
        reused_arena.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw;
        });
    ASSERT_NE(reused_raw, reused_arena.compiled_graph->node_layout.regions.end());
    EXPECT_EQ(reused_raw->size, 64u * sizeof(iv::Sample));

    auto reused_storage =
        reused_arena.compiled_graph->node_layout.create_storage(resources);
    reused_storage.initialize();
    std::vector<SampleConsumerProbeStateMirror*> reused_consumer_states;
    for (std::size_t i = 0;
         i < reused_arena.compiled_graph->node_layout.nodes.size(); ++i) {
        if (reused_arena.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(SampleConsumerProbeStateMirror)) {
            reused_consumer_states.push_back(
                static_cast<SampleConsumerProbeStateMirror*>(
                    reused_storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(reused_consumer_states.size(), 2u);
    reused_arena.compiled_graph->root_operations.tick_block(
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

}

TEST_F(GraphJitRuntimeFixture, SampleFanoutConversion)
{
    auto fanout_graph = configured_module_graph(
        *revision, graph_jit_sample_fanout_conversion_module_id);
    ASSERT_TRUE(fanout_graph);
    auto fanout_analysis = iv::graph_jit::detail::build_connection_analysis_plan(
        *fanout_graph, 64);
    ASSERT_TRUE(fanout_analysis.has_value())
        << (fanout_analysis ? std::string{} : fanout_analysis.error());
    ASSERT_EQ(fanout_analysis->sample_producer_groups.size(), 1u);
    ASSERT_EQ(fanout_analysis->sample_producer_groups[0].connection_indices.size(), 4u);
    ASSERT_TRUE(fanout_analysis->sample_producer_groups[0].implementation.has_value());
    EXPECT_EQ(
        *fanout_analysis->sample_producer_groups[0].implementation,
        iv::SampleConnectionImplementationKind::transient_materialization);

    auto fanout_physical = iv::graph_jit::detail::build_sample_physical_plan(
        *fanout_analysis, 64);
    ASSERT_TRUE(fanout_physical.has_value())
        << (fanout_physical ? std::string{} : fanout_physical.error());
    ASSERT_EQ(fanout_physical->producer_groups.size(), 1u);
    ASSERT_TRUE(fanout_physical->producer_groups[0].has_value());
    ASSERT_EQ(fanout_physical->representations.size(), 3u);
    ASSERT_EQ(fanout_physical->materializations.size(), 2u);
    ASSERT_EQ(fanout_physical->connection_representations.size(), 4u);
    auto const fanout_canonical =
        fanout_physical->producer_groups[0]->canonical_representation;
    std::size_t canonical_connection_count = 0;
    std::optional<std::size_t> mono_interleaved_representation;
    std::optional<std::size_t> stereo_representation;
    std::size_t mono_interleaved_connection_count = 0;
    std::size_t stereo_connection_count = 0;
    for (auto const representation : fanout_physical->connection_representations) {
        ASSERT_TRUE(representation.has_value());
        if (*representation == fanout_canonical) {
            ++canonical_connection_count;
            continue;
        }
        auto const layout = fanout_physical->representations[*representation]
            .channel_layout;
        if (layout.channel_type == iv::ChannelTypeId::mono) {
            if (mono_interleaved_representation) {
                EXPECT_EQ(*representation, *mono_interleaved_representation);
            }
            mono_interleaved_representation = *representation;
            ++mono_interleaved_connection_count;
        } else {
            ASSERT_EQ(layout.channel_type, iv::ChannelTypeId::stereo);
            if (stereo_representation) {
                EXPECT_EQ(*representation, *stereo_representation);
            }
            stereo_representation = *representation;
            ++stereo_connection_count;
        }
    }
    EXPECT_EQ(canonical_connection_count, 1u);
    EXPECT_EQ(mono_interleaved_connection_count, 1u);
    EXPECT_EQ(stereo_connection_count, 2u);
    ASSERT_TRUE(mono_interleaved_representation.has_value());
    ASSERT_TRUE(stereo_representation.has_value());
    EXPECT_EQ(
        fanout_physical->representations[*mono_interleaved_representation]
            .channel_layout,
        (iv::ChannelLayout{
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        }));
    EXPECT_EQ(
        fanout_physical->representations[*stereo_representation].channel_layout,
        (iv::ChannelLayout{
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        }));
    EXPECT_EQ(
        std::ranges::count_if(
            fanout_physical->materializations,
            [&](auto const& materialization) {
                return materialization.source_representation == fanout_canonical;
            }),
        2);

    auto fanout = compile_graph(fanout_graph, 112);
    ASSERT_TRUE(fanout.succeeded())
        << (fanout.diagnostics.empty() ? "" : fanout.diagnostics.front().message);
    ASSERT_EQ(fanout.compiled_graph->node_layout.nodes.size(), 5u);
    ASSERT_EQ(count_raw_regions(fanout.compiled_graph->node_layout), 1u);
    auto const fanout_raw = std::ranges::find_if(
        fanout.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw;
        });
    ASSERT_NE(fanout_raw, fanout.compiled_graph->node_layout.regions.end());
    EXPECT_EQ(fanout_raw->size, 4u * 64u * sizeof(iv::Sample));

    auto fanout_storage =
        fanout.compiled_graph->node_layout.create_storage(resources);
    fanout_storage.initialize();
    std::vector<SampleConsumerProbeStateMirror*> fanout_mono_states;
    std::vector<StereoSampleConsumerProbeStateMirror*> fanout_stereo_states;
    for (std::size_t i = 0; i < fanout.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size = fanout.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(SampleConsumerProbeStateMirror)) {
            fanout_mono_states.push_back(
                static_cast<SampleConsumerProbeStateMirror*>(
                    fanout_storage.state_ptr(i)));
        } else if (state_size == sizeof(StereoSampleConsumerProbeStateMirror)) {
            fanout_stereo_states.push_back(
                static_cast<StereoSampleConsumerProbeStateMirror*>(
                    fanout_storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(fanout_mono_states.size(), 2u);
    ASSERT_EQ(fanout_stereo_states.size(), 2u);

    // 37..100 crosses the 64-frame physical ring boundary. Conversion must use
    // absolute-index addressing rather than treating the representation as one
    // contiguous block. Both converted consumers share the same derived block.
    fanout.compiled_graph->root_operations.tick_block(
        fanout_storage.buffer().data(), 37, 64);
    for (auto const* state : fanout_mono_states) {
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->calls, 1u);
        EXPECT_FLOAT_EQ(state->first, 37.0f);
        EXPECT_FLOAT_EQ(state->last, 100.0f);
        EXPECT_FLOAT_EQ(state->sum, 4384.0f);
    }
    for (auto const* state : fanout_stereo_states) {
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->calls, 1u);
        EXPECT_EQ(state->last_index, 37u);
        EXPECT_EQ(state->last_block_size, 64u);
        EXPECT_FLOAT_EQ(state->first_left, 37.0f);
        EXPECT_FLOAT_EQ(state->first_right, 37.0f);
        EXPECT_FLOAT_EQ(state->last_left, 100.0f);
        EXPECT_FLOAT_EQ(state->last_right, 100.0f);
        EXPECT_FLOAT_EQ(state->sum_left, 4384.0f);
        EXPECT_FLOAT_EQ(state->sum_right, 4384.0f);
    }

    fanout.compiled_graph->root_operations.tick_block(
        fanout_storage.buffer().data(), 205, 16);
    for (auto const* state : fanout_mono_states) {
        EXPECT_EQ(state->calls, 2u);
        EXPECT_FLOAT_EQ(state->first, 205.0f);
        EXPECT_FLOAT_EQ(state->last, 220.0f);
        EXPECT_FLOAT_EQ(state->sum, 3400.0f);
    }
    for (auto const* state : fanout_stereo_states) {
        EXPECT_EQ(state->calls, 2u);
        EXPECT_EQ(state->last_index, 205u);
        EXPECT_EQ(state->last_block_size, 16u);
        EXPECT_FLOAT_EQ(state->first_left, 205.0f);
        EXPECT_FLOAT_EQ(state->first_right, 205.0f);
        EXPECT_FLOAT_EQ(state->last_left, 220.0f);
        EXPECT_FLOAT_EQ(state->last_right, 220.0f);
        EXPECT_FLOAT_EQ(state->sum_left, 3400.0f);
        EXPECT_FLOAT_EQ(state->sum_right, 3400.0f);
    }

}

TEST_F(GraphJitRuntimeFixture, StereoSampleConversion)
{
    auto stereo_conversion_graph = configured_module_graph(
        *revision, graph_jit_stereo_conversion_module_id);
    ASSERT_TRUE(stereo_conversion_graph);
    auto stereo_conversion_analysis =
        iv::graph_jit::detail::build_connection_analysis_plan(
            *stereo_conversion_graph, 64);
    ASSERT_TRUE(stereo_conversion_analysis.has_value())
        << (stereo_conversion_analysis
                ? std::string{}
                : stereo_conversion_analysis.error());
    ASSERT_EQ(stereo_conversion_analysis->sample_producer_groups.size(), 1u);
    ASSERT_EQ(
        stereo_conversion_analysis->sample_producer_groups[0]
            .connection_indices.size(),
        2u);
    ASSERT_TRUE(
        stereo_conversion_analysis->sample_producer_groups[0]
            .implementation.has_value());
    EXPECT_EQ(
        *stereo_conversion_analysis->sample_producer_groups[0].implementation,
        iv::SampleConnectionImplementationKind::transient_materialization);
    auto stereo_conversion_physical =
        iv::graph_jit::detail::build_sample_physical_plan(
            *stereo_conversion_analysis, 64);
    ASSERT_TRUE(stereo_conversion_physical.has_value())
        << (stereo_conversion_physical
                ? std::string{}
                : stereo_conversion_physical.error());
    EXPECT_EQ(stereo_conversion_physical->representations.size(), 3u);
    EXPECT_EQ(stereo_conversion_physical->materializations.size(), 2u);

    auto stereo_conversion = compile_graph(stereo_conversion_graph, 113);
    ASSERT_TRUE(stereo_conversion.succeeded())
        << (stereo_conversion.diagnostics.empty()
                ? ""
                : stereo_conversion.diagnostics.front().message);
    ASSERT_EQ(stereo_conversion.compiled_graph->node_layout.nodes.size(), 3u);
    ASSERT_EQ(count_raw_regions(stereo_conversion.compiled_graph->node_layout), 1u);
    auto const stereo_conversion_raw = std::ranges::find_if(
        stereo_conversion.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw;
        });
    ASSERT_NE(
        stereo_conversion_raw,
        stereo_conversion.compiled_graph->node_layout.regions.end());
    EXPECT_EQ(
        stereo_conversion_raw->size,
        5u * 64u * sizeof(iv::Sample));

    auto stereo_conversion_storage =
        stereo_conversion.compiled_graph->node_layout.create_storage(resources);
    stereo_conversion_storage.initialize();
    SampleConsumerProbeStateMirror* stereo_to_mono_state = nullptr;
    StereoSampleConsumerProbeStateMirror* stereo_to_planar_state = nullptr;
    for (std::size_t i = 0;
         i < stereo_conversion.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            stereo_conversion.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(SampleConsumerProbeStateMirror)) {
            stereo_to_mono_state = static_cast<SampleConsumerProbeStateMirror*>(
                stereo_conversion_storage.state_ptr(i));
        } else if (state_size == sizeof(StereoSampleConsumerProbeStateMirror)) {
            stereo_to_planar_state =
                static_cast<StereoSampleConsumerProbeStateMirror*>(
                    stereo_conversion_storage.state_ptr(i));
        }
    }
    ASSERT_NE(stereo_to_mono_state, nullptr);
    ASSERT_NE(stereo_to_planar_state, nullptr);

    stereo_conversion.compiled_graph->root_operations.tick_block(
        stereo_conversion_storage.buffer().data(), 51, 32);
    EXPECT_EQ(stereo_to_mono_state->calls, 1u);
    EXPECT_EQ(stereo_to_mono_state->last_index, 51u);
    EXPECT_EQ(stereo_to_mono_state->last_block_size, 32u);
    EXPECT_FLOAT_EQ(stereo_to_mono_state->first, 551.0f);
    EXPECT_FLOAT_EQ(stereo_to_mono_state->last, 582.0f);
    EXPECT_FLOAT_EQ(stereo_to_mono_state->sum, 18128.0f);

    EXPECT_EQ(stereo_to_planar_state->calls, 1u);
    EXPECT_EQ(stereo_to_planar_state->last_index, 51u);
    EXPECT_EQ(stereo_to_planar_state->last_block_size, 32u);
    EXPECT_FLOAT_EQ(stereo_to_planar_state->first_left, 51.0f);
    EXPECT_FLOAT_EQ(stereo_to_planar_state->first_right, 1051.0f);
    EXPECT_FLOAT_EQ(stereo_to_planar_state->last_left, 82.0f);
    EXPECT_FLOAT_EQ(stereo_to_planar_state->last_right, 1082.0f);
    EXPECT_FLOAT_EQ(stereo_to_planar_state->sum_left, 2128.0f);
    EXPECT_FLOAT_EQ(stereo_to_planar_state->sum_right, 34128.0f);

}

TEST_F(GraphJitRuntimeFixture, SampleLatencyCompensation)
{
    auto latency_compensation_graph = configured_module_graph(
        *revision, graph_jit_latency_compensation_module_id);
    ASSERT_TRUE(latency_compensation_graph);
    auto latency_compensation = compile_graph(latency_compensation_graph, 114);
    ASSERT_TRUE(latency_compensation.succeeded())
        << (latency_compensation.diagnostics.empty()
                ? ""
                : latency_compensation.diagnostics.front().message);
    ASSERT_EQ(latency_compensation.compiled_graph->node_layout.nodes.size(), 3u);

    auto latency_storage =
        latency_compensation.compiled_graph->node_layout.create_storage(resources);
    latency_storage.initialize();
    LatencyCompensationProbeStateMirror* latency_probe = nullptr;
    for (std::size_t i = 0;
         i < latency_compensation.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const& node = latency_compensation.compiled_graph->node_layout.nodes[i];
        if (node.state_size == sizeof(LatencyCompensationProbeStateMirror)) {
            ASSERT_EQ(latency_probe, nullptr);
            latency_probe = static_cast<LatencyCompensationProbeStateMirror*>(
                latency_storage.state_ptr(i));
        }
    }
    ASSERT_NE(latency_probe, nullptr);

    latency_compensation.compiled_graph->root_operations.tick_block(
        latency_storage.buffer().data(), 0, 64);
    EXPECT_EQ(latency_probe->calls, 1u);
    EXPECT_EQ(latency_probe->last_index, 0u);
    EXPECT_EQ(latency_probe->last_block_size, 64u);
    EXPECT_EQ(latency_probe->mismatches, 0u);
    EXPECT_FLOAT_EQ(latency_probe->fast_first, 0.0f);
    EXPECT_FLOAT_EQ(latency_probe->slow_first, 0.0f);
    EXPECT_FLOAT_EQ(latency_probe->fast_last, 56.0f);
    EXPECT_FLOAT_EQ(latency_probe->slow_last, 56.0f);
    EXPECT_FLOAT_EQ(latency_probe->max_abs_difference, 0.0f);
    EXPECT_EQ(latency_probe->marker, 0x71ac0deu);

    // The second kernel call requires the fast branch to restore the source's
    // compiler-owned seven-frame carry while the slow branch restores its own
    // two-frame output carry. Both must still address the same logical sample.
    latency_compensation.compiled_graph->root_operations.tick_block(
        latency_storage.buffer().data(), 64, 64);
    EXPECT_EQ(latency_probe->calls, 2u);
    EXPECT_EQ(latency_probe->last_index, 64u);
    EXPECT_EQ(latency_probe->last_block_size, 64u);
    EXPECT_EQ(latency_probe->mismatches, 0u);
    EXPECT_FLOAT_EQ(latency_probe->fast_first, 57.0f);
    EXPECT_FLOAT_EQ(latency_probe->slow_first, 57.0f);
    EXPECT_FLOAT_EQ(latency_probe->fast_last, 120.0f);
    EXPECT_FLOAT_EQ(latency_probe->slow_last, 120.0f);
    EXPECT_FLOAT_EQ(latency_probe->max_abs_difference, 0.0f);

}

TEST_F(GraphJitRuntimeFixture, ConvertedFanoutLatencyWindows)
{
    auto latency_conversion_fanout_graph = configured_module_graph(
        *revision, graph_jit_latency_conversion_fanout_module_id);
    ASSERT_TRUE(latency_conversion_fanout_graph);
    auto latency_conversion_analysis =
        iv::graph_jit::detail::build_connection_analysis_plan(
            *latency_conversion_fanout_graph, 64);
    ASSERT_TRUE(latency_conversion_analysis.has_value())
        << (latency_conversion_analysis
                ? std::string{}
                : latency_conversion_analysis.error());

    auto latency_source_group = std::ranges::find_if(
        latency_conversion_analysis->sample_producer_groups,
        [](auto const& group) { return group.connection_indices.size() == 3; });
    ASSERT_NE(
        latency_source_group,
        latency_conversion_analysis->sample_producer_groups.end());
    EXPECT_EQ(latency_source_group->requirements.retained_frames, 7u);
    ASSERT_TRUE(latency_source_group->implementation.has_value());
    EXPECT_EQ(
        *latency_source_group->implementation,
        iv::SampleConnectionImplementationKind::compact_persistent_carry);

    std::size_t converted_read_0 = 0;
    std::size_t converted_read_7 = 0;
    for (auto const connection_index : latency_source_group->connection_indices) {
        auto const& connection =
            latency_conversion_analysis->sample_connections[connection_index];
        if (!connection.requires_conversion) continue;
        if (connection.read_latency == 0) ++converted_read_0;
        if (connection.read_latency == 7) ++converted_read_7;
    }
    EXPECT_EQ(converted_read_0, 1u);
    EXPECT_EQ(converted_read_7, 1u);

    auto latency_conversion_physical =
        iv::graph_jit::detail::build_sample_physical_plan(
            *latency_conversion_analysis, 64);
    ASSERT_TRUE(latency_conversion_physical.has_value())
        << (latency_conversion_physical
                ? std::string{}
                : latency_conversion_physical.error());
    auto const latency_source_group_index = static_cast<std::size_t>(
        latency_source_group
        - latency_conversion_analysis->sample_producer_groups.begin());
    ASSERT_LT(
        latency_source_group_index,
        latency_conversion_physical->producer_groups.size());
    ASSERT_TRUE(
        latency_conversion_physical->producer_groups[latency_source_group_index]
            .has_value());
    auto const latency_source_representation =
        latency_conversion_physical->producer_groups[latency_source_group_index]
            ->canonical_representation;
    auto source_conversion = std::ranges::find_if(
        latency_conversion_physical->materializations,
        [&](auto const& materialization) {
            return materialization.source_representation
                    == latency_source_representation
                && materialization.target_layout.channel_type
                    == iv::ChannelTypeId::mono
                && materialization.target_layout.sample_layout
                    == iv::SampleStreamLayout::interleaved;
        });
    ASSERT_NE(
        source_conversion,
        latency_conversion_physical->materializations.end());
    EXPECT_EQ(source_conversion->retained_before, 7u);
    EXPECT_EQ(source_conversion->latest_read_latency, 0u);

    auto latency_conversion_fanout =
        compile_graph(latency_conversion_fanout_graph, 115);
    ASSERT_TRUE(latency_conversion_fanout.succeeded())
        << (latency_conversion_fanout.diagnostics.empty()
                ? ""
                : latency_conversion_fanout.diagnostics.front().message);
    ASSERT_EQ(
        latency_conversion_fanout.compiled_graph->node_layout.nodes.size(), 4u);

    auto latency_conversion_storage =
        latency_conversion_fanout.compiled_graph->node_layout.create_storage(
            resources);
    latency_conversion_storage.initialize();
    LatencyCompensationProbeStateMirror* interleaved_latency_probe = nullptr;
    SampleConsumerProbeStateMirror* current_observer = nullptr;
    for (std::size_t i = 0;
         i < latency_conversion_fanout.compiled_graph->node_layout.nodes.size();
         ++i) {
        auto const& node =
            latency_conversion_fanout.compiled_graph->node_layout.nodes[i];
        if (node.state_size == sizeof(LatencyCompensationProbeStateMirror)) {
            ASSERT_EQ(interleaved_latency_probe, nullptr);
            interleaved_latency_probe =
                static_cast<LatencyCompensationProbeStateMirror*>(
                    latency_conversion_storage.state_ptr(i));
        } else if (node.state_size == sizeof(SampleConsumerProbeStateMirror)) {
            ASSERT_EQ(current_observer, nullptr);
            current_observer = static_cast<SampleConsumerProbeStateMirror*>(
                latency_conversion_storage.state_ptr(i));
        }
    }
    ASSERT_NE(interleaved_latency_probe, nullptr);
    ASSERT_NE(current_observer, nullptr);

    latency_conversion_fanout.compiled_graph->root_operations.tick_block(
        latency_conversion_storage.buffer().data(), 0, 64);
    EXPECT_EQ(interleaved_latency_probe->calls, 1u);
    EXPECT_EQ(interleaved_latency_probe->mismatches, 0u);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->fast_first, 0.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->slow_first, 0.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->fast_last, 56.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->slow_last, 56.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->max_abs_difference, 0.0f);
    EXPECT_EQ(interleaved_latency_probe->marker, 0x1a7e2e0u);

    // This observer shares the same converted interleaved representation as the
    // compensated fast branch but reads it at zero latency. The materializer
    // must therefore cover both [-7, +56] and [0, +63], not just the older
    // compensated window.
    EXPECT_EQ(current_observer->calls, 1u);
    EXPECT_EQ(current_observer->last_index, 0u);
    EXPECT_EQ(current_observer->last_block_size, 64u);
    EXPECT_FLOAT_EQ(current_observer->first, 0.0f);
    EXPECT_FLOAT_EQ(current_observer->last, 63.0f);
    EXPECT_FLOAT_EQ(current_observer->sum, 2016.0f);

    latency_conversion_fanout.compiled_graph->root_operations.tick_block(
        latency_conversion_storage.buffer().data(), 64, 64);
    EXPECT_EQ(interleaved_latency_probe->calls, 2u);
    EXPECT_EQ(interleaved_latency_probe->mismatches, 0u);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->fast_first, 57.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->slow_first, 57.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->fast_last, 120.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->slow_last, 120.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->max_abs_difference, 0.0f);
    EXPECT_EQ(current_observer->calls, 2u);
    EXPECT_EQ(current_observer->last_index, 64u);
    EXPECT_FLOAT_EQ(current_observer->first, 64.0f);
    EXPECT_FLOAT_EQ(current_observer->last, 127.0f);
    EXPECT_FLOAT_EQ(current_observer->sum, 6112.0f);

}

TEST_F(GraphJitRuntimeFixture, ComposedSampleLatency)
{
    auto composed_latency_graph = configured_module_graph(
        *revision, graph_jit_composed_latency_module_id);
    ASSERT_TRUE(composed_latency_graph);
    auto composed_latency_analysis =
        iv::graph_jit::detail::build_connection_analysis_plan(
            *composed_latency_graph, 64);
    ASSERT_TRUE(composed_latency_analysis.has_value())
        << (composed_latency_analysis
                ? std::string{}
                : composed_latency_analysis.error());
    auto composed_connection = std::ranges::find_if(
        composed_latency_analysis->sample_connections,
        [](auto const& connection) {
            return !connection.canonical_source_port
                && connection.source_channel_timings.size() == 2;
        });
    ASSERT_NE(
        composed_connection,
        composed_latency_analysis->sample_connections.end());
    ASSERT_EQ(composed_connection->source_channel_timings.size(), 2u);
    EXPECT_EQ(composed_connection->source_channel_timings[0].read_latency, 7u);
    EXPECT_EQ(composed_connection->source_channel_timings[1].read_latency, 2u);

    auto composed_latency = compile_graph(composed_latency_graph, 120);
    ASSERT_TRUE(composed_latency.succeeded())
        << (composed_latency.diagnostics.empty()
                ? ""
                : composed_latency.diagnostics.front().message);
    ASSERT_EQ(composed_latency.compiled_graph->node_layout.nodes.size(), 3u);

    auto composed_latency_storage =
        composed_latency.compiled_graph->node_layout.create_storage(resources);
    composed_latency_storage.initialize();
    StereoSampleConsumerProbeStateMirror* composed_probe = nullptr;
    for (std::size_t i = 0;
         i < composed_latency.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const& node = composed_latency.compiled_graph->node_layout.nodes[i];
        if (node.state_size == sizeof(StereoSampleConsumerProbeStateMirror)) {
            ASSERT_EQ(composed_probe, nullptr);
            composed_probe = static_cast<StereoSampleConsumerProbeStateMirror*>(
                composed_latency_storage.state_ptr(i));
        }
    }
    ASSERT_NE(composed_probe, nullptr);

    composed_latency.compiled_graph->root_operations.tick_block(
        composed_latency_storage.buffer().data(), 0, 64);
    EXPECT_EQ(composed_probe->calls, 1u);
    EXPECT_EQ(composed_probe->last_index, 0u);
    EXPECT_EQ(composed_probe->last_block_size, 64u);
    EXPECT_FLOAT_EQ(composed_probe->first_left, 0.0f);
    EXPECT_FLOAT_EQ(composed_probe->first_right, 0.0f);
    EXPECT_FLOAT_EQ(composed_probe->last_left, 56.0f);
    EXPECT_FLOAT_EQ(composed_probe->last_right, 56.0f);
    EXPECT_FLOAT_EQ(composed_probe->sum_left, 1596.0f);
    EXPECT_FLOAT_EQ(composed_probe->sum_right, 1596.0f);

    // The second call proves composition reads each producer's independently
    // restored history (7 frames from the direct source, 2 from the delayed
    // source) before gathering them into one zero-latency stereo input.
    composed_latency.compiled_graph->root_operations.tick_block(
        composed_latency_storage.buffer().data(), 64, 64);
    EXPECT_EQ(composed_probe->calls, 2u);
    EXPECT_EQ(composed_probe->last_index, 64u);
    EXPECT_EQ(composed_probe->last_block_size, 64u);
    EXPECT_FLOAT_EQ(composed_probe->first_left, 57.0f);
    EXPECT_FLOAT_EQ(composed_probe->first_right, 57.0f);
    EXPECT_FLOAT_EQ(composed_probe->last_left, 120.0f);
    EXPECT_FLOAT_EQ(composed_probe->last_right, 120.0f);
    EXPECT_FLOAT_EQ(composed_probe->sum_left, 5664.0f);
    EXPECT_FLOAT_EQ(composed_probe->sum_right, 5664.0f);

    // A composed input with history must extend each producer's retention by
    // the target history before gathering the independently delayed channels.
    // The direct channel therefore needs 5 + 7 frames while the delayed
    // channel needs 5 + 2. The synthetic stereo representation itself is
    // timestamp-aligned and exposes those five historical frames at latency 0.
}

TEST_F(GraphJitRuntimeFixture, ComposedSampleHistory)
{
    auto composed_history_graph = configured_module_graph(
        *revision, graph_jit_composed_history_module_id);
    ASSERT_TRUE(composed_history_graph);
    auto composed_history_analysis =
        iv::graph_jit::detail::build_connection_analysis_plan(
            *composed_history_graph, 64);
    ASSERT_TRUE(composed_history_analysis.has_value())
        << (composed_history_analysis
                ? std::string{}
                : composed_history_analysis.error());

    auto composed_history_connection = std::ranges::find_if(
        composed_history_analysis->sample_connections,
        [](auto const& connection) {
            return !connection.canonical_source_port
                && connection.source_channel_timings.size() == 2;
        });
    ASSERT_NE(
        composed_history_connection,
        composed_history_analysis->sample_connections.end());
    ASSERT_EQ(composed_history_connection->source_channel_timings.size(), 2u);
    EXPECT_EQ(composed_history_connection->target_history, 5u);
    EXPECT_EQ(
        composed_history_connection->source_channel_timings[0].read_latency,
        7u);
    EXPECT_EQ(
        composed_history_connection->source_channel_timings[1].read_latency,
        2u);

    auto producer_group_for = [&](auto const& channel) {
        auto const source_port = iv::NodeBundlePortId{
            channel.source.bundle,
            iv::PortKind::sample,
            channel.source.port,
        };
        return std::ranges::find_if(
            composed_history_analysis->sample_producer_groups,
            [&](auto const& group) {
                return group.source_port && *group.source_port == source_port;
            });
    };
    auto direct_history_group = producer_group_for(
        composed_history_connection->source_channel_timings[0]);
    auto delayed_history_group = producer_group_for(
        composed_history_connection->source_channel_timings[1]);
    ASSERT_NE(
        direct_history_group,
        composed_history_analysis->sample_producer_groups.end());
    ASSERT_NE(
        delayed_history_group,
        composed_history_analysis->sample_producer_groups.end());
    EXPECT_EQ(direct_history_group->requirements.retained_frames, 12u);
    EXPECT_EQ(delayed_history_group->requirements.retained_frames, 7u);

    auto composed_history_physical =
        iv::graph_jit::detail::build_sample_physical_plan(
            *composed_history_analysis, 64);
    ASSERT_TRUE(composed_history_physical.has_value())
        << (composed_history_physical
                ? std::string{}
                : composed_history_physical.error());
    ASSERT_EQ(composed_history_physical->compositions.size(), 1u);
    EXPECT_EQ(composed_history_physical->compositions[0].target_history, 5u);
    auto const composed_history_representation =
        composed_history_physical->compositions[0].target_representation;
    ASSERT_LT(
        composed_history_representation,
        composed_history_physical->representations.size());
    EXPECT_EQ(
        composed_history_physical->representations[composed_history_representation]
            .frame_capacity,
        128u);

    auto composed_history = compile_graph(composed_history_graph, 121);
    ASSERT_TRUE(composed_history.succeeded())
        << (composed_history.diagnostics.empty()
                ? ""
                : composed_history.diagnostics.front().message);
    ASSERT_EQ(composed_history.compiled_graph->node_layout.nodes.size(), 3u);

    auto composed_history_storage =
        composed_history.compiled_graph->node_layout.create_storage(resources);
    composed_history_storage.initialize();
    StereoHistoryConsumerStateMirror* composed_history_probe = nullptr;
    for (std::size_t i = 0;
         i < composed_history.compiled_graph->node_layout.nodes.size(); ++i) {
        if (composed_history.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(StereoHistoryConsumerStateMirror)) {
            ASSERT_EQ(composed_history_probe, nullptr);
            composed_history_probe =
                static_cast<StereoHistoryConsumerStateMirror*>(
                    composed_history_storage.state_ptr(i));
        }
    }
    ASSERT_NE(composed_history_probe, nullptr);

    composed_history.compiled_graph->root_operations.tick_block(
        composed_history_storage.buffer().data(), 0, 64);
    EXPECT_EQ(composed_history_probe->calls, 1u);
    EXPECT_EQ(composed_history_probe->last_index, 0u);
    EXPECT_FLOAT_EQ(composed_history_probe->current_left, 0.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->current_right, 0.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_left, 0.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_right, 0.0f);

    // At absolute index 64 both channels represent source frame 57 after path
    // equalization, and get(5) must reconstruct source frame 52 on each side.
    composed_history.compiled_graph->root_operations.tick_block(
        composed_history_storage.buffer().data(), 64, 64);
    EXPECT_EQ(composed_history_probe->calls, 2u);
    EXPECT_EQ(composed_history_probe->last_index, 64u);
    EXPECT_FLOAT_EQ(composed_history_probe->current_left, 57.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->current_right, 57.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_left, 52.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_right, 52.0f);
    EXPECT_EQ(composed_history_probe->marker, 0xa1b2c3d4e5f60718ull);

    // A short third block catches composition code that accidentally assumes
    // the kernel's configured block size while reconstructing target history.
    composed_history.compiled_graph->root_operations.tick_block(
        composed_history_storage.buffer().data(), 128, 4);
    EXPECT_EQ(composed_history_probe->calls, 3u);
    EXPECT_EQ(composed_history_probe->last_index, 128u);
    EXPECT_FLOAT_EQ(composed_history_probe->current_left, 121.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->current_right, 121.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_left, 116.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_right, 116.0f);

    // Split a valid whole-port projected composition into two configured mono
    // target-channel contributions. Connection analysis must normalize them
    // back into one full stereo logical input, preserving both the source
    // permutation (stereo right -> target left) and independent path latency.
}

TEST_F(GraphJitRuntimeFixture, ProjectedSampleComposition)
{
    auto projected_base_graph = configured_module_graph(
        *revision, graph_jit_projected_composition_module_id);
    ASSERT_TRUE(projected_base_graph);
    auto projected_graph = std::make_shared<iv::ConfiguredGraph>(*projected_base_graph);
    std::vector<iv::ConfiguredSampleConnection> projected_connections;
    for (auto const& connection :
         projected_base_graph->connections.configured_sample_connections()) {
        if (connection.source_type == iv::ChannelTypeId::stereo
            && connection.target_type == iv::ChannelTypeId::stereo
            && connection.source_channels.size() == 2
            && connection.target_channels.size() == 2) {
            for (std::size_t channel = 0; channel < 2; ++channel) {
                projected_connections.push_back(iv::ConfiguredSampleConnection{
                    .source_type = iv::ChannelTypeId::mono,
                    .source_channels = {connection.source_channels[channel]},
                    .target_type = iv::ChannelTypeId::mono,
                    .target_channels = {connection.target_channels[channel]},
                });
            }
        } else {
            projected_connections.push_back(connection);
        }
    }
    ASSERT_EQ(projected_connections.size(), 3u);
    projected_graph->connections = iv::GraphBuilderConnections::from_configured_connections(
        projected_connections,
        projected_base_graph->connections.configured_event_connections());

    auto projected_analysis = iv::graph_jit::detail::build_connection_analysis_plan(
        *projected_graph, 64);
    ASSERT_TRUE(projected_analysis.has_value())
        << (projected_analysis ? std::string{} : projected_analysis.error());
    // The two target-channel contributions normalize into one logical sink
    // connection, alongside the mono connection feeding FiveSampleDelay.
    ASSERT_EQ(projected_analysis->sample_connections.size(), 2u);
    auto projected_connection = std::ranges::find_if(
        projected_analysis->sample_connections,
        [](auto const& connection) {
            return connection.target_type == iv::ChannelTypeId::stereo;
        });
    ASSERT_NE(projected_connection, projected_analysis->sample_connections.end());
    EXPECT_FALSE(projected_connection->canonical_source_port.has_value());
    ASSERT_EQ(projected_connection->source_channel_timings.size(), 2u);
    ASSERT_EQ(projected_connection->target_channels.size(), 2u);
    EXPECT_EQ(projected_connection->source_channel_timings[0].source.channel, 1u);
    EXPECT_EQ(projected_connection->source_channel_timings[0].read_latency, 7u);
    EXPECT_EQ(projected_connection->source_channel_timings[1].source.channel, 0u);
    EXPECT_EQ(projected_connection->source_channel_timings[1].read_latency, 2u);
    EXPECT_EQ(projected_connection->target_channels[0].channel, 0u);
    EXPECT_EQ(projected_connection->target_channels[1].channel, 1u);

    auto projected_physical = iv::graph_jit::detail::build_sample_physical_plan(
        *projected_analysis, 64);
    ASSERT_TRUE(projected_physical.has_value())
        << (projected_physical ? std::string{} : projected_physical.error());
    ASSERT_EQ(projected_physical->compositions.size(), 1u);
    ASSERT_EQ(projected_physical->compositions[0].sources.size(), 2u);
    EXPECT_EQ(projected_physical->compositions[0].sources[0].source_channel, 1u);
    EXPECT_EQ(projected_physical->compositions[0].sources[0].target_channel, 0u);
    EXPECT_EQ(projected_physical->compositions[0].sources[0].read_latency, 7u);
    EXPECT_EQ(projected_physical->compositions[0].sources[1].source_channel, 0u);
    EXPECT_EQ(projected_physical->compositions[0].sources[1].target_channel, 1u);
    EXPECT_EQ(projected_physical->compositions[0].sources[1].read_latency, 2u);

    auto projected = compile_graph(projected_graph, 122);
    ASSERT_TRUE(projected.succeeded())
        << (projected.diagnostics.empty()
                ? ""
                : projected.diagnostics.front().message);
    auto projected_storage =
        projected.compiled_graph->node_layout.create_storage(resources);
    projected_storage.initialize();
    StereoSampleConsumerProbeStateMirror* projected_probe = nullptr;
    for (std::size_t i = 0;
         i < projected.compiled_graph->node_layout.nodes.size(); ++i) {
        if (projected.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(StereoSampleConsumerProbeStateMirror)) {
            ASSERT_EQ(projected_probe, nullptr);
            projected_probe = static_cast<StereoSampleConsumerProbeStateMirror*>(
                projected_storage.state_ptr(i));
        }
    }
    ASSERT_NE(projected_probe, nullptr);

    projected.compiled_graph->root_operations.tick_block(
        projected_storage.buffer().data(), 0, 64);
    EXPECT_EQ(projected_probe->calls, 1u);
    EXPECT_FLOAT_EQ(projected_probe->first_left, 0.0f);
    EXPECT_FLOAT_EQ(projected_probe->first_right, 0.0f);
    EXPECT_FLOAT_EQ(projected_probe->last_left, 1056.0f);
    EXPECT_FLOAT_EQ(projected_probe->last_right, 56.0f);
    EXPECT_FLOAT_EQ(projected_probe->sum_left, 58596.0f);
    EXPECT_FLOAT_EQ(projected_probe->sum_right, 1596.0f);

    projected.compiled_graph->root_operations.tick_block(
        projected_storage.buffer().data(), 64, 64);
    EXPECT_EQ(projected_probe->calls, 2u);
    EXPECT_FLOAT_EQ(projected_probe->first_left, 1057.0f);
    EXPECT_FLOAT_EQ(projected_probe->first_right, 57.0f);
    EXPECT_FLOAT_EQ(projected_probe->last_left, 1120.0f);
    EXPECT_FLOAT_EQ(projected_probe->last_right, 120.0f);
    EXPECT_FLOAT_EQ(projected_probe->sum_left, 69664.0f);
    EXPECT_FLOAT_EQ(projected_probe->sum_right, 5664.0f);

}

TEST_F(GraphJitRuntimeFixture, DirectEventFlow)
{
    auto direct_event_graph = configured_module_graph(
        *revision, graph_jit_direct_event_module_id);
    ASSERT_TRUE(direct_event_graph);
    auto direct_event_analysis =
        iv::graph_jit::detail::build_connection_analysis_plan(
            *direct_event_graph, 64);
    ASSERT_TRUE(direct_event_analysis.has_value())
        << (direct_event_analysis
                ? std::string{}
                : direct_event_analysis.error());
    ASSERT_EQ(direct_event_analysis->event_connections.size(), 1u);
    ASSERT_EQ(direct_event_analysis->event_producer_groups.size(), 1u);
    auto const& direct_event_group =
        direct_event_analysis->event_producer_groups.front();
    ASSERT_TRUE(direct_event_group.implementation.has_value());
    EXPECT_EQ(
        *direct_event_group.implementation,
        iv::EventConnectionImplementationKind::direct);
    EXPECT_EQ(direct_event_group.sources.size(), 1u);
    EXPECT_EQ(direct_event_group.connection_indices.size(), 1u);

    auto direct_event = compile_graph(direct_event_graph, 123);
    ASSERT_TRUE(direct_event.succeeded())
        << (direct_event.diagnostics.empty()
                ? ""
                : direct_event.diagnostics.front().message);
    ASSERT_EQ(direct_event.compiled_graph->node_layout.nodes.size(), 2u);
    ASSERT_EQ(count_raw_regions(direct_event.compiled_graph->node_layout), 1u);

    auto direct_event_storage =
        direct_event.compiled_graph->node_layout.create_storage(resources);
    direct_event_storage.initialize();
    EventConsumerProbeStateMirror* direct_event_probe = nullptr;
    for (std::size_t i = 0;
         i < direct_event.compiled_graph->node_layout.nodes.size(); ++i) {
        if (direct_event.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            ASSERT_EQ(direct_event_probe, nullptr);
            direct_event_probe = static_cast<EventConsumerProbeStateMirror*>(
                direct_event_storage.state_ptr(i));
        }
    }
    ASSERT_NE(direct_event_probe, nullptr);

    direct_event.compiled_graph->root_operations.tick_block(
        direct_event_storage.buffer().data(), 0, 64);
    EXPECT_EQ(direct_event_probe->calls, 1u);
    EXPECT_EQ(direct_event_probe->last_index, 0u);
    EXPECT_EQ(direct_event_probe->last_block_size, 64u);
    EXPECT_EQ(direct_event_probe->event_count, 2u);
    EXPECT_EQ(direct_event_probe->trigger_count, 2u);
    EXPECT_EQ(direct_event_probe->first_time, 3u);
    EXPECT_EQ(direct_event_probe->last_time, 63u);
    EXPECT_EQ(direct_event_probe->marker, 0xe71e17u);

    // Direct event storage is per-root-invocation scratch: the producer clears
    // the previous bounded sequence before publishing this block, and the
    // consumer observes only timestamps in the new block.
    direct_event.compiled_graph->root_operations.tick_block(
        direct_event_storage.buffer().data(), 64, 64);
    EXPECT_EQ(direct_event_probe->calls, 2u);
    EXPECT_EQ(direct_event_probe->last_index, 64u);
    EXPECT_EQ(direct_event_probe->event_count, 2u);
    EXPECT_EQ(direct_event_probe->trigger_count, 2u);
    EXPECT_EQ(direct_event_probe->first_time, 67u);
    EXPECT_EQ(direct_event_probe->last_time, 127u);

}

TEST_F(GraphJitRuntimeFixture, ExactTypeEventDetachFeedback)
{
    auto feedback_graph = configured_event_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = iv::graph_jit::detail::build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const detached = std::ranges::find_if(
        analysis->event_connections,
        [](iv::graph_jit::detail::EventConnectionPlan const& connection) { return connection.detach.has_value(); });
    ASSERT_NE(detached, analysis->event_connections.end());
    ASSERT_TRUE(detached->detach.has_value());
    EXPECT_EQ(detached->detach->loop_extra_latency, 10u);
    ASSERT_TRUE(detached->detach_region.has_value());
    ASSERT_LT(*detached->detach_region, analysis->schedule.regions.size());
    auto const& region = analysis->schedule.regions[*detached->detach_region];
    ASSERT_TRUE(region.cyclic);
    EXPECT_EQ(region.maximum_block_size, 8u);
    EXPECT_EQ(region.scc_feedback_latency, 8u);

    auto compiled = compile_graph(feedback_graph, 124);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    EventFeedbackAStateMirror* state_a = nullptr;
    EventFeedbackBStateMirror* state_b = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            compiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(EventFeedbackAStateMirror)) {
            ASSERT_EQ(state_a, nullptr);
            state_a = static_cast<EventFeedbackAStateMirror*>(storage.state_ptr(i));
        } else if (state_size == sizeof(EventFeedbackBStateMirror)) {
            ASSERT_EQ(state_b, nullptr);
            state_b = static_cast<EventFeedbackBStateMirror*>(storage.state_ptr(i));
        }
    }
    ASSERT_NE(state_a, nullptr);
    ASSERT_NE(state_b, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), 0, 64);

    ASSERT_EQ(state_a->calls, 8u);
    ASSERT_EQ(state_b->calls, 8u);
    EXPECT_EQ(state_a->scc_feedback_latency, 8u);
    EXPECT_EQ(state_b->scc_feedback_latency, 8u);
    EXPECT_EQ(state_a->marker, 0xa11ce001u);
    EXPECT_EQ(state_b->marker, 0xb22ce002ull);

    for (std::size_t slice = 0; slice < 8; ++slice) {
        auto const index = slice * 8u;
        EXPECT_EQ(state_a->indices[slice], index);
        EXPECT_EQ(state_b->indices[slice], index);
        EXPECT_EQ(state_a->block_sizes[slice], 8u);
        EXPECT_EQ(state_b->block_sizes[slice], 8u);

        // B runs before A in the explicit same-slice order. B publishes at +2,
        // so A observes that event directly in the current SCC slice.
        EXPECT_EQ(state_a->input_counts[slice], 1u);
        EXPECT_EQ(state_a->first_input_times[slice], index + 2u);

        // A's output is detached by exactly 10 samples. The first event lands at
        // time 11 and therefore first becomes visible to B in slice [8, 16).
        if (slice == 0) {
            EXPECT_EQ(state_b->input_counts[slice], 0u);
            EXPECT_EQ(state_b->first_input_times[slice], 0u);
        } else {
            EXPECT_EQ(state_b->input_counts[slice], 1u);
            EXPECT_EQ(state_b->first_input_times[slice], (slice - 1u) * 8u + 11u);
        }
    }

    // The last event emitted by A in the first root call is at 57. Detach adds
    // 10, so the persistent feedback ring must carry time 67 across both the
    // root boundary and a graph-generation migration. This also proves
    // transport latency (10) is independent of the SCC scheduling quantum (8).
    auto recompiled = compile_graph(feedback_graph, 125);
    ASSERT_TRUE(recompiled.succeeded())
        << (recompiled.diagnostics.empty()
                ? ""
                : recompiled.diagnostics.front().message);
    auto migrated_storage =
        recompiled.compiled_graph->node_layout.create_storage(resources);
    migrated_storage.initialize(&storage);
    EventFeedbackAStateMirror* migrated_a = nullptr;
    EventFeedbackBStateMirror* migrated_b = nullptr;
    for (std::size_t i = 0;
         i < recompiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            recompiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(EventFeedbackAStateMirror)) {
            migrated_a = static_cast<EventFeedbackAStateMirror*>(
                migrated_storage.state_ptr(i));
        } else if (state_size == sizeof(EventFeedbackBStateMirror)) {
            migrated_b = static_cast<EventFeedbackBStateMirror*>(
                migrated_storage.state_ptr(i));
        }
    }
    ASSERT_NE(migrated_a, nullptr);
    ASSERT_NE(migrated_b, nullptr);
    recompiled.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), 64, 8);
    ASSERT_EQ(migrated_a->calls, 1u);
    ASSERT_EQ(migrated_b->calls, 1u);
    EXPECT_EQ(migrated_a->indices[0], 64u);
    EXPECT_EQ(migrated_b->indices[0], 64u);
    EXPECT_EQ(migrated_a->block_sizes[0], 8u);
    EXPECT_EQ(migrated_b->block_sizes[0], 8u);
    EXPECT_EQ(migrated_a->input_counts[0], 1u);
    EXPECT_EQ(migrated_a->first_input_times[0], 66u);
    EXPECT_EQ(migrated_b->input_counts[0], 1u);
    EXPECT_EQ(migrated_b->first_input_times[0], 67u);

    // Legal root block sizes smaller than the SCC quantum must preserve exact
    // absolute event time across changing invocation boundaries.
    auto tail_storage =
        compiled.compiled_graph->node_layout.create_storage(resources);
    tail_storage.initialize();
    EventFeedbackAStateMirror* tail_a = nullptr;
    EventFeedbackBStateMirror* tail_b = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            compiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(EventFeedbackAStateMirror)) {
            tail_a = static_cast<EventFeedbackAStateMirror*>(
                tail_storage.state_ptr(i));
        } else if (state_size == sizeof(EventFeedbackBStateMirror)) {
            tail_b = static_cast<EventFeedbackBStateMirror*>(
                tail_storage.state_ptr(i));
        }
    }
    ASSERT_NE(tail_a, nullptr);
    ASSERT_NE(tail_b, nullptr);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), 0, 4);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), 4, 2);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), 6, 1);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), 7, 8);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), 15, 1);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), 16, 2);

    std::array<std::uint64_t, 6> const expected_indices{0, 4, 6, 7, 15, 16};
    std::array<std::uint64_t, 6> const expected_sizes{4, 2, 1, 8, 1, 2};
    std::array<std::uint64_t, 6> const expected_a_times{2, 5, 6, 9, 15, 17};
    std::array<std::uint64_t, 6> const expected_b_counts{0, 0, 0, 1, 1, 1};
    std::array<std::uint64_t, 6> const expected_b_times{0, 0, 0, 11, 15, 16};
    ASSERT_EQ(tail_a->calls, 6u);
    ASSERT_EQ(tail_b->calls, 6u);
    EXPECT_EQ(tail_a->scc_feedback_latency, 8u);
    EXPECT_EQ(tail_b->scc_feedback_latency, 8u);
    for (std::size_t i = 0; i < expected_indices.size(); ++i) {
        EXPECT_EQ(tail_a->indices[i], expected_indices[i]);
        EXPECT_EQ(tail_b->indices[i], expected_indices[i]);
        EXPECT_EQ(tail_a->block_sizes[i], expected_sizes[i]);
        EXPECT_EQ(tail_b->block_sizes[i], expected_sizes[i]);
        EXPECT_EQ(tail_a->input_counts[i], 1u);
        EXPECT_EQ(tail_a->first_input_times[i], expected_a_times[i]);
        EXPECT_EQ(tail_b->input_counts[i], expected_b_counts[i]);
        EXPECT_EQ(tail_b->first_input_times[i], expected_b_times[i]);
    }
}

TEST_F(GraphJitRuntimeFixture, EventDetachFeedbackFanoutSharesDelayedStream)
{
    auto feedback_graph = configured_event_feedback_fanout_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto const configured_events =
        feedback_graph->connections.configured_event_connections();
    EXPECT_EQ(
        std::ranges::count_if(
            configured_events,
            [](iv::ConfiguredEventConnection const& connection) {
                return connection.detach.has_value();
            }),
        2);

    auto compiled = compile_graph(feedback_graph, 126);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    EXPECT_EQ(
        std::ranges::count_if(
            compiled.compiled_graph->node_layout.regions,
            [](iv::NodeLayout::Region const& region) {
                return region.migration_identity.find(
                           "graphjit.event.feedback:")
                    != std::string::npos;
            }),
        1);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    std::vector<EventFeedbackBStateMirror*> consumers;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventFeedbackBStateMirror)) {
            consumers.push_back(static_cast<EventFeedbackBStateMirror*>(
                storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(consumers.size(), 2u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), 0, 24);
    for (auto const* state : consumers) {
        ASSERT_NE(state, nullptr);
        ASSERT_EQ(state->calls, 3u);
        EXPECT_EQ(state->scc_feedback_latency, 8u);
        EXPECT_EQ(state->indices[0], 0u);
        EXPECT_EQ(state->indices[1], 8u);
        EXPECT_EQ(state->indices[2], 16u);
        EXPECT_EQ(state->input_counts[0], 0u);
        EXPECT_EQ(state->input_counts[1], 1u);
        EXPECT_EQ(state->input_counts[2], 1u);
        EXPECT_EQ(state->first_input_times[1], 11u);
        EXPECT_EQ(state->first_input_times[2], 19u);
    }
}

TEST_F(GraphJitRuntimeFixture, EventDetachFeedbackRetainsBurstAcrossTinyBlocks)
{
    auto feedback_graph = configured_event_feedback_graph(
        *revision, graph_jit_event_feedback_burst_a_id);
    ASSERT_TRUE(feedback_graph);

    auto compiled = compile_graph(feedback_graph, 126);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const feedback_region = std::ranges::find_if(
        compiled.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.migration_identity.find(
                       "graphjit.event.feedback:")
                != std::string::npos;
        });
    ASSERT_NE(
        feedback_region,
        compiled.compiled_graph->node_layout.regions.end());
    EXPECT_NE(
        feedback_region->migration_identity.find("capacity=256"),
        std::string::npos);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventFeedbackBStateMirror* state_b = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventFeedbackBStateMirror)) {
            ASSERT_EQ(state_b, nullptr);
            state_b = static_cast<EventFeedbackBStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(state_b, nullptr);

    // Each one-sample root call legally fills the entire 16-event producer
    // sequence at one timestamp. Ten delayed calls can therefore be pending at
    // once; a capacity derived from 64-frame root windows would silently lose
    // events here.
    for (std::size_t index = 0; index < 15; ++index) {
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(), index, 1);
    }

    ASSERT_EQ(state_b->calls, 15u);
    EXPECT_EQ(state_b->scc_feedback_latency, 8u);
    for (std::size_t index = 0; index < 10; ++index) {
        EXPECT_EQ(state_b->input_counts[index], 0u);
    }
    for (std::size_t index = 10; index < 15; ++index) {
        EXPECT_EQ(state_b->input_counts[index], 16u);
        EXPECT_EQ(state_b->first_input_times[index], index);
    }
}

TEST_F(GraphJitRuntimeFixture, TransientEventSlicing)
{
    auto transient_event_graph = configured_module_graph(
        *revision, graph_jit_transient_event_module_id);
    ASSERT_TRUE(transient_event_graph);
    auto transient_event_analysis =
        iv::graph_jit::detail::build_connection_analysis_plan(
            *transient_event_graph, 64);
    ASSERT_TRUE(transient_event_analysis.has_value())
        << (transient_event_analysis
                ? std::string{}
                : transient_event_analysis.error());
    ASSERT_EQ(transient_event_analysis->event_connections.size(), 1u);
    ASSERT_EQ(transient_event_analysis->event_producer_groups.size(), 1u);
    EXPECT_TRUE(
        transient_event_analysis->event_connections[0]
            .requires_block_materialization);
    auto const& transient_event_group =
        transient_event_analysis->event_producer_groups.front();
    ASSERT_TRUE(transient_event_group.implementation.has_value());
    EXPECT_EQ(
        *transient_event_group.implementation,
        iv::EventConnectionImplementationKind::transient_sequence);

    auto transient_event = compile_graph(transient_event_graph, 124);
    ASSERT_TRUE(transient_event.succeeded())
        << (transient_event.diagnostics.empty()
                ? ""
                : transient_event.diagnostics.front().message);
    ASSERT_EQ(transient_event.compiled_graph->node_layout.nodes.size(), 2u);
    // One producer sequence accumulates all 16-frame source slices; a second
    // transient sequence is materialized once for the sliced consumer.
    ASSERT_EQ(count_raw_regions(transient_event.compiled_graph->node_layout), 2u);

    auto transient_event_storage =
        transient_event.compiled_graph->node_layout.create_storage(resources);
    transient_event_storage.initialize();
    SlicedEventConsumerProbeStateMirror* transient_event_probe = nullptr;
    for (std::size_t i = 0;
         i < transient_event.compiled_graph->node_layout.nodes.size(); ++i) {
        if (transient_event.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(SlicedEventConsumerProbeStateMirror)) {
            ASSERT_EQ(transient_event_probe, nullptr);
            transient_event_probe =
                static_cast<SlicedEventConsumerProbeStateMirror*>(
                    transient_event_storage.state_ptr(i));
        }
    }
    ASSERT_NE(transient_event_probe, nullptr);

    transient_event.compiled_graph->root_operations.tick_block(
        transient_event_storage.buffer().data(), 0, 64);
    ASSERT_EQ(transient_event_probe->calls, 2u);
    EXPECT_EQ(transient_event_probe->indices[0], 0u);
    EXPECT_EQ(transient_event_probe->indices[1], 32u);
    EXPECT_EQ(transient_event_probe->block_sizes[0], 32u);
    EXPECT_EQ(transient_event_probe->block_sizes[1], 32u);
    EXPECT_EQ(transient_event_probe->event_counts[0], 4u);
    EXPECT_EQ(transient_event_probe->event_counts[1], 4u);
    EXPECT_EQ(transient_event_probe->trigger_counts[0], 4u);
    EXPECT_EQ(transient_event_probe->trigger_counts[1], 4u);
    EXPECT_EQ(transient_event_probe->first_times[0], 3u);
    EXPECT_EQ(transient_event_probe->last_times[0], 31u);
    EXPECT_EQ(transient_event_probe->first_times[1], 35u);
    EXPECT_EQ(transient_event_probe->last_times[1], 63u);

    // A short root block produces three 16-frame source slices and a 32+16
    // consumer split. The transient producer sequence is cleared once at the
    // root boundary, then all source slices append before materialization.
    transient_event.compiled_graph->root_operations.tick_block(
        transient_event_storage.buffer().data(), 64, 48);
    ASSERT_EQ(transient_event_probe->calls, 4u);
    EXPECT_EQ(transient_event_probe->indices[2], 64u);
    EXPECT_EQ(transient_event_probe->indices[3], 96u);
    EXPECT_EQ(transient_event_probe->block_sizes[2], 32u);
    EXPECT_EQ(transient_event_probe->block_sizes[3], 16u);
    EXPECT_EQ(transient_event_probe->event_counts[2], 4u);
    EXPECT_EQ(transient_event_probe->event_counts[3], 2u);
    EXPECT_EQ(transient_event_probe->trigger_counts[2], 4u);
    EXPECT_EQ(transient_event_probe->trigger_counts[3], 2u);
    EXPECT_EQ(transient_event_probe->first_times[2], 67u);
    EXPECT_EQ(transient_event_probe->last_times[2], 95u);
    EXPECT_EQ(transient_event_probe->first_times[3], 99u);
    EXPECT_EQ(transient_event_probe->last_times[3], 111u);

    // Event conversion is a transient physical operation owned by the producer
    // group. Two consumers requesting the same MIDI->trigger branch must share
    // one converted sequence rather than materializing the same fanout twice.
}

TEST_F(GraphJitRuntimeFixture, ConvertedEventFanout)
{
    auto converted_event_graph = configured_module_graph(
        *revision, graph_jit_converted_event_fanout_module_id);
    ASSERT_TRUE(converted_event_graph);
    auto converted_event_analysis =
        iv::graph_jit::detail::build_connection_analysis_plan(
            *converted_event_graph, 64);
    ASSERT_TRUE(converted_event_analysis.has_value())
        << (converted_event_analysis
                ? std::string{}
                : converted_event_analysis.error());
    ASSERT_EQ(converted_event_analysis->event_connections.size(), 2u);
    ASSERT_EQ(converted_event_analysis->event_producer_groups.size(), 1u);
    auto const& converted_event_group =
        converted_event_analysis->event_producer_groups.front();
    ASSERT_TRUE(converted_event_group.implementation.has_value());
    EXPECT_EQ(
        *converted_event_group.implementation,
        iv::EventConnectionImplementationKind::transient_sequence);
    ASSERT_EQ(converted_event_group.connection_indices.size(), 2u);
    for (auto const& connection : converted_event_analysis->event_connections) {
        EXPECT_EQ(connection.source_type, iv::EventTypeId::midi);
        EXPECT_EQ(connection.target_type, iv::EventTypeId::trigger);
        EXPECT_TRUE(connection.requires_conversion);
        EXPECT_EQ(connection.conversion.source_type, iv::EventTypeId::midi);
        EXPECT_EQ(connection.conversion.target_type, iv::EventTypeId::trigger);
        ASSERT_EQ(connection.conversion.step_count, 1u);
        EXPECT_EQ(
            connection.conversion.steps[0],
            iv::EventConversionStepId::midi_to_trigger);
    }

    auto converted_event = compile_graph(converted_event_graph, 125);
    ASSERT_TRUE(converted_event.succeeded())
        << (converted_event.diagnostics.empty()
                ? ""
                : converted_event.diagnostics.front().message);
    ASSERT_EQ(converted_event.compiled_graph->node_layout.nodes.size(), 3u);
    // Canonical MIDI producer sequence + one deduplicated trigger sequence.
    ASSERT_EQ(count_raw_regions(converted_event.compiled_graph->node_layout), 2u);

    auto converted_event_storage =
        converted_event.compiled_graph->node_layout.create_storage(resources);
    converted_event_storage.initialize();
    std::vector<EventConsumerProbeStateMirror*> converted_event_probes;
    for (std::size_t i = 0;
         i < converted_event.compiled_graph->node_layout.nodes.size(); ++i) {
        if (converted_event.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            converted_event_probes.push_back(
                static_cast<EventConsumerProbeStateMirror*>(
                    converted_event_storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(converted_event_probes.size(), 2u);

    converted_event.compiled_graph->root_operations.tick_block(
        converted_event_storage.buffer().data(), 0, 64);
    for (auto const* probe : converted_event_probes) {
        EXPECT_EQ(probe->calls, 1u);
        EXPECT_EQ(probe->event_count, 2u);
        EXPECT_EQ(probe->trigger_count, 2u);
        EXPECT_EQ(probe->first_time, 5u);
        EXPECT_EQ(probe->last_time, 17u);
    }

    converted_event.compiled_graph->root_operations.tick_block(
        converted_event_storage.buffer().data(), 64, 64);
    for (auto const* probe : converted_event_probes) {
        EXPECT_EQ(probe->calls, 2u);
        EXPECT_EQ(probe->event_count, 2u);
        EXPECT_EQ(probe->trigger_count, 2u);
        EXPECT_EQ(probe->first_time, 69u);
        EXPECT_EQ(probe->last_time, 81u);
    }

    // Small retained event windows use a compact persistent carry. The
    // producer may publish into its authored future-latency window, while the
    // consumer asks for eight samples of history on the following root call.
}

TEST_F(GraphJitRuntimeFixture, CompactRetainedEvents)
{
    auto retained_event_graph = configured_module_graph(
        *revision, graph_jit_retained_event_module_id);
    ASSERT_TRUE(retained_event_graph);
    auto retained_event_analysis =
        iv::graph_jit::detail::build_connection_analysis_plan(
            *retained_event_graph, 64);
    ASSERT_TRUE(retained_event_analysis.has_value())
        << (retained_event_analysis
                ? std::string{}
                : retained_event_analysis.error());
    ASSERT_EQ(retained_event_analysis->event_connections.size(), 1u);
    ASSERT_EQ(retained_event_analysis->event_producer_groups.size(), 1u);
    auto const& retained_event_connection =
        retained_event_analysis->event_connections.front();
    EXPECT_EQ(retained_event_connection.source_latency, 8u);
    EXPECT_EQ(retained_event_connection.target_history, 8u);
    EXPECT_DOUBLE_EQ(retained_event_connection.max_events_per_sample, 0.5);
    auto const& retained_event_group =
        retained_event_analysis->event_producer_groups.front();
    EXPECT_DOUBLE_EQ(retained_event_group.max_events_per_sample, 0.5);
    ASSERT_TRUE(retained_event_group.implementation.has_value());
    EXPECT_EQ(
        *retained_event_group.implementation,
        iv::EventConnectionImplementationKind::compact_persistent_carry);
    EXPECT_EQ(retained_event_group.requirements.retained_window_samples, 16u);
    ASSERT_TRUE(retained_event_group.requirements.retained_event_capacity);
    EXPECT_EQ(
        *retained_event_group.requirements.retained_event_capacity,
        8u);
    EXPECT_LE(
        *retained_event_group.requirements.retained_event_capacity,
        iv::EventConnectionCostModel{}.compact_carry_max_events);

    auto retained_event = compile_graph(retained_event_graph, 126);
    ASSERT_TRUE(retained_event.succeeded())
        << (retained_event.diagnostics.empty()
                ? ""
                : retained_event.diagnostics.front().message);
    ASSERT_EQ(retained_event.compiled_graph->node_layout.nodes.size(), 2u);
    // Transient working sequence + persistent compact carry sequence.
    ASSERT_EQ(count_raw_regions(retained_event.compiled_graph->node_layout), 2u);
    auto retained_persistent_region = std::ranges::find_if(
        retained_event.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw
                && !region.migration_identity.empty();
        });
    ASSERT_NE(
        retained_persistent_region,
        retained_event.compiled_graph->node_layout.regions.end());
    EXPECT_NE(
        retained_persistent_region->migration_identity.find(
            "graphjit.event:"),
        std::string::npos);

    auto retained_event_storage =
        retained_event.compiled_graph->node_layout.create_storage(resources);
    retained_event_storage.initialize();
    RetainedEventConsumerProbeStateMirror* retained_event_probe = nullptr;
    for (std::size_t i = 0;
         i < retained_event.compiled_graph->node_layout.nodes.size(); ++i) {
        if (retained_event.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(RetainedEventConsumerProbeStateMirror)) {
            ASSERT_EQ(retained_event_probe, nullptr);
            retained_event_probe =
                static_cast<RetainedEventConsumerProbeStateMirror*>(
                    retained_event_storage.state_ptr(i));
        }
    }
    ASSERT_NE(retained_event_probe, nullptr);

    retained_event.compiled_graph->root_operations.tick_block(
        retained_event_storage.buffer().data(), 0, 64);
    ASSERT_EQ(retained_event_probe->calls, 1u);
    EXPECT_EQ(retained_event_probe->indices[0], 0u);
    EXPECT_EQ(retained_event_probe->event_counts[0], 2u);
    EXPECT_EQ(retained_event_probe->first_times[0], 5u);
    EXPECT_EQ(retained_event_probe->last_times[0], 61u);

    // The compact carry committed [56, 72): timestamp 61 is historical input
    // on the next call and timestamp 67 was authored ahead by output latency.
    // Current-call events 69 and 125 are appended after the restored carry.
    retained_event.compiled_graph->root_operations.tick_block(
        retained_event_storage.buffer().data(), 64, 64);
    ASSERT_EQ(retained_event_probe->calls, 2u);
    EXPECT_EQ(retained_event_probe->indices[1], 64u);
    EXPECT_EQ(retained_event_probe->event_counts[1], 4u);
    EXPECT_EQ(retained_event_probe->first_times[1], 61u);
    EXPECT_EQ(retained_event_probe->second_times[1], 67u);
    EXPECT_EQ(retained_event_probe->last_times[1], 125u);

    // The next retained window is [120, 136), proving stale events from the
    // first call were trimmed rather than accumulating indefinitely.
    retained_event.compiled_graph->root_operations.tick_block(
        retained_event_storage.buffer().data(), 128, 64);
    ASSERT_EQ(retained_event_probe->calls, 3u);
    EXPECT_EQ(retained_event_probe->indices[2], 128u);
    EXPECT_EQ(retained_event_probe->event_counts[2], 4u);
    EXPECT_EQ(retained_event_probe->first_times[2], 125u);
    EXPECT_EQ(retained_event_probe->second_times[2], 131u);
    EXPECT_EQ(retained_event_probe->last_times[2], 189u);

    // Retention larger than the compact-carry budget binds producer and
    // consumer directly to one persistent ring. The ring keeps monotonic
    // read/write indices in NodeStorage and advances only the oldest retained
    // index as the 160-sample history window moves across root calls.
}

TEST_F(GraphJitRuntimeFixture, PersistentEventRing)
{
    auto persistent_event_ring_graph = configured_module_graph(
        *revision, graph_jit_persistent_event_ring_module_id);
    ASSERT_TRUE(persistent_event_ring_graph);
    auto persistent_event_ring_analysis =
        iv::graph_jit::detail::build_connection_analysis_plan(
            *persistent_event_ring_graph, 64);
    ASSERT_TRUE(persistent_event_ring_analysis.has_value())
        << (persistent_event_ring_analysis
                ? std::string{}
                : persistent_event_ring_analysis.error());
    ASSERT_EQ(persistent_event_ring_analysis->event_producer_groups.size(), 1u);
    auto const& persistent_event_ring_group =
        persistent_event_ring_analysis->event_producer_groups.front();
    ASSERT_TRUE(persistent_event_ring_group.implementation.has_value());
    EXPECT_EQ(
        *persistent_event_ring_group.implementation,
        iv::EventConnectionImplementationKind::persistent_ring);
    EXPECT_EQ(
        persistent_event_ring_group.requirements.retained_window_samples,
        160u);
    ASSERT_TRUE(
        persistent_event_ring_group.requirements.retained_event_capacity);
    EXPECT_EQ(
        *persistent_event_ring_group.requirements.retained_event_capacity,
        80u);
    EXPECT_GT(
        *persistent_event_ring_group.requirements.retained_event_capacity,
        iv::EventConnectionCostModel{}.compact_carry_max_events);

    auto persistent_event_ring = compile_graph(
        persistent_event_ring_graph, 127);
    ASSERT_TRUE(persistent_event_ring.succeeded())
        << (persistent_event_ring.diagnostics.empty()
                ? ""
                : persistent_event_ring.diagnostics.front().message);
    ASSERT_EQ(
        persistent_event_ring.compiled_graph->node_layout.nodes.size(),
        2u);
    // The persistent ring is the canonical producer representation: no
    // transient working copy or compact carry region is required.
    ASSERT_EQ(
        count_raw_regions(persistent_event_ring.compiled_graph->node_layout),
        1u);
    auto persistent_event_ring_region = std::ranges::find_if(
        persistent_event_ring.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw
                && !region.migration_identity.empty();
        });
    ASSERT_NE(
        persistent_event_ring_region,
        persistent_event_ring.compiled_graph->node_layout.regions.end());
    EXPECT_NE(
        persistent_event_ring_region->migration_identity.find(
            "kind=persistent_ring"),
        std::string::npos);

    auto persistent_event_ring_storage =
        persistent_event_ring.compiled_graph->node_layout.create_storage(resources);
    persistent_event_ring_storage.initialize();
    PersistentEventRingConsumerProbeStateMirror* persistent_event_ring_probe =
        nullptr;
    for (std::size_t i = 0;
         i < persistent_event_ring.compiled_graph->node_layout.nodes.size(); ++i) {
        if (persistent_event_ring.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(PersistentEventRingConsumerProbeStateMirror)) {
            ASSERT_EQ(persistent_event_ring_probe, nullptr);
            persistent_event_ring_probe =
                static_cast<PersistentEventRingConsumerProbeStateMirror*>(
                    persistent_event_ring_storage.state_ptr(i));
        }
    }
    ASSERT_NE(persistent_event_ring_probe, nullptr);

    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), 0, 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 1u);
    EXPECT_EQ(persistent_event_ring_probe->indices[0], 0u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[0], 32u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[0], 5u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[0], 5u);

    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), 64, 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 2u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[1], 64u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[1], 5u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[1], 69u);

    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), 128, 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 3u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[2], 96u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[2], 5u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[2], 133u);

    // At index 192 the requested history begins at sample 32, so the first
    // block's 32 co-timestamped events expire in-place. The retained payload
    // itself is not copied.
    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), 192, 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 4u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[3], 96u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[3], 69u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[3], 197u);

    // Advancing by another root block moves the history start to 96, expires
    // the second block, and appends through the physical end of the 128-slot
    // ring. The consumer therefore exercises a wrapped BlockView.
    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), 256, 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 5u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[4], 96u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[4], 133u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[4], 261u);

    // A retained canonical MIDI ring may simultaneously serve an identity
    // history consumer and a deduplicated converted trigger fanout. Conversion
    // materializes only the current consumer window, while target capacity
    // remains source-capacity-sized because the sizing rate does not constrain
    // timestamp clustering.
}

TEST_F(GraphJitRuntimeFixture, RetainedConvertedEventFanout)
{
    auto retained_converted_event_graph = configured_module_graph(
        *revision, graph_jit_retained_converted_event_fanout_module_id);
    ASSERT_TRUE(retained_converted_event_graph);
    auto retained_converted_event_analysis =
        iv::graph_jit::detail::build_connection_analysis_plan(
            *retained_converted_event_graph, 64);
    ASSERT_TRUE(retained_converted_event_analysis.has_value())
        << (retained_converted_event_analysis
                ? std::string{}
                : retained_converted_event_analysis.error());
    ASSERT_EQ(
        retained_converted_event_analysis->event_producer_groups.size(),
        1u);
    auto const& retained_converted_event_group =
        retained_converted_event_analysis->event_producer_groups.front();
    ASSERT_TRUE(retained_converted_event_group.implementation.has_value());
    EXPECT_EQ(
        *retained_converted_event_group.implementation,
        iv::EventConnectionImplementationKind::persistent_ring);
    EXPECT_EQ(
        retained_converted_event_group.requirements.retained_window_samples,
        160u);
    EXPECT_TRUE(retained_converted_event_group.requirements.requires_materialization);
    ASSERT_EQ(
        retained_converted_event_analysis->event_connections.size(),
        3u);
    EXPECT_EQ(
        std::ranges::count_if(
            retained_converted_event_analysis->event_connections,
            [](auto const& connection) {
                return connection.requires_conversion
                    && connection.target_type == iv::EventTypeId::trigger;
            }),
        2);

    EXPECT_EQ(
        std::ranges::count_if(
            retained_converted_event_analysis->event_connections,
            [](auto const& connection) {
                return connection.requires_block_materialization;
            }),
        1);

    auto retained_converted_event = compile_graph(
        retained_converted_event_graph, 128);
    ASSERT_TRUE(retained_converted_event.succeeded())
        << (retained_converted_event.diagnostics.empty()
                ? ""
                : retained_converted_event.diagnostics.front().message);
    ASSERT_EQ(
        retained_converted_event.compiled_graph->node_layout.nodes.size(),
        4u);
    // One persistent canonical MIDI ring + one deduplicated transient trigger
    // representation shared by both converted consumers.
    ASSERT_EQ(
        count_raw_regions(retained_converted_event.compiled_graph->node_layout),
        2u);
    auto retained_converted_persistent_region = std::ranges::find_if(
        retained_converted_event.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw
                && !region.migration_identity.empty();
        });
    auto retained_converted_transient_region = std::ranges::find_if(
        retained_converted_event.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw
                && region.migration_identity.empty();
        });
    ASSERT_NE(
        retained_converted_persistent_region,
        retained_converted_event.compiled_graph->node_layout.regions.end());
    ASSERT_NE(
        retained_converted_transient_region,
        retained_converted_event.compiled_graph->node_layout.regions.end());
    EXPECT_NE(
        retained_converted_persistent_region->migration_identity.find(
            "kind=persistent_ring"),
        std::string::npos);
    auto retained_converted_event_storage =
        retained_converted_event.compiled_graph->node_layout.create_storage(resources);
    retained_converted_event_storage.initialize();
    RetainedMidiConsumerProbeStateMirror* retained_midi_probe = nullptr;
    EventConsumerProbeStateMirror* retained_trigger_probe = nullptr;
    SlicedEventConsumerProbeStateMirror* retained_sliced_trigger_probe = nullptr;
    for (std::size_t i = 0;
         i < retained_converted_event.compiled_graph->node_layout.nodes.size();
         ++i) {
        auto const state_size =
            retained_converted_event.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(RetainedMidiConsumerProbeStateMirror)) {
            ASSERT_EQ(retained_midi_probe, nullptr);
            retained_midi_probe =
                static_cast<RetainedMidiConsumerProbeStateMirror*>(
                    retained_converted_event_storage.state_ptr(i));
        } else if (state_size == sizeof(EventConsumerProbeStateMirror)) {
            ASSERT_EQ(retained_trigger_probe, nullptr);
            retained_trigger_probe =
                static_cast<EventConsumerProbeStateMirror*>(
                    retained_converted_event_storage.state_ptr(i));
        } else if (state_size == sizeof(SlicedEventConsumerProbeStateMirror)) {
            ASSERT_EQ(retained_sliced_trigger_probe, nullptr);
            retained_sliced_trigger_probe =
                static_cast<SlicedEventConsumerProbeStateMirror*>(
                    retained_converted_event_storage.state_ptr(i));
        }
    }
    ASSERT_NE(retained_midi_probe, nullptr);
    ASSERT_NE(retained_trigger_probe, nullptr);
    ASSERT_NE(retained_sliced_trigger_probe, nullptr);

    retained_converted_event.compiled_graph->root_operations.tick_block(
        retained_converted_event_storage.buffer().data(), 0, 64);
    ASSERT_EQ(retained_midi_probe->calls, 1u);
    EXPECT_EQ(retained_midi_probe->event_counts[0], 3u);
    EXPECT_EQ(retained_midi_probe->midi_counts[0], 3u);
    EXPECT_EQ(retained_midi_probe->first_times[0], 5u);
    EXPECT_EQ(retained_midi_probe->last_times[0], 17u);
    EXPECT_EQ(retained_trigger_probe->calls, 1u);
    EXPECT_EQ(retained_trigger_probe->event_count, 2u);
    EXPECT_EQ(retained_trigger_probe->trigger_count, 2u);
    EXPECT_EQ(retained_trigger_probe->first_time, 5u);
    EXPECT_EQ(retained_trigger_probe->last_time, 17u);
    ASSERT_EQ(retained_sliced_trigger_probe->calls, 2u);
    EXPECT_EQ(retained_sliced_trigger_probe->indices[0], 0u);
    EXPECT_EQ(retained_sliced_trigger_probe->indices[1], 32u);
    EXPECT_EQ(retained_sliced_trigger_probe->event_counts[0], 2u);
    EXPECT_EQ(retained_sliced_trigger_probe->event_counts[1], 0u);

    retained_converted_event.compiled_graph->root_operations.tick_block(
        retained_converted_event_storage.buffer().data(), 64, 64);
    ASSERT_EQ(retained_midi_probe->calls, 2u);
    EXPECT_EQ(retained_midi_probe->event_counts[1], 6u);
    EXPECT_EQ(retained_midi_probe->midi_counts[1], 6u);
    EXPECT_EQ(retained_midi_probe->first_times[1], 5u);
    EXPECT_EQ(retained_midi_probe->last_times[1], 81u);
    EXPECT_EQ(retained_trigger_probe->calls, 2u);
    EXPECT_EQ(retained_trigger_probe->event_count, 2u);
    EXPECT_EQ(retained_trigger_probe->trigger_count, 2u);
    EXPECT_EQ(retained_trigger_probe->first_time, 69u);
    EXPECT_EQ(retained_trigger_probe->last_time, 81u);
    ASSERT_EQ(retained_sliced_trigger_probe->calls, 4u);
    EXPECT_EQ(retained_sliced_trigger_probe->indices[2], 64u);
    EXPECT_EQ(retained_sliced_trigger_probe->indices[3], 96u);
    EXPECT_EQ(retained_sliced_trigger_probe->event_counts[2], 2u);
    EXPECT_EQ(retained_sliced_trigger_probe->event_counts[3], 0u);
    // Although the persistent source now contains both root calls, the
    // converted transient contains only the current root window.
    auto const* converted_count = reinterpret_cast<std::size_t const*>(
        retained_converted_event_storage.buffer().data()
        + retained_converted_transient_region->storage_offset);
    EXPECT_EQ(*converted_count, 2u);

}

TEST_F(GraphJitRuntimeFixture, SampleHistoryCarryAndMigration)
{
    auto history_graph = configured_module_graph(
        *revision, graph_jit_history_fanout_module_id);
    ASSERT_TRUE(history_graph);
    auto history_analysis = iv::graph_jit::detail::build_connection_analysis_plan(
        *history_graph, 64);
    ASSERT_TRUE(history_analysis.has_value())
        << (history_analysis ? std::string{} : history_analysis.error());
    ASSERT_EQ(history_analysis->sample_producer_groups.size(), 1u);
    auto const& history_group = history_analysis->sample_producer_groups[0];
    ASSERT_TRUE(history_group.implementation.has_value());
    EXPECT_EQ(
        *history_group.implementation,
        iv::SampleConnectionImplementationKind::compact_persistent_carry);
    EXPECT_EQ(history_group.requirements.retained_frames, 7u);

    auto history_physical = iv::graph_jit::detail::build_sample_physical_plan(
        *history_analysis, 64);
    ASSERT_TRUE(history_physical.has_value())
        << (history_physical ? std::string{} : history_physical.error());
    ASSERT_EQ(history_physical->carry_operations.size(), 1u);
    ASSERT_EQ(history_physical->persistent_allocations.size(), 1u);
    ASSERT_EQ(history_physical->materializations.size(), 1u);
    EXPECT_EQ(
        history_physical->persistent_allocations[0].kind,
        iv::graph_jit::detail::SamplePersistentStorageKind::compact_carry);
    EXPECT_EQ(
        history_physical->persistent_allocations[0].size_bytes,
        7u * sizeof(iv::Sample));

    auto history = compile_graph(history_graph, 116);
    ASSERT_TRUE(history.succeeded())
        << (history.diagnostics.empty() ? "" : history.diagnostics.front().message);
    ASSERT_EQ(history.compiled_graph->node_layout.nodes.size(), 3u);
    ASSERT_EQ(count_raw_regions(history.compiled_graph->node_layout), 2u);
    auto history_persistent_region = std::ranges::find_if(
        history.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw
                && !region.migration_identity.empty();
        });
    ASSERT_NE(
        history_persistent_region,
        history.compiled_graph->node_layout.regions.end());
    EXPECT_EQ(history_persistent_region->size, 7u * sizeof(iv::Sample));

    auto history_storage =
        history.compiled_graph->node_layout.create_storage(resources);
    history_storage.initialize();
    HistoryRampSourceStateMirror* history_source_state = nullptr;
    HistoryConsumerStateMirror* history_mono_state = nullptr;
    StereoHistoryConsumerStateMirror* history_stereo_state = nullptr;
    for (std::size_t i = 0; i < history.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size = history.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(HistoryRampSourceStateMirror)) {
            history_source_state = static_cast<HistoryRampSourceStateMirror*>(
                history_storage.state_ptr(i));
        } else if (state_size == sizeof(HistoryConsumerStateMirror)) {
            history_mono_state = static_cast<HistoryConsumerStateMirror*>(
                history_storage.state_ptr(i));
        } else if (state_size == sizeof(StereoHistoryConsumerStateMirror)) {
            history_stereo_state = static_cast<StereoHistoryConsumerStateMirror*>(
                history_storage.state_ptr(i));
        }
    }
    ASSERT_NE(history_source_state, nullptr);
    ASSERT_NE(history_mono_state, nullptr);
    ASSERT_NE(history_stereo_state, nullptr);

    history.compiled_graph->root_operations.tick_block(
        history_storage.buffer().data(), 0, 64);
    EXPECT_FLOAT_EQ(history_source_state->previous_output, 0.0f);
    EXPECT_FLOAT_EQ(history_mono_state->current, 0.0f);
    EXPECT_FLOAT_EQ(history_mono_state->history_5, 0.0f);
    EXPECT_FLOAT_EQ(history_stereo_state->current_left, 0.0f);
    EXPECT_FLOAT_EQ(history_stereo_state->current_right, 0.0f);

    history.compiled_graph->root_operations.tick_block(
        history_storage.buffer().data(), 64, 64);
    EXPECT_FLOAT_EQ(history_source_state->previous_output, 63.0f);
    EXPECT_EQ(history_mono_state->last_index, 64u);
    EXPECT_FLOAT_EQ(history_mono_state->current, 62.0f);
    EXPECT_FLOAT_EQ(history_mono_state->history_1, 61.0f);
    EXPECT_FLOAT_EQ(history_mono_state->history_5, 57.0f);
    EXPECT_EQ(history_stereo_state->last_index, 64u);
    EXPECT_FLOAT_EQ(history_stereo_state->current_left, 62.0f);
    EXPECT_FLOAT_EQ(history_stereo_state->current_right, 62.0f);
    EXPECT_FLOAT_EQ(history_stereo_state->history_5_left, 57.0f);
    EXPECT_FLOAT_EQ(history_stereo_state->history_5_right, 57.0f);

    // A block shorter than the retained tail must combine restored older
    // samples with newly produced samples when committing the next carry.
    history.compiled_graph->root_operations.tick_block(
        history_storage.buffer().data(), 128, 4);
    EXPECT_FLOAT_EQ(history_source_state->previous_output, 127.0f);
    EXPECT_FLOAT_EQ(history_mono_state->current, 126.0f);
    EXPECT_FLOAT_EQ(history_mono_state->history_5, 121.0f);
    history.compiled_graph->root_operations.tick_block(
        history_storage.buffer().data(), 132, 4);
    EXPECT_FLOAT_EQ(history_source_state->previous_output, 131.0f);
    EXPECT_FLOAT_EQ(history_mono_state->current, 130.0f);
    EXPECT_FLOAT_EQ(history_mono_state->history_5, 125.0f);

    // Compile a distinct generation and migrate into its canonical NodeStorage.
    // The connection history lives in compiler-owned raw storage, so this
    // proves it survives independently of primitive State migration.
    auto history_next = compile_graph(history_graph, 117);
    ASSERT_TRUE(history_next.succeeded())
        << (history_next.diagnostics.empty()
                ? ""
                : history_next.diagnostics.front().message);
    auto history_next_storage =
        history_next.compiled_graph->node_layout.create_storage(resources);
    history_next_storage.initialize(&history_storage);
    HistoryRampSourceStateMirror* migrated_source = nullptr;
    HistoryConsumerStateMirror* migrated_mono = nullptr;
    for (std::size_t i = 0;
         i < history_next.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size = history_next.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(HistoryRampSourceStateMirror)) {
            migrated_source = static_cast<HistoryRampSourceStateMirror*>(
                history_next_storage.state_ptr(i));
        } else if (state_size == sizeof(HistoryConsumerStateMirror)) {
            migrated_mono = static_cast<HistoryConsumerStateMirror*>(
                history_next_storage.state_ptr(i));
        }
    }
    ASSERT_NE(migrated_source, nullptr);
    ASSERT_NE(migrated_mono, nullptr);
    history_next.compiled_graph->root_operations.tick_block(
        history_next_storage.buffer().data(), 136, 4);
    EXPECT_FLOAT_EQ(migrated_source->previous_output, 135.0f);
    EXPECT_FLOAT_EQ(migrated_mono->current, 134.0f);
    EXPECT_FLOAT_EQ(migrated_mono->history_1, 133.0f);
    EXPECT_FLOAT_EQ(migrated_mono->history_5, 129.0f);

}

TEST_F(GraphJitRuntimeFixture, PersistentSampleHistory)
{
    auto persistent_history_graph = configured_module_graph(
        *revision, graph_jit_persistent_history_module_id);
    ASSERT_TRUE(persistent_history_graph);
    auto persistent_history_analysis =
        iv::graph_jit::detail::build_connection_analysis_plan(
            *persistent_history_graph, 64);
    ASSERT_TRUE(persistent_history_analysis.has_value())
        << (persistent_history_analysis
                ? std::string{}
                : persistent_history_analysis.error());
    ASSERT_EQ(persistent_history_analysis->sample_producer_groups.size(), 1u);
    ASSERT_TRUE(
        persistent_history_analysis->sample_producer_groups[0]
            .implementation.has_value());
    EXPECT_EQ(
        *persistent_history_analysis->sample_producer_groups[0].implementation,
        iv::SampleConnectionImplementationKind::persistent_ring);
    EXPECT_EQ(
        persistent_history_analysis->sample_producer_groups[0]
            .requirements.retained_frames,
        5000u);

    auto persistent_history = compile_graph(persistent_history_graph, 118);
    ASSERT_TRUE(persistent_history.succeeded())
        << (persistent_history.diagnostics.empty()
                ? ""
                : persistent_history.diagnostics.front().message);
    ASSERT_EQ(count_raw_regions(persistent_history.compiled_graph->node_layout), 1u);
    auto const persistent_ring_region = std::ranges::find_if(
        persistent_history.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw;
        });
    ASSERT_NE(
        persistent_ring_region,
        persistent_history.compiled_graph->node_layout.regions.end());
    EXPECT_EQ(
        persistent_ring_region->size,
        8192u * sizeof(iv::Sample));
    EXPECT_FALSE(persistent_ring_region->migration_identity.empty());

    auto persistent_history_storage =
        persistent_history.compiled_graph->node_layout.create_storage(resources);
    persistent_history_storage.initialize();
    LargeHistoryConsumerStateMirror* large_history_state = nullptr;
    for (std::size_t i = 0;
         i < persistent_history.compiled_graph->node_layout.nodes.size(); ++i) {
        if (persistent_history.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(LargeHistoryConsumerStateMirror)) {
            large_history_state = static_cast<LargeHistoryConsumerStateMirror*>(
                persistent_history_storage.state_ptr(i));
        }
    }
    ASSERT_NE(large_history_state, nullptr);
    for (std::uint64_t index = 0; index <= 5056; index += 64) {
        persistent_history.compiled_graph->root_operations.tick_block(
            persistent_history_storage.buffer().data(), index, 64);
    }
    EXPECT_EQ(large_history_state->calls, 80u);
    EXPECT_EQ(large_history_state->last_index, 5056u);
    EXPECT_FLOAT_EQ(large_history_state->current, 5056.0f);
    EXPECT_FLOAT_EQ(large_history_state->history_1, 5055.0f);
    EXPECT_FLOAT_EQ(large_history_state->history_5000, 56.0f);

}

TEST_F(GraphJitRuntimeFixture, ExternalSampleBoundariesRejected)
{
    auto ported_graph = configured_module_graph(*revision, graph_jit_ported_module_id);
    ASSERT_TRUE(ported_graph);
    auto ported = compile_graph(ported_graph, 119);
    expect_lowering_failure(ported, "does not yet support external sample boundaries");

    auto disconnected_ported_graph =
        std::make_shared<iv::ConfiguredGraph>(*ported_graph);
    disconnected_ported_graph->connections = {};
    auto disconnected_ported = compile_graph(disconnected_ported_graph, 120);
    expect_lowering_failure(
        disconnected_ported, "does not yet support external sample boundaries");
}

TEST_F(GraphJitRuntimeFixture, CompiledGraphsRetainPackageAndOrcOwnership)
{
    auto stateful = compile(graph_jit_stateful_module_id, 130);
    ASSERT_TRUE(stateful.succeeded())
        << (stateful.diagnostics.empty() ? "" : stateful.diagnostics.front().message);
    auto stateful_survivor = stateful.compiled_graph;
    auto stateful_storage = stateful_survivor->node_layout.create_storage(resources);
    stateful_storage.initialize();
    auto* state = static_cast<StatefulProbeStateMirror*>(stateful_storage.state_ptr(0));
    auto* compiled = static_cast<StatefulProbeCompiledStateMirror*>(
        stateful_storage.compiled_state_ptr(0));
    ASSERT_NE(state, nullptr);
    ASSERT_NE(compiled, nullptr);
    stateful_survivor->root_operations.tick_block(
        stateful_storage.buffer().data(), 17, 32);
    stateful_survivor->root_operations.skip_block(
        stateful_storage.buffer().data(), 41, 16);
    stateful_survivor->root_operations.tick_block(
        stateful_storage.buffer().data(), 73, 64);

    auto pointer_graph = configured_module_graph(
        *revision, graph_jit_pointer_configured_module_id);
    ASSERT_TRUE(pointer_graph);
    auto pointer_result = compile_graph(pointer_graph, 131);
    ASSERT_TRUE(pointer_result.succeeded())
        << (pointer_result.diagnostics.empty()
                ? ""
                : pointer_result.diagnostics.front().message);
    auto pointer_survivor = pointer_result.compiled_graph;

    auto revision_weak = std::weak_ptr<iv::PackageRevision const>{revision};
    stateful = {};
    pointer_result = {};
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
