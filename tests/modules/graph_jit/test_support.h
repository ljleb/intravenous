#pragma once

// Shared fixture declarations and helpers used by multiple behavior groups.
// Keep helpers used by one group in that group's _gtest.cpp.

#include "../../module_test_utils.h"
#include "../../event_source_test_support.h"

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/graph_jit/activity_runtime.h>
#include <intravenous/graph_jit/connection_plan.h>
#include <intravenous/graph_jit/event_conversion_runtime.h>
#include <intravenous/graph_jit/event_retention_runtime.h>
#include <intravenous/graph_jit/sample_storage_plan.h>
#include <intravenous/graph_jit/transient_arena_plan.h>
#include <intravenous/node/resources.h>
#include <intravenous/module/builder_session.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/runtime/background_coverage_propagation.h>
#include <intravenous/runtime/background_evaluation_transaction.h>
#include <intravenous/runtime/configuration_expression.h>
#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/graph_executor.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/project_graph_graph_connections_bridge.h>
#include <intravenous/runtime/project_graph_graph_executor_bridge.h>
#include <intravenous/runtime/project_graph_graph_jit_bridge.h>
#include <intravenous/runtime/project_graph_node_instances_bridge.h>
#include <intravenous/runtime/package_jit.h>
#include <intravenous/runtime/realtime_produced_record.h>
#include <intravenous/runtime/startup_config.h>
#include <intravenous/runtime/tick_invocation_frame.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <unistd.h>

namespace iv::test::graph_jit_tests {

iv::GlobalIndex index(std::int64_t value);

iv::GlobalIndexRange range(std::int64_t begin, std::int64_t end);

iv::Sample sample_value_from_bytes(std::span<std::byte const> bytes);

bool configured_sample_connection_is_detached(
    iv::ConfiguredSampleConnection const& connection);

std::expected<iv::graph_jit::detail::ConnectionAnalysisPlan, std::string>
build_connection_analysis_plan(
    iv::ConfiguredGraph const& graph,
    std::size_t kernel_block_size,
    iv::RealtimeStorageCostModel const& cost_model = {});

extern iv::graph_jit::TickInvocationCall const empty_tick_invocation;

inline constexpr char graph_jit_stateful_module_id[] = "iv.test.graph_jit.state_context.stateful_module";

inline constexpr char graph_jit_background_execution_module_id[] = "iv.test.graph_jit.state_context.background_execution_module";

inline constexpr char graph_jit_multi_region_module_id[] =
    "iv.test.graph_jit.state_context.multi_region_module";

inline constexpr char graph_jit_native_batch_feedback_module_id[] =
    "iv.test.graph_jit.state_context.native_batch_feedback_module";

inline constexpr char graph_jit_native_batch_background_module_id[] =
    "iv.test.graph_jit.state_context.native_batch_background_module";

inline constexpr char graph_jit_native_batch_reverse_module_id[] =
    "iv.test.graph_jit.state_context.native_batch_reverse_module";

inline constexpr char graph_jit_activity_type_default_module_id[] =
    "iv.test.graph_jit.state_context.activity_type_default_module";

inline constexpr char graph_jit_disconnected_sample_output_module_id[] = "iv.test.graph_jit.state_context.disconnected_sample_output_module";

inline constexpr char graph_jit_reused_sample_arena_module_id[] = "iv.test.graph_jit.state_context.reused_sample_arena_module";

inline constexpr char graph_jit_sample_fanout_conversion_module_id[] = "iv.test.graph_jit.state_context.sample_fanout_conversion_module";

inline constexpr char graph_jit_sample_revision_module_id[] = "iv.test.graph_jit.state_context.sample_revision_module";

inline constexpr char graph_jit_persistent_sample_revision_module_id[] = "iv.test.graph_jit.state_context.persistent_sample_revision_module";

inline constexpr char graph_jit_composed_sample_revision_module_id[] = "iv.test.graph_jit.state_context.composed_sample_revision_module";

inline constexpr char graph_jit_stereo_conversion_module_id[] = "iv.test.graph_jit.state_context.stereo_conversion_module";

inline constexpr char graph_jit_stereo_history_conversion_module_id[] = "iv.test.graph_jit.state_context.stereo_history_conversion_module";

inline constexpr char graph_jit_history_fanout_module_id[] = "iv.test.graph_jit.state_context.history_fanout_module";

inline constexpr char graph_jit_persistent_history_module_id[] = "iv.test.graph_jit.state_context.persistent_history_module";

inline constexpr char graph_jit_constant_history_module_id[] =
    "iv.test.graph_jit.state_context.constant_history_module";

inline constexpr char graph_jit_constant_gain_feedback_module_id[] =
    "iv.test.graph_jit.state_context.constant_gain_feedback_module";

inline constexpr char graph_jit_immutable_history_module_id[] =
    "iv.test.graph_jit.state_context.immutable_history_module";

inline constexpr char graph_jit_distinct_immutable_history_module_id[] =
    "iv.test.graph_jit.state_context.distinct_immutable_history_module";

inline constexpr char graph_jit_large_history_rewire_module_id[] =
    "iv.test.graph_jit.state_context.large_history_rewire_module";

inline constexpr char graph_jit_latency_compensation_module_id[] = "iv.test.graph_jit.state_context.latency_compensation_module";

inline constexpr char graph_jit_latency_conversion_fanout_module_id[] = "iv.test.graph_jit.state_context.latency_conversion_fanout_module";

inline constexpr char graph_jit_composed_latency_module_id[] = "iv.test.graph_jit.state_context.composed_latency_module";

inline constexpr char graph_jit_composed_history_module_id[] = "iv.test.graph_jit.state_context.composed_history_module";

inline constexpr char graph_jit_projected_composition_module_id[] = "iv.test.graph_jit.state_context.projected_composition_module";

inline constexpr char graph_jit_fft_value_module_id[] = "iv.test.graph_jit.state_context.fft_value_module";

inline constexpr char graph_jit_fractional_index_value_module_id[] = "iv.test.graph_jit.state_context.fractional_index_value_module";

inline constexpr char graph_jit_persisted_fft_value_module_id[] = "iv.test.graph_jit.state_context.persisted_fft_value_module";

inline constexpr char graph_jit_persisted_non_project_rate_sample_module_id[] = "iv.test.graph_jit.state_context.persisted_non_project_rate_sample_module";

inline constexpr char graph_jit_direct_event_module_id[] = "iv.test.graph_jit.state_context.direct_event_module";

inline constexpr char graph_jit_transient_event_module_id[] = "iv.test.graph_jit.state_context.transient_event_module";

inline constexpr char graph_jit_converted_event_fanout_module_id[] = "iv.test.graph_jit.state_context.converted_event_fanout_module";

inline constexpr char graph_jit_retained_event_module_id[] = "iv.test.graph_jit.state_context.retained_event_module";

inline constexpr char graph_jit_persistent_event_ring_module_id[] = "iv.test.graph_jit.state_context.persistent_event_ring_module";

inline constexpr char graph_jit_retained_converted_event_fanout_module_id[] = "iv.test.graph_jit.state_context.retained_converted_event_fanout_module";

inline constexpr char graph_jit_sample_feedback_a_id[] = "iv.test.graph_jit.state_context.sample_feedback_a";

inline constexpr char graph_jit_sample_feedback_b_id[] = "iv.test.graph_jit.state_context.sample_feedback_b";

inline constexpr char graph_jit_multi_branch_sample_feedback_id[] = "iv.test.graph_jit.state_context.multi_branch_sample_feedback";

inline constexpr char graph_jit_temporal_sample_feedback_id[] = "iv.test.graph_jit.state_context.temporal_sample_feedback";

inline constexpr char graph_jit_projected_revising_sample_feedback_id[] = "iv.test.graph_jit.state_context.projected_revising_sample_feedback";

inline constexpr char graph_jit_converted_sample_feedback_id[] = "iv.test.graph_jit.state_context.converted_sample_feedback";

inline constexpr char graph_jit_event_feedback_a_id[] = "iv.test.graph_jit.state_context.event_feedback_a";

inline constexpr char graph_jit_retained_fan_in_event_feedback_a_id[] = "iv.test.graph_jit.state_context.retained_fan_in_event_feedback_a";

inline constexpr char graph_jit_latent_event_feedback_a_id[] = "iv.test.graph_jit.state_context.latent_event_feedback_a";

inline constexpr char graph_jit_persistent_latent_event_feedback_a_id[] = "iv.test.graph_jit.state_context.persistent_latent_event_feedback_a";

inline constexpr char graph_jit_persistent_latent_boundary_event_feedback_a_id[] = "iv.test.graph_jit.state_context.persistent_latent_boundary_event_feedback_a";

inline constexpr char graph_jit_event_feedback_b_id[] = "iv.test.graph_jit.state_context.event_feedback_b";

inline constexpr char graph_jit_boundary_event_feedback_a_id[] = "iv.test.graph_jit.state_context.boundary_event_feedback_a";

inline constexpr char graph_jit_boundary_event_feedback_b_id[] = "iv.test.graph_jit.state_context.boundary_event_feedback_b";

inline constexpr char graph_jit_retained_dual_event_feedback_a_id[] = "iv.test.graph_jit.state_context.retained_dual_event_feedback_a";

inline constexpr char graph_jit_dual_event_feedback_b_id[] = "iv.test.graph_jit.state_context.dual_event_feedback_b";

struct alignas(64) StatefulProbeStateMirror {
    std::uint64_t tick_calls = 0;
    std::uint64_t skip_calls = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
    std::uint64_t sample_rate = 0;
    std::uint64_t observed_state_extent = 0;
    std::uint64_t observed_background_extent = 0;
};

struct alignas(128) StatefulProbeBackgroundStateMirror {
    std::uint64_t tick_calls = 0;
    std::uint64_t skip_calls = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
    std::uint64_t observed_state_extent = 0;
    std::uint64_t observed_background_extent = 0;
};

struct NativeBatchFeedbackProbeStateMirror {
    std::uint64_t tick_batch_calls = 0;
    std::uint64_t last_lane_count = 0;
    std::uint64_t configured_id = 0;
    float last_input = 0.0f;
    float last_output = 0.0f;
};

struct ActivityProbeStateMirror {
    std::uint64_t tick_calls = 0;
    std::uint64_t skip_calls = 0;
};

struct SampleConsumerProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
    float first = 0.0f;
    float last = 0.0f;
    float sum = 0.0f;
};

struct MultiBranchSampleFeedbackStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t scc_feedback_latency = 0;
    std::array<std::uint64_t, 24> indices{};
    std::array<std::uint64_t, 24> block_sizes{};
    std::array<float, 24> first_fast{};
    std::array<float, 24> last_fast{};
    std::array<float, 24> first_slow{};
    std::array<float, 24> last_slow{};
    std::array<float, 24> first_initialized{};
    std::array<float, 24> last_initialized{};
    std::uint32_t marker = 0;
};

struct ConvertedSampleFeedbackStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t scc_feedback_latency = 0;
    std::array<std::uint64_t, 24> indices{};
    std::array<std::uint64_t, 24> block_sizes{};
    std::array<float, 24> first_left{};
    std::array<float, 24> first_right{};
    std::array<float, 24> last_left{};
    std::array<float, 24> last_right{};
    std::uint32_t marker = 0;
};

struct HistoryRampSourceStateMirror {
    std::uint64_t calls = 0;
    float previous_output = 0.0f;
    std::uint32_t marker = 0;
};

// State of CompensatedSequenceProbe and SampleSequenceProbe.
struct SequenceProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t read_latency = 0;
    std::int64_t first_index = 0;
    std::uint64_t recorded = 0;
    std::array<float, 64> values{};
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

struct RetainedEventConsumerProbeStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 3> indices{};
    std::array<std::uint64_t, 3> event_counts{};
    std::array<std::uint64_t, 3> first_times{};
    std::array<std::uint64_t, 3> second_times{};
    std::array<std::uint64_t, 3> last_times{};
};

struct PersistentEventFeedbackConsumerStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 4> indices{};
    std::array<std::uint64_t, 4> event_counts{};
    std::array<std::uint64_t, 4> first_times{};
    std::array<std::uint64_t, 4> last_times{};
    std::uint64_t marker = 0;
};

struct RetainedMidiConsumerProbeStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 4> indices{};
    std::array<std::uint64_t, 4> event_counts{};
    std::array<std::uint64_t, 4> midi_counts{};
    std::array<std::uint64_t, 4> first_times{};
    std::array<std::uint64_t, 4> last_times{};
};

std::shared_ptr<iv::NodeDefinitionsSnapshot const> make_graph_jit_snapshot(
    std::shared_ptr<iv::PackageRevision const> revision,
    std::uint64_t generation);

std::shared_ptr<iv::ConfiguredGraph const> configured_module_graph(
    iv::PackageRevision const& revision,
    std::string_view definition_id);

// Builds a module definition's graph the way the package loader builds its
// default graph (module/loader.cpp), but with construction arguments. Two graphs
// built from one module this way give the nodes they share the same virtual
// nodes, whatever the arguments.
std::shared_ptr<iv::ConfiguredGraph const> configured_module_graph_with_arguments(
    iv::PackageRevision const& revision,
    std::string_view definition_id,
    std::span<iv::details::ConfigurationArgument> arguments);

void expect_single_node_canonical_regions(
    iv::NodeLayout const& layout,
    iv::TockStateLayout const& tock_layout,
    std::size_t state_size,
    std::size_t background_state_size);

std::shared_ptr<iv::PackageRevision const> load_graph_jit_runtime_revision();

// `anonymous` with each of its concrete nodes in a virtual node of its own, as
// authored module code would place it, so that the nodes and what they own
// keep their identity across compilations of the result.
std::shared_ptr<iv::ConfiguredGraph const> with_node_identities(
    std::shared_ptr<iv::ConfiguredGraph const> const& anonymous);

std::shared_ptr<iv::ConfiguredGraph const> configured_multi_branch_sample_feedback_graph(
    iv::PackageRevision const& revision);

// Replaces the converted node's detached input connection, from the temporal
// node, by two: its left channel mixes the temporal and converted outputs,
// whose latencies differ, and its right channel reads the temporal output. Both
// keep the base connection's detach().
std::shared_ptr<iv::ConfiguredGraph const>
project_unequal_latency_sample_feedback(
    std::shared_ptr<iv::ConfiguredGraph const> const& base);

std::shared_ptr<iv::ConfiguredGraph const>
configured_unequal_latency_projected_sample_feedback_graph(
    iv::PackageRevision const& revision,
    std::size_t latency = 6,
    iv::Sample initial_value = iv::Sample{-0.25f});

std::shared_ptr<iv::ConfiguredGraph const> configured_merged_retained_event_graph(
    iv::PackageRevision const& revision,
    bool persistent_ring);

// One consumer (history 8) reads the fan-in of a trigger source with latency 8
// and one without latency.
std::shared_ptr<iv::ConfiguredGraph const>
configured_unequal_latency_event_fan_in_graph(
    iv::PackageRevision const& revision);

std::shared_ptr<iv::ConfiguredGraph const> configured_event_feedback_graph(
    iv::PackageRevision const& revision,
    std::string_view first_definition = graph_jit_event_feedback_a_id);

// With `observed`, an acyclic consumer (history 320), created after the
// loop's nodes so that their identities stay, also reads the staged fan-in.
std::shared_ptr<iv::ConfiguredGraph const>
configured_staged_event_fan_in_graph(
    iv::PackageRevision const& revision, bool observed = false);

std::size_t count_raw_regions(iv::NodeLayout const& layout);

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

// The State of the one node whose State has the mirror's size.
template<class Mirror>
Mirror* find_unique_state(
    iv::NodeLayout const& layout,
    iv::NodeStorage const& storage)
{
    Mirror* found = nullptr;
    for (std::size_t node = 0; node < layout.nodes.size(); ++node) {
        if (layout.nodes[node].state_size != sizeof(Mirror)) continue;
        EXPECT_EQ(found, nullptr) << "two nodes share the mirrored state size";
        found = static_cast<Mirror*>(storage.state_ptr(node));
    }
    return found;
}

// Runs the predecessor graph from position 0 to `cutover`, then activates the
// successor the way GraphExecutor activates a compiled cutover: initialize
// successor NodeStorage from the predecessor's, execute the cold transfers at
// the cutover position, run the transition root until every Sequential expiry
// cursor has advanced by its value count, then run the steady root until
// `end`. `check(compiled, storage, begin)` runs after every block.
template<class Check>
void run_across_graph_edit(
    iv::GraphJit& jit,
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> const& definitions,
    iv::ResourceContext const& resources,
    std::shared_ptr<iv::ConfiguredGraph const> const& predecessor_graph,
    std::shared_ptr<iv::ConfiguredGraph const> const& successor_graph,
    std::uint64_t generation,
    std::int64_t cutover,
    std::int64_t end,
    Check const& check)
{
    constexpr std::size_t block = 64;
    auto predecessor = jit.compile(iv::GraphJitCompileRequest{
        .project_generation = generation,
        .graph = predecessor_graph,
        .definitions = definitions,
    });
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);
    iv::GraphJitCompileRequest successor_request{
        .project_generation = generation + 1,
        .graph = successor_graph,
        .definitions = definitions,
    };
    auto steady_successor = jit.compile(successor_request);
    ASSERT_TRUE(steady_successor.succeeded())
        << (steady_successor.diagnostics.empty()
                ? ""
                : steady_successor.diagnostics.front().message);
    auto specialized = jit.compile_transition(
        iv::GraphJitTransitionCompileRequest{
            .successor = successor_request,
            .steady_successor = steady_successor.compiled_graph,
            .active_predecessor = predecessor.compiled_graph,
            .active_predecessor_phase =
                iv::CompiledGraphRealtimePhase::steady,
        });
    ASSERT_TRUE(specialized.succeeded())
        << (specialized.diagnostics.empty()
                ? ""
                : specialized.diagnostics.front().message);
    auto const& successor = *specialized.compiled_graph;
    ASSERT_TRUE(successor.cutover);

    auto predecessor_storage = predecessor.compiled_graph
        ->node_layout.create_storage(resources);
    predecessor_storage.initialize();
    for (std::int64_t begin = 0; begin < cutover;
         begin += static_cast<std::int64_t>(block)) {
        SCOPED_TRACE("predecessor block at " + std::to_string(begin));
        predecessor.compiled_graph->root_operations.tick_block(
            predecessor_storage.buffer().data(),
            &empty_tick_invocation,
            index(begin),
            block);
        check(*predecessor.compiled_graph, predecessor_storage, begin);
        if (::testing::Test::HasFatalFailure()) return;
    }

    auto successor_storage = successor.node_layout.create_storage(resources);
    successor_storage.initialize(&predecessor_storage);
    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        successor.cutover->state_transition,
        predecessor_storage.buffer(),
        successor_storage.buffer());
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared,
        predecessor_storage.buffer(),
        successor_storage.buffer(),
        index(cutover));

    auto read_cursor = [&](std::size_t offset) {
        std::uint64_t value = 0;
        std::memcpy(
            &value, successor_storage.buffer().data() + offset, sizeof(value));
        return value;
    };
    auto const& expiry =
        successor.cutover->state_transition.sample_expiry_cursors;
    auto const event_expiry =
        successor.cutover->state_transition.event_expiry_samples;
    std::vector<std::uint64_t> activation;
    for (auto const& cursor : expiry) {
        activation.push_back(read_cursor(cursor.node_storage_offset));
    }
    auto transition = !expiry.empty() || event_expiry != 0;
    std::uint64_t event_progress = 0;
    for (auto begin = cutover; begin < end;
         begin += static_cast<std::int64_t>(block)) {
        SCOPED_TRACE(
            std::string{transition ? "transition" : "steady"}
            + " block at " + std::to_string(begin));
        auto const operation = transition
            ? successor.root_operations.transition_tick_block
            : successor.root_operations.tick_block;
        ASSERT_NE(operation, nullptr);
        operation(
            successor_storage.buffer().data(),
            &empty_tick_invocation,
            index(begin),
            block);
        check(successor, successor_storage, begin);
        if (::testing::Test::HasFatalFailure()) return;
        if (!transition) continue;
        event_progress += block;
        auto samples_complete = true;
        for (std::size_t cursor = 0; cursor < expiry.size(); ++cursor) {
            samples_complete = samples_complete
                && read_cursor(expiry[cursor].node_storage_offset)
                        - activation[cursor]
                    >= expiry[cursor].value_count;
        }
        transition = !(samples_complete && event_progress >= event_expiry);
    }
    EXPECT_FALSE(transition)
        << "the transition realization was still active at the end of the run";
}

inline constexpr std::int64_t sequence_cutover = 16 * 64;

float ramp_value(std::int64_t position);

// The stable identities of a graph's concrete nodes, sorted. An edit test uses
// them to check that the nodes it expects to survive keep their identity.
std::vector<iv::graph_jit::StableConcreteNodeId> stable_node_identities(
    iv::ConfiguredGraph const& graph);

// Compiles `graph` and expects the realization of its one Sequential input
// record with `history` values of history and `in_flight` in-flight values to
// have `access`. A reproduction test uses it to show which realization it
// reaches.
void expect_sequential_input_access(
    iv::GraphJit& jit,
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> const& definitions,
    std::shared_ptr<iv::ConfiguredGraph const> const& graph,
    std::size_t history,
    std::size_t in_flight,
    iv::graph_jit::SampleRealtimePortStateAccess access,
    std::uint64_t generation);

} // namespace iv::test::graph_jit_tests
