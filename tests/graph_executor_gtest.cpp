#include <intravenous/runtime/background_coverage_propagation.h>
#include <intravenous/graph_jit/sequential_coverage.h>
#include "event_source_test_support.h"
#include <intravenous/runtime/graph_executor.h>

#include <intravenous/node/layout.h>
#include <intravenous/node/lifecycle.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

iv::GlobalIndex index(std::int64_t value)
{
    return iv::GlobalIndex::from_signed(value);
}

iv::GlobalIndexRange range(std::int64_t begin, std::int64_t end)
{
    return iv::GlobalIndexRange::from_begin_and_end(index(begin), index(end));
}

iv::IndexCoverage index_coverage(
    std::initializer_list<std::pair<std::int64_t, std::int64_t>> ranges)
{
    iv::IndexCoverage result;
    for (auto const [begin, end] : ranges) result.include(range(begin, end));
    return result;
}

std::vector<iv::GlobalIndexRange> coverage_regions(
    iv::IndexCoverage const& coverage)
{
    return {coverage.begin(), coverage.end()};
}

bool combine_sample_values(
    iv::graph_jit::BackgroundPortIndex,
    std::byte* destination,
    std::byte const* source,
    std::size_t byte_count)
{
    if (byte_count % sizeof(iv::Sample) != 0) return false;
    auto destination_values = std::span{
        reinterpret_cast<iv::Sample*>(destination),
        byte_count / sizeof(iv::Sample)};
    auto source_values = std::span{
        reinterpret_cast<iv::Sample const*>(source),
        byte_count / sizeof(iv::Sample)};
    for (std::size_t index = 0; index < destination_values.size(); ++index) {
        destination_values[index] = iv::Sample{
            destination_values[index].value + source_values[index].value};
    }
    return true;
}

struct TickObservation {
    std::size_t calls = 0;
    iv::GlobalIndex sample_index{};
    std::size_t block_size = 0;
    std::byte* storage = nullptr;
    iv::graph_jit::TickInvocationCall const* invocation = nullptr;
};

TickObservation first_tick;
TickObservation second_tick;
std::size_t first_raw_offset = 0;
std::size_t second_raw_offset = 0;
std::size_t third_raw_offset = 0;
unsigned migrated_raw_value = 0;
iv::IndexCoverage persisted_probe_coverage{};
std::size_t persisted_probe_evaluate_calls = 0;
bool throw_persisted_probe_evaluate = false;
bool omit_last_persisted_probe_sample = false;
std::size_t replay_probe_evaluate_calls = 0;
std::vector<iv::GlobalIndexRange> replay_probe_regions{};
iv::PersistedDataStore* competing_persisted_store = nullptr;
bool publish_competing_persisted_snapshot = false;
std::optional<iv::PersistedDataVersion> competing_persisted_version{};
std::vector<iv::GlobalIndexRange> propagated_region_shape{};
std::array<std::vector<iv::GlobalIndexRange>, 2> sorted_source_region_shapes{};

struct BackgroundPropagationObservation {
    std::size_t source_forward_calls[2]{};
    std::size_t sink_forward_calls = 0;
    std::size_t sink_reverse_calls = 0;
    iv::IndexCoverage sink_input_coverage{};
    iv::IndexCoverage sink_input_changed{};
    iv::IndexCoverage sink_output_required{};
    bool throw_from_sink = false;
    bool throw_from_reverse = false;
};

BackgroundPropagationObservation background_observation;

void observe_first(
    iv::graph_jit::RealtimeRootPlan const*,
    std::byte* storage,
    iv::graph_jit::TickInvocationCall const* invocation,
    iv::GlobalIndex sample_index,
    std::size_t block_size)
{
    first_tick = TickObservation{
        .calls = first_tick.calls + 1,
        .sample_index = sample_index,
        .block_size = block_size,
        .storage = storage,
        .invocation = invocation,
    };
}

void observe_second(
    iv::graph_jit::RealtimeRootPlan const*,
    std::byte* storage,
    iv::graph_jit::TickInvocationCall const* invocation,
    iv::GlobalIndex sample_index,
    std::size_t block_size)
{
    second_tick = TickObservation{
        .calls = second_tick.calls + 1,
        .sample_index = sample_index,
        .block_size = block_size,
        .storage = storage,
        .invocation = invocation,
    };
}

void observe_first_and_advance_transition_cursor(
    iv::graph_jit::RealtimeRootPlan const*,
    std::byte* storage,
    iv::graph_jit::TickInvocationCall const* invocation,
    iv::GlobalIndex sample_index,
    std::size_t block_size)
{
    observe_first(nullptr, storage, invocation, sample_index, block_size);
    std::uint64_t cursor = 0;
    std::memcpy(&cursor, storage, sizeof(cursor));
    cursor += block_size;
    std::memcpy(storage, &cursor, sizeof(cursor));
}

void initialize_zero_raw_region(
    std::span<std::byte> storage,
    std::span<std::byte const>)
{
    std::ranges::fill(storage, std::byte{});
}

void write_raw_state(
    iv::graph_jit::RealtimeRootPlan const*,
    std::byte* storage,
    iv::graph_jit::TickInvocationCall const*,
    iv::GlobalIndex,
    std::size_t)
{
    storage[first_raw_offset] = std::byte{0x5a};
}

void observe_raw_state(
    iv::graph_jit::RealtimeRootPlan const*,
    std::byte* storage,
    iv::graph_jit::TickInvocationCall const*,
    iv::GlobalIndex,
    std::size_t)
{
    migrated_raw_value = std::to_integer<unsigned>(storage[second_raw_offset]);
}

void replace_second_raw_state(
    iv::graph_jit::RealtimeRootPlan const*,
    std::byte* storage,
    iv::graph_jit::TickInvocationCall const*,
    iv::GlobalIndex,
    std::size_t)
{
    migrated_raw_value = std::to_integer<unsigned>(storage[second_raw_offset]);
    storage[second_raw_offset] = std::byte{0x6b};
}

void observe_third_raw_state(
    iv::graph_jit::RealtimeRootPlan const*,
    std::byte* storage,
    iv::graph_jit::TickInvocationCall const*,
    iv::GlobalIndex,
    std::size_t)
{
    migrated_raw_value = std::to_integer<unsigned>(storage[third_raw_offset]);
}

void propagate_background_forward(
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    auto nodes = static_cast<
        std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes);
    for (std::size_t node = 0; node < nodes.size(); ++node) {
        auto& frame = nodes[node];
        if (!iv::graph_jit::has_activity(
                frame.activity,
                iv::graph_jit::BackgroundNodeActivity::forward)) {
            continue;
        }
        auto outputs = static_cast<std::span<iv::OutputCoverageChange>>(
            frame.forward.outputs);
        if (node < 2) {
            ++background_observation.source_forward_calls[node];
            auto const coverage = node == 0
                ? index_coverage({{10, 13}})
                : index_coverage({{20, 22}});
            auto const regions = coverage_regions(coverage);
            outputs[0].publish_regions(regions);
            outputs[0].change(coverage);
            continue;
        }

        ++background_observation.sink_forward_calls;
        if (background_observation.throw_from_sink) {
            throw std::runtime_error("background propagation probe failure");
        }
        auto const inputs = static_cast<
            std::span<iv::InputCoverageChange const>>(frame.forward.inputs);
        background_observation.sink_input_coverage = inputs[0].coverage();
        background_observation.sink_input_changed = inputs[0].changed();
        outputs[0].publish_regions(inputs[0].regions());
        outputs[0].change(inputs[0].changed());
    }
}

void propagate_background_reverse(
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    auto nodes = static_cast<
        std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes);
    auto& sink = nodes[2];
    if (!iv::graph_jit::has_activity(
            sink.activity,
            iv::graph_jit::BackgroundNodeActivity::reverse)) {
        return;
    }
    ++background_observation.sink_reverse_calls;
    if (background_observation.throw_from_reverse) {
        throw std::runtime_error("background reverse propagation probe failure");
    }
    auto const outputs = static_cast<
        std::span<iv::OutputCoverageRequirement const>>(sink.reverse.outputs);
    auto inputs = static_cast<std::span<iv::InputCoverageRequirement>>(
        sink.reverse.inputs);
    background_observation.sink_output_required = outputs[0].required();
    inputs[0].require(outputs[0].required());
}

void no_op_background_evaluate(
    iv::graph_jit::BackgroundEvaluationCall*)
{}

std::atomic<bool> initial_playback_started{false};
std::atomic<bool> allow_initial_playback{false};
void wait_for_initial_playback(iv::graph_jit::BackgroundEvaluationCall*)
{
    initial_playback_started.store(true, std::memory_order_release);
    while (!allow_initial_playback.load(std::memory_order_acquire))
        allow_initial_playback.wait(false, std::memory_order_acquire);
}

void fail_initial_playback(iv::graph_jit::BackgroundEvaluationCall* call)
{
    wait_for_initial_playback(call);
    throw iv::TerminalBackgroundPreparationError("executable cannot prepare");
}

void propagate_region_shape(
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    for (auto& node :
         static_cast<std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes)) {
        if (!iv::graph_jit::has_activity(
                node.activity,
                iv::graph_jit::BackgroundNodeActivity::forward)) {
            continue;
        }
        auto outputs = static_cast<std::span<iv::OutputCoverageChange>>(
            node.forward.outputs);
        ASSERT_EQ(outputs.size(), 1u);
        outputs[0].publish_regions(propagated_region_shape);
    }
}

void propagate_positional_continuous_shape(
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    for (auto& node :
         static_cast<std::span<iv::graph_jit::BackgroundNodeCall>>(
             batch->nodes)) {
        if (!iv::graph_jit::has_activity(
                node.activity,
                iv::graph_jit::BackgroundNodeActivity::forward)) {
            continue;
        }
        auto outputs = static_cast<std::span<iv::OutputCoverageChange>>(
            node.forward.outputs);
        ASSERT_EQ(outputs.size(), 1u);
        outputs[0].publish_coverage(index_coverage({{10, 12}}));
    }
}

void propagate_sorted_source_regions(
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    auto nodes = static_cast<
        std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes);
    for (std::size_t node = 0; node < nodes.size(); ++node) {
        if (!iv::graph_jit::has_activity(
                nodes[node].activity,
                iv::graph_jit::BackgroundNodeActivity::forward)) {
            continue;
        }
        auto outputs = static_cast<std::span<iv::OutputCoverageChange>>(
            nodes[node].forward.outputs);
        if (node < sorted_source_region_shapes.size()) {
            ASSERT_EQ(outputs.size(), 1u);
            outputs[0].publish_regions(sorted_source_region_shapes[node]);
        }
    }
}

void propagate_sorted_source_event_regions(
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    auto nodes = static_cast<
        std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes);
    for (std::size_t node = 0; node < nodes.size(); ++node) {
        if (!iv::graph_jit::has_activity(
                nodes[node].activity,
                iv::graph_jit::BackgroundNodeActivity::forward)) {
            continue;
        }
        auto outputs = static_cast<std::span<iv::OutputCoverageChange>>(
            nodes[node].forward.event_outputs);
        if (node < sorted_source_region_shapes.size()) {
            ASSERT_EQ(outputs.size(), 1u);
            outputs[0].publish_regions(sorted_source_region_shapes[node]);
        }
    }
}

void propagate_persisted_probe_forward(
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    auto nodes = static_cast<
        std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes);
    for (auto& node : nodes) {
        if (!iv::graph_jit::has_activity(
                node.activity,
                iv::graph_jit::BackgroundNodeActivity::forward)) {
            continue;
        }
        auto outputs = static_cast<std::span<iv::OutputCoverageChange>>(
            node.forward.outputs);
        ASSERT_EQ(outputs.size(), 1u);
        auto const regions = coverage_regions(persisted_probe_coverage);
        outputs[0].publish_regions(regions);
    }
}

void evaluate_persisted_probe(
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    auto nodes = static_cast<
        std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes);
    for (auto& node : nodes) {
        if (!iv::graph_jit::has_activity(
                node.activity,
                iv::graph_jit::BackgroundNodeActivity::evaluate)) {
            continue;
        }
        ++persisted_probe_evaluate_calls;
        ASSERT_NE(node.prepare_operations, nullptr);
        ASSERT_NE(node.finalize_operations, nullptr);
        node.prepare_operations(node.operation_frame);

        auto outputs = static_cast<std::span<iv::TockContinuousOutputPort>>(
            node.tock.outputs);
        ASSERT_EQ(outputs.size(), 1u);
        for (auto run : outputs[0].prepared_runs<iv::Sample>()) {
            for (std::size_t slot = 0; slot < run.size(); ++slot) {
                auto const position = run.position(slot);
                if (omit_last_persisted_probe_sample
                    && slot + 1 == run.size()) {
                    continue;
                }
                run.value(slot) = iv::Sample{
                    100.0f + static_cast<float>(position.signed_value())};
            }
        }
        if (throw_persisted_probe_evaluate) {
            throw std::runtime_error("persisted background probe failure");
        }
        node.finalize_operations(node.operation_frame);
        if (publish_competing_persisted_snapshot) {
            auto candidate = competing_persisted_store->begin_candidate(99);
            auto const version = candidate.target_version();
            if (competing_persisted_store->publish(std::move(candidate)) !=
                iv::PersistedDataPublishResult::published) {
                throw std::runtime_error(
                    "competing persisted background probe publish failed");
            }
            competing_persisted_version = version;
            publish_competing_persisted_snapshot = false;
        }
    }
}

iv::graph_jit::BackgroundEvaluationPlan probe_tock_plan(
    iv::OutputRealization realization,
    iv::graph_jit::PortStorageKind storage_kind)
{
    using namespace iv::graph_jit;
    BackgroundEvaluationPlan plan;
    plan.nodes = {{
        .bundle = 0,
        .tock_state_node = 0,
        .authored_tock_execution = true,
        .outputs = {0},
        .accumulators = NodeAccumulatorRanges{
            .output_change_begin = 0,
            .output_change_count = 1,
            .output_requirement_begin = 0,
            .output_requirement_count = 1,
        },
    }};
    plan.ports = {{
        .node = 0,
        .configured_port = {0, iv::PortKind::sample, 0},
        .kind = iv::PortKind::sample,
        .direction = PortDirection::output,
        .output_production = iv::OutputProductionKind::coverage,
        .authored_tock_output = true,
        .realization = realization,
        .sample_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .accumulators = PortAccumulatorIndices{
            .output_change = 0,
            .output_requirement = 0,
        },
    }};
    plan.storage.ports = {{
        .kind = iv::PortKind::sample,
        .storage = storage_kind,
        .source_port = iv::NodeBundlePortId{0, iv::PortKind::sample, 0},
        .output_port = 0,
        .sample_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .sample_channels = {0},
    }};
    plan.runtime.bindings = {{
        .node = 0,
        .port = 0,
        .kind = iv::PortKind::sample,
        .direction = PortDirection::output,
        .storage = {0},
    }};
    plan.runtime.port_bindings = {BackgroundBindingSlot{0}};
    plan.runtime.node_operations.resize(1);
    plan.runtime.node_replay_invocations.resize(1);
    plan.accumulators = CoverageAccumulatorCounts{
        .output_change_count = 1,
        .output_requirement_count = 1,
    };
    return plan;
}

iv::CompiledGraph persisted_tock_graph()
{
    iv::CompiledGraph graph;
    graph.project_generation = 1;
    graph.specialization.sample_rate = 48000;
    graph.specialization.block_size = 4;
    graph.tock_state_layout.nodes.emplace_back();
    graph.background_evaluation_plan = probe_tock_plan(
        iv::OutputRealization::buffered,
        iv::graph_jit::PortStorageKind::persisted_data);
    graph.background_operations = {
        .propagate_forward = &propagate_persisted_probe_forward,
        .propagate_reverse = &no_op_background_evaluate,
        .evaluate = &evaluate_persisted_probe,
    };
    return graph;
}

iv::CompiledGraph ephemeral_tock_graph()
{
    auto graph = persisted_tock_graph();
    graph.background_evaluation_plan = probe_tock_plan(
        iv::OutputRealization::computed,
        iv::graph_jit::PortStorageKind::background);
    return graph;
}

void propagate_replay_probe_forward(
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    for (auto& node :
         static_cast<std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes)) {
        if (iv::graph_jit::has_activity(
                node.activity,
                iv::graph_jit::BackgroundNodeActivity::forward)) {
            node.replay_forward(node.replay_context, node.forward);
        }
    }
}

void propagate_replay_probe_reverse(
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    for (auto& node :
         static_cast<std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes)) {
        if (iv::graph_jit::has_activity(
                node.activity,
                iv::graph_jit::BackgroundNodeActivity::reverse)) {
            node.replay_reverse(node.replay_context, node.reverse);
        }
    }
}

void evaluate_replay_probe(
    iv::graph_jit::BackgroundEvaluationCall* batch)
{
    for (auto& node :
         static_cast<std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes)) {
        if (!iv::graph_jit::has_activity(
                node.activity,
                iv::graph_jit::BackgroundNodeActivity::evaluate)) {
            continue;
        }
        ++replay_probe_evaluate_calls;
        auto const replay_regions = static_cast<
            std::span<iv::GlobalIndexRange const>>(node.replay_regions);
        replay_probe_regions.assign(
            replay_regions.begin(), replay_regions.end());
        node.prepare_operations(node.operation_frame);
        auto outputs = static_cast<
            std::span<iv::ReflectedContinuousOutputPortBinding const>>(
            node.replay.continuous_output_bindings);
        for (auto const region :
             static_cast<std::span<iv::GlobalIndexRange const>>(
                 node.replay_regions)) {
            for (iv::GlobalIndex::extent_type offset = 0;
                 offset < region.extent;
                 ++offset) {
                auto const position = region.begin.advanced_by(offset);
                for (std::size_t output = 0; output < outputs.size(); ++output) {
                    auto const& storage = outputs[output].storage;
                    auto const& channel = storage.channels[0];
                    auto const index = position.bits() - channel.value_delay;
                    auto const slot = static_cast<std::size_t>(index)
                        & (channel.value_capacity - 1);
                    *reinterpret_cast<iv::Sample*>(
                        channel.storage
                            + slot * channel.value_stride_bytes) = iv::Sample{
                                100.0f * static_cast<float>(output)
                                + static_cast<float>(position.signed_value())};
                }
            }
        }
        node.finalize_operations(node.operation_frame);
    }
}

iv::CompiledGraph replay_probe_graph()
{
    using namespace iv::graph_jit;
    iv::CompiledGraph graph;
    graph.project_generation = 1;
    graph.specialization.sample_rate = 48000;
    graph.specialization.block_size = 4;
    auto& plan = graph.background_evaluation_plan;
    plan.nodes = {{
        .bundle = 0,
        .replays_tick = true,
        .uses_replay_forward_coverage = true,
        .uses_replay_reverse_coverage = true,
        .replay_mode = iv::TickReplayMode::full,
        .uses_imported_tick_block_for_replay = true,
        .replay_maximum_block_size = 4,
        .outputs = {0, 1},
        .accumulators = NodeAccumulatorRanges{
            .output_change_begin = 0,
            .output_change_count = 2,
            .output_requirement_begin = 0,
            .output_requirement_count = 2,
        },
    }};
    plan.ports = {
        {
            .node = 0,
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .replayed_tick_output = true,
            .realization = iv::OutputRealization::computed,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .accumulators = PortAccumulatorIndices{
                .output_change = 0,
                .output_requirement = 0,
            },
        },
        {
            .node = 0,
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .replayed_tick_output = true,
            .realization = iv::OutputRealization::computed,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .accumulators = PortAccumulatorIndices{
                .output_change = 1,
                .output_requirement = 1,
            },
        },
    };
    plan.storage.ports = {
        {
            .kind = iv::PortKind::sample,
            .storage = PortStorageKind::background,
            .output_port = 0,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .sample_channels = {0},
        },
        {
            .kind = iv::PortKind::sample,
            .storage = PortStorageKind::background,
            .output_port = 1,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .sample_channels = {0},
        },
    };
    plan.runtime.bindings = {
        {.node = 0,
         .port = 0,
         .kind = iv::PortKind::sample,
         .direction = PortDirection::output,
         .storage = {0}},
        {.node = 0,
         .port = 1,
         .kind = iv::PortKind::sample,
         .direction = PortDirection::output,
         .storage = {1}},
    };
    plan.runtime.port_bindings = {0, 1};
    plan.runtime.node_operations.resize(1);
    plan.runtime.replay_invocations = {{
        .node = 0,
        .maximum_block_size = 4,
        .output_bindings = {0, 1},
    }};
    plan.runtime.node_replay_invocations = {0};
    plan.accumulators = {
        .output_change_count = 2,
        .output_requirement_count = 2,
    };
    graph.background_operations = {
        .propagate_forward = &propagate_replay_probe_forward,
        .propagate_reverse = &propagate_replay_probe_reverse,
        .evaluate = &evaluate_replay_probe,
    };
    return graph;
}

iv::graph_jit::BackgroundEvaluationPlan background_fanin_plan()
{
    using namespace iv::graph_jit;
    BackgroundEvaluationPlan plan;
    plan.nodes = {
        BackgroundNodePlan{
            .bundle = 0,
            .authored_tock_execution = true,
            .outputs = {0},
            .accumulators = NodeAccumulatorRanges{
                .output_change_begin = 0,
                .output_change_count = 1,
                .output_requirement_begin = 0,
                .output_requirement_count = 1,
            },
        },
        BackgroundNodePlan{
            .bundle = 1,
            .authored_tock_execution = true,
            .outputs = {1},
            .accumulators = NodeAccumulatorRanges{
                .output_change_begin = 1,
                .output_change_count = 1,
                .output_requirement_begin = 1,
                .output_requirement_count = 1,
            },
        },
        BackgroundNodePlan{
            .bundle = 2,
            .authored_tock_execution = true,
            .inputs = {2},
            .outputs = {3},
            .accumulators = NodeAccumulatorRanges{
                .input_change_begin = 0,
                .input_change_count = 1,
                .output_change_begin = 2,
                .output_change_count = 1,
                .output_requirement_begin = 2,
                .output_requirement_count = 1,
                .input_requirement_begin = 0,
                .input_requirement_count = 1,
            },
        },
    };
    plan.ports = {
        BackgroundPortPlan{
            .node = 0,
            .configured_port = {0, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .output_production = iv::OutputProductionKind::coverage,
            .authored_tock_output = true,
            .realization = iv::OutputRealization::computed,
            .outgoing_connections = {0},
            .accumulators = PortAccumulatorIndices{
                .output_change = 0,
                .output_requirement = 0,
            },
        },
        BackgroundPortPlan{
            .node = 1,
            .configured_port = {1, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .output_production = iv::OutputProductionKind::coverage,
            .authored_tock_output = true,
            .realization = iv::OutputRealization::computed,
            .outgoing_connections = {1},
            .accumulators = PortAccumulatorIndices{
                .output_change = 1,
                .output_requirement = 1,
            },
        },
        BackgroundPortPlan{
            .node = 2,
            .configured_port = {2, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::input,
            .input_access = iv::InputAccessKind::coverage,
            .random_access_input = true,
            .incoming_connections = {0, 1},
            .accumulators = PortAccumulatorIndices{
                .input_change = 0,
                .input_requirement = 0,
            },
        },
        BackgroundPortPlan{
            .node = 2,
            .configured_port = {2, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .output_production = iv::OutputProductionKind::coverage,
            .authored_tock_output = true,
            .realization = iv::OutputRealization::computed,
            .accumulators = PortAccumulatorIndices{
                .output_change = 2,
                .output_requirement = 2,
            },
        },
    };
    plan.connections = {
        BackgroundConnectionPlan{
            .kind = iv::PortKind::sample,
            .source_coverage_ports = {0},
            .target_coverage_ports = {2},
        },
        BackgroundConnectionPlan{
            .kind = iv::PortKind::sample,
            .source_coverage_ports = {1},
            .target_coverage_ports = {2},
        },
    };
    plan.accumulators = CoverageAccumulatorCounts{
        .input_change_count = 1,
        .output_change_count = 3,
        .output_requirement_count = 3,
        .input_requirement_count = 1,
    };
    return plan;
}

iv::graph_jit::BackgroundEvaluationPlan background_mixed_output_plan()
{
    using namespace iv::graph_jit;
    BackgroundEvaluationPlan plan;
    plan.nodes = {
        BackgroundNodePlan{
            .bundle = 0,
            .authored_tock_execution = true,
            .outputs = {0, 1},
            .accumulators = NodeAccumulatorRanges{
                .output_change_begin = 0,
                .output_change_count = 2,
                .output_requirement_begin = 0,
                .output_requirement_count = 2,
            },
        },
    };
    plan.ports = {
        BackgroundPortPlan{
            .node = 0,
            .configured_port = {0, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .persisted_tick_output = true,
            .realization = iv::OutputRealization::buffered,
            .accumulators = PortAccumulatorIndices{
                .output_change = 0,
                .output_requirement = 0,
            },
        },
        BackgroundPortPlan{
            .node = 0,
            .configured_port = {0, iv::PortKind::sample, 1},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .output_production = iv::OutputProductionKind::coverage,
            .authored_tock_output = true,
            .realization = iv::OutputRealization::computed,
            .accumulators = PortAccumulatorIndices{
                .output_change = 1,
                .output_requirement = 1,
            },
        },
    };
    plan.accumulators = CoverageAccumulatorCounts{
        .output_change_count = 2,
        .output_requirement_count = 2,
    };
    return plan;
}

std::shared_ptr<iv::CompiledGraph const> compiled_graph(
    std::uint64_t generation,
    iv::CompiledGraphBlockFunction tick,
    iv::NodeLayout layout = iv::NodeLayoutBuilder(64).build(),
    iv::CompiledGraphBackgroundFunction background = nullptr)
{
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = generation;
    graph->specialization.sample_rate = 48000;
    graph->specialization.block_size = 64;
    graph->node_layout = std::move(layout);
    graph->root_operations.tick_block = {.function = tick};
    graph->root_operations.combine_sequential_input = &combine_sample_values;
    if (background) {
        graph->background_operations = {
            .propagate_forward = &no_op_background_evaluate,
            .propagate_reverse = &no_op_background_evaluate,
            .evaluate = background,
        };
    }
    return graph;
}

std::shared_ptr<iv::CompiledGraph const> transition_graph(
    std::uint64_t generation,
    std::shared_ptr<iv::CompiledGraph const> const& predecessor,
    iv::CompiledGraphRealtimePhase predecessor_phase,
    std::size_t expiry_values,
    iv::CompiledGraphBlockFunction transition_tick,
    iv::CompiledGraphBlockFunction steady_tick,
    std::size_t event_expiry_samples = 0)
{
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = generation;
    graph->specialization.sample_rate = 48000;
    graph->specialization.block_size = 64;
    iv::NodeLayoutBuilder layout_builder(64);
    auto const cursor_region = layout_builder.declare_raw_region(
        sizeof(std::uint64_t),
        alignof(std::uint64_t),
        {},
        initialize_zero_raw_region);
    graph->node_layout = std::move(layout_builder).build();
    graph->root_operations = {
        .tick_block = {.function = steady_tick},
        .transition_tick_block = {.function = transition_tick},
        .combine_sequential_input = &combine_sample_values,
    };
    graph->cutover.emplace(iv::CompiledGraphCutover{
        .predecessor = predecessor,
        .predecessor_phase = predecessor_phase,
        .state_transition = {
            .event_expiry_samples = event_expiry_samples,
        },
    });
    if (expiry_values != 0) {
        graph->cutover->state_transition.sample_expiry_cursors.push_back({
            .node_storage_offset = graph->node_layout
                .regions[cursor_region.index].storage_offset,
            .value_count = expiry_values,
        });
    }
    return graph;
}

iv::GraphExecutorStageResult stage_graph(
    iv::GraphExecutor& executor,
    std::shared_ptr<iv::CompiledGraph const> const& graph)
{
    if (graph->cutover || !executor.accepted_graph()) {
        return executor.stage(graph);
    }
    auto executable = std::make_shared<iv::CompiledGraph>(*graph);
    auto transition = iv::graph_jit::plan_realtime_port_state_transition(
        executor.accepted_graph()->realtime_port_state_requirements,
        executor.accepted_graph()->realtime_port_state_realizations,
        executable->realtime_port_state_requirements,
        executable->realtime_port_state_realizations);
    if (!transition) {
        throw std::runtime_error(transition.error());
    }
    executable->cutover.emplace(iv::CompiledGraphCutover{
        .predecessor = executor.accepted_graph(),
        .predecessor_phase = iv::CompiledGraphRealtimePhase::steady,
        .state_transition = std::move(*transition),
    });
    return executor.stage(std::move(executable));
}

iv::NodeLayout persistent_raw_layout(std::size_t& offset)
{
    iv::NodeLayoutBuilder builder(64);
    auto const region = builder.declare_raw_region(
        1, alignof(std::byte), "graph-executor-test-state");
    auto layout = std::move(builder).build();
    offset = layout.regions[region.index].storage_offset;
    return layout;
}

iv::ManualControlId manual_control_id()
{
    return {
        .node = {.instance_id = "instance:manual"},
        .port_name = "value",
    };
}

iv::NodeLayout manual_control_layout(std::size_t& offset)
{
    iv::NodeLayoutBuilder builder(64);
    auto const region = builder.declare_raw_region(
        sizeof(iv::ManualControlRuntimeState),
        alignof(iv::ManualControlRuntimeState),
        "graph-executor-test-manual-control",
        initialize_zero_raw_region);
    auto layout = std::move(builder).build();
    offset = layout.regions[region.index].storage_offset;
    return layout;
}

struct BackgroundLifecycleProbe {
    static inline std::thread::id control_thread{};
    static inline std::atomic<int> initialized{0}, moved{0}, released{0}, live{0};
    static inline std::atomic<bool> wrong_thread{false};
    static void check_owner()
    { if (std::this_thread::get_id() == control_thread) wrong_thread.store(true); }
    struct State {
        std::unique_ptr<int> cache;
        State() { check_owner(); ++live; }
        ~State() { check_owner(); --live; }
    };
    static constexpr auto outputs()
    { return std::tuple{iv::output<iv::Sample>("out", iv::OutputProduction::coverage)}; }
    void tock_coverage(iv::TockCoverageContext<BackgroundLifecycleProbe>&) const {}
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<BackgroundLifecycleProbe>&) const {}
    void initialize(iv::InitializationContext<BackgroundLifecycleProbe> const& ctx) const
    {
        check_owner();
        ctx.state().cache = std::make_unique<int>(17);
        ++initialized;
    }
    void move(iv::MoveContext<BackgroundLifecycleProbe> const& ctx) const
    {
        check_owner();
        ctx.state().cache = std::move(ctx.previous_state().cache);
        EXPECT_NE(ctx.state().cache, nullptr);
        if (ctx.state().cache) {
            EXPECT_EQ(*ctx.state().cache, 17);
        }
        ++moved;
    }
    void release(iv::ReleaseContext<BackgroundLifecycleProbe> const&) const
    { check_owner(); ++released; }
};

std::shared_ptr<iv::CompiledGraph> background_lifecycle_graph(std::uint64_t generation)
{
    auto graph = std::const_pointer_cast<iv::CompiledGraph>(compiled_graph(generation, &observe_first));
    iv::NodeLayoutBuilder builder(64);
    iv::do_declare(BackgroundLifecycleProbe{}, builder, {});
    builder.set_node_migration_identity(0, "background-lifecycle-probe");
    graph->tock_state_layout = builder.build_tock_state_layout();
    graph->node_layout = std::move(builder).build();
    return graph;
}

class GraphExecutorFixture : public testing::Test {
protected:
    iv::ResourceContext resources_{};

    iv::TockStateStorage initialize_tock_states(
        iv::CompiledGraph const& graph)
    {
        auto storage = graph.tock_state_layout.create_storage(resources_);
        storage.initialize();
        return storage;
    }

    void SetUp() override
    {
        first_tick = {};
        second_tick = {};
        first_raw_offset = 0;
        second_raw_offset = 0;
        third_raw_offset = 0;
        migrated_raw_value = 0;
        persisted_probe_coverage = index_coverage({{0, 8}});
        persisted_probe_evaluate_calls = 0;
        throw_persisted_probe_evaluate = false;
        omit_last_persisted_probe_sample = false;
        replay_probe_evaluate_calls = 0;
        replay_probe_regions.clear();
        competing_persisted_store = nullptr;
        publish_competing_persisted_snapshot = false;
        competing_persisted_version.reset();
    }
};

class BackgroundCoveragePropagationFixture : public testing::Test {
protected:
    void SetUp() override { background_observation = {}; }
};

TEST(GraphExecutorPersistedStorageCompatibility,
     IncludesProductionRolesAndContinuousGeometry)
{
    auto sample_plan = [] {
        iv::graph_jit::BackgroundEvaluationPlan plan;
        plan.ports = {{
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::output,
            .output_production = iv::OutputProductionKind::coverage,
            .authored_tock_output = true,
            .realization = iv::OutputRealization::buffered,
            .sample_layout = iv::mono_planar_channel_layout,
            .continuous_geometry =
                *iv::continuous_value_storage_geometry(
                    iv::continuous_value_type_id_v<iv::Sample>,
                    std::nullopt),
        }};
        plan.storage.ports = {{
            .kind = iv::PortKind::sample,
            .storage = iv::graph_jit::PortStorageKind::persisted_data,
            .output_port = 0,
            .sample_layout = iv::mono_planar_channel_layout,
            .continuous_geometry = plan.ports[0].continuous_geometry,
            .sample_channels = {0},
        }};
        return plan;
    };

    auto const previous = sample_plan();
    auto current = previous;
    EXPECT_TRUE(iv::graph_jit::persisted_storage_is_compatible(
        current, 0, previous, 0));

    auto computed = previous;
    computed.ports[0].realization = iv::OutputRealization::computed;
    computed.storage.ports[0].storage = iv::graph_jit::PortStorageKind::background;
    EXPECT_TRUE(iv::graph_jit::uses_output_cache(computed, computed.storage.ports[0], true));
    EXPECT_FALSE(iv::graph_jit::uses_output_cache(computed, computed.storage.ports[0], false));
    EXPECT_TRUE(iv::graph_jit::persisted_storage_is_compatible(computed, 0, computed, 0));

    current.ports[0].output_production = iv::OutputProductionKind::region;
    EXPECT_FALSE(iv::graph_jit::persisted_storage_is_compatible(
        current, 0, previous, 0));
    current = previous;
    current.ports[0].authored_tock_output = false;
    current.ports[0].persisted_tick_output = true;
    current.ports[0].output_production = iv::OutputProductionKind::sequential;
    EXPECT_FALSE(iv::graph_jit::persisted_storage_is_compatible(
        current, 0, previous, 0));
    EXPECT_FALSE(iv::graph_jit::persisted_storage_is_compatible(
        previous, 0, current, 0));

    current = previous;
    current.storage.ports[0].continuous_geometry =
        *iv::continuous_value_storage_geometry(
            iv::continuous_value_type_id_v<iv::FFTBlock>,
            std::size_t{8});
    EXPECT_FALSE(iv::graph_jit::persisted_storage_is_compatible(
        current, 0, previous, 0));

    current = previous;
    current.storage.ports[0].sample_channels = {1};
    EXPECT_FALSE(iv::graph_jit::persisted_storage_is_compatible(
        current, 0, previous, 0));
}

TEST(GraphExecutorPersistedStorageCompatibility,
     IncludesEventProductionAndType)
{
    iv::graph_jit::BackgroundEvaluationPlan previous;
    previous.ports = {{
        .kind = iv::PortKind::event,
        .direction = iv::graph_jit::PortDirection::output,
        .output_production = iv::OutputProductionKind::coverage,
        .authored_tock_output = true,
        .realization = iv::OutputRealization::buffered,
        .event_type = iv::EventTypeId::trigger,
    }};
    previous.storage.ports = {{
        .kind = iv::PortKind::event,
        .storage = iv::graph_jit::PortStorageKind::persisted_data,
        .output_port = 0,
        .event_type = iv::EventTypeId::trigger,
    }};

    auto current = previous;
    EXPECT_TRUE(iv::graph_jit::persisted_storage_is_compatible(
        current, 0, previous, 0));
    current.ports[0].authored_tock_output = false;
    current.ports[0].persisted_tick_output = true;
    current.ports[0].output_production = iv::OutputProductionKind::sequential;
    EXPECT_FALSE(iv::graph_jit::persisted_storage_is_compatible(
        current, 0, previous, 0));

    current = previous;
    current.storage.ports[0].event_type = iv::EventTypeId::midi;
    EXPECT_FALSE(iv::graph_jit::persisted_storage_is_compatible(
        current, 0, previous, 0));
}

TEST(ProjectPlayback, CommandsAreOrderedAndAppliedOnlyAtBlockBoundaries)
{
    iv::ProjectPlayback playback;
    auto seek = playback.seek(index(100));
    auto const play = playback.set_running(true);
    EXPECT_LT(seek, play);
    EXPECT_EQ(playback.snapshot().applied_command, 0u);
    auto selected = playback.begin_block(48000);
    EXPECT_EQ(selected.state.foreground_playhead, index(100));
    EXPECT_FALSE(selected.audition);
    EXPECT_EQ(playback.snapshot().applied_command, 0u);
    playback.end_block(selected, 16);
    auto state = playback.snapshot();
    EXPECT_EQ(state.background_playhead, index(116));
    EXPECT_EQ(state.start_position, index(100));
    EXPECT_EQ(state.foreground_playhead, index(116));
    EXPECT_TRUE(state.running);
    EXPECT_EQ(state.applied_command, play);
    EXPECT_EQ(state.segment, 2u);
    (void)playback.toggle();
    (void)playback.toggle();
    selected = playback.begin_block(48000);
    EXPECT_EQ(selected.state.foreground_playhead, index(100));
    EXPECT_TRUE(selected.state.running);
}

TEST(ProjectPlayback, PauseReturnsToTheSelectedStartWithoutAudition)
{
    iv::ProjectPlayback playback;
    EXPECT_EQ(playback.snapshot().background_playhead, index(0));
    EXPECT_EQ(playback.snapshot().start_position, index(0));
    EXPECT_FALSE(playback.snapshot().running);
    playback.end_block(playback.begin_block(48000), 8);

    (void)playback.seek(index(-10));
    auto selected = playback.begin_block(48000);
    EXPECT_EQ(selected.selection().held_position, index(-10));
    EXPECT_TRUE(selected.audition);
    EXPECT_EQ(selected.state.foreground_playhead, index(8));
    EXPECT_EQ(playback.snapshot().start_position, index(0));
    playback.end_block(selected, 8);
    EXPECT_EQ(playback.snapshot().start_position, index(-10));
    EXPECT_EQ(playback.snapshot().foreground_playhead, index(16));

    (void)playback.toggle();
    playback.end_block(playback.begin_block(48000), 8);
    (void)playback.seek(index(50));
    playback.end_block(playback.begin_block(48000), 8);
    EXPECT_EQ(playback.snapshot().background_playhead, index(58));

    auto const pause = playback.set_running(false);
    selected = playback.begin_block(48000);
    EXPECT_EQ(selected.selection().held_position, index(50));
    EXPECT_EQ(selected.state.foreground_playhead, index(58));
    EXPECT_FALSE(selected.audition);
    // Old playback can continue while the return position is being prepared.
    playback.end_block(playback.current_block(48000), 8);
    selected = playback.begin_block(48000);
    EXPECT_EQ(playback.snapshot().background_playhead, index(66));
    EXPECT_EQ(selected.state.foreground_playhead, index(66));
    playback.end_block(selected, 8);
    EXPECT_EQ(playback.snapshot().applied_command, pause);
    EXPECT_EQ(playback.snapshot().background_playhead, index(50));
    EXPECT_EQ(playback.snapshot().start_position, index(50));
    EXPECT_EQ(playback.snapshot().foreground_playhead, index(74));
    EXPECT_FALSE(playback.snapshot().running);
    (void)playback.toggle();
    EXPECT_EQ(playback.begin_block(48000).state.foreground_playhead, index(50));
}

TEST(ProjectPlayback, FullCommandQueueRejectsNewCommandsWithoutReplacingAcceptedCommands)
{
    iv::ProjectPlayback playback;
    std::uint64_t last = 0;
    for (int i = 0; i < 256; ++i) last = playback.seek(index(i));
    EXPECT_THROW((void)playback.seek(index(1000)), std::runtime_error);
    auto selected = playback.begin_block(48000);
    EXPECT_EQ(selected.selection().held_position, index(255));
    EXPECT_EQ(selected.state.foreground_playhead, index(0));
    EXPECT_EQ(selected.state.applied_command, last);
    EXPECT_EQ(playback.snapshot().applied_command, 0u);
    auto next = playback.seek(index(-5));
    EXPECT_EQ(next, last + 1);
    EXPECT_EQ(playback.begin_block(48000).selection().held_position, index(-5));
}

TEST(ProjectPlayback, DragHoldsWhilePreparingAndResumesFromItsReleasePosition)
{
    using Phase = iv::ProjectPlaybackDragPhase;
    iv::ProjectPlayback playback;
    (void)playback.set_running(true);
    playback.end_block(playback.begin_block(48000), 16);

    auto const begin = playback.drag({Phase::begin, 1}, index(100));
    ASSERT_TRUE(begin);
    auto candidate = playback.begin_block(48000);
    EXPECT_EQ(candidate.selection().held_position, index(100));
    EXPECT_TRUE(candidate.audition);
    auto const fallback = playback.current_block(48000);
    EXPECT_EQ(fallback.selection().held_position, index(16));
    EXPECT_EQ(fallback.state.foreground_playhead, index(16));
    EXPECT_FALSE(fallback.audition);
    playback.end_block(fallback, 16);
    EXPECT_FALSE(playback.snapshot().running);
    EXPECT_EQ(playback.snapshot().start_position, index(0));
    EXPECT_EQ(playback.snapshot().foreground_playhead, index(32));
    EXPECT_LT(playback.snapshot().applied_command, *begin);

    (void)playback.drag({Phase::move, 1}, index(200));
    candidate = playback.begin_block(48000);
    EXPECT_EQ(candidate.selection().held_position, index(200));
    EXPECT_EQ(candidate.state.foreground_playhead, index(32));
    playback.end_block(candidate, 16);

    (void)playback.drag({Phase::end, 1}, index(300));
    candidate = playback.begin_block(48000);
    EXPECT_EQ(playback.current_block(48000).selection().held_position, index(200));
    EXPECT_TRUE(candidate.state.running);
    EXPECT_EQ(candidate.state.foreground_playhead, index(300));
    EXPECT_FALSE(candidate.audition);
    playback.end_block(candidate, 16);
    EXPECT_EQ(playback.snapshot().start_position, index(300));
    EXPECT_EQ(playback.snapshot().foreground_playhead, index(316));
    (void)playback.toggle();
    EXPECT_EQ(playback.begin_block(48000).selection().held_position, index(300));
}

TEST(ProjectPlayback, QuickDragRestoresTheOrderedRunningState)
{
    using Phase = iv::ProjectPlaybackDragPhase;
    iv::ProjectPlayback playback;
    (void)playback.toggle();
    (void)playback.drag({Phase::begin, 1}, index(-20));
    (void)playback.drag({Phase::move, 1}, index(-30));
    auto const end = playback.drag({Phase::end, 1}, index(-40));
    ASSERT_TRUE(end);
    EXPECT_FALSE(playback.snapshot().running);
    auto const candidate = playback.begin_block(48000);
    EXPECT_TRUE(candidate.state.running);
    EXPECT_EQ(candidate.state.applied_command, *end);
    EXPECT_EQ(candidate.state.foreground_playhead, index(-40));
}

TEST(ProjectPlayback, PausedDragReleasePreservesItsLastAudition)
{
    using Phase = iv::ProjectPlaybackDragPhase;
    iv::ProjectPlayback playback;
    (void)playback.drag({Phase::begin, 1}, index(-20));
    (void)playback.drag({Phase::end, 1}, index(-20));
    auto candidate = playback.begin_block(48000);
    EXPECT_EQ(candidate.selection().held_position, index(-20));
    EXPECT_TRUE(candidate.audition);
    playback.end_block(candidate, 16);

    (void)playback.drag({Phase::begin, 2}, index(-30));
    playback.end_block(playback.begin_block(48000), 16);
    (void)playback.drag({Phase::end, 2}, index(-30));
    candidate = playback.begin_block(48000);
    EXPECT_EQ(candidate.selection().held_position, index(-30));
    EXPECT_FALSE(candidate.audition);
}

TEST(ProjectPlayback, DragReleaseHasQueueSpaceAndRequiresTheActiveId)
{
    using Phase = iv::ProjectPlaybackDragPhase;
    iv::ProjectPlayback playback;
    ASSERT_TRUE(playback.drag({Phase::begin, 1}, index(1)));
    for (int i = 0; i < 254; ++i) ASSERT_TRUE(playback.drag({Phase::move, 1}, index(i)));
    EXPECT_THROW((void)playback.drag({Phase::move, 1}, index(900)), std::runtime_error);
    EXPECT_FALSE(playback.drag({Phase::end, 2}, index(800)));
    ASSERT_TRUE(playback.drag({Phase::end, 1}, index(700)));
    EXPECT_EQ(playback.begin_block(48000).selection().held_position, index(700));
}

TEST(ProjectPlayback, RunningPositionWrapsThroughTheSignedSampleSeam)
{
    iv::ProjectPlayback playback;
    (void)playback.seek(index(std::numeric_limits<std::int64_t>::max()));
    (void)playback.set_running(true);
    playback.end_block(playback.begin_block(96000), 2);
    EXPECT_EQ(playback.snapshot().background_playhead, index(std::numeric_limits<std::int64_t>::min() + 1));
    EXPECT_EQ(playback.snapshot().sample_rate, 96000);
}

TEST_F(GraphExecutorFixture, DeviceRenderingKeepsLiveTickStateAcrossPauseResumeAndSeek)
{
    iv::GraphExecutor executor;
    auto graph = compiled_graph(1, &observe_first);
    ASSERT_EQ(stage_graph(executor, graph), iv::GraphExecutorStageResult::accepted);
    executor.render_block(32, 48000);
    auto* storage = first_tick.storage;
    EXPECT_EQ(first_tick.sample_index, index(0));
    EXPECT_EQ(executor.playback_snapshot().background_playhead, index(0));
    EXPECT_FALSE(executor.playback_snapshot().running);
    (void)executor.set_playback_running(true);
    executor.render_block(32, 48000);
    EXPECT_EQ(first_tick.sample_index, index(0));
    EXPECT_EQ(executor.playback_snapshot().background_playhead, index(32));
    auto paused = executor.set_playback_running(false);
    executor.render_block(32, 48000);
    EXPECT_EQ(first_tick.sample_index, index(32));
    EXPECT_EQ(executor.playback_snapshot().applied_command, paused);
    executor.render_block(32, 48000);
    EXPECT_EQ(first_tick.sample_index, index(64));
    EXPECT_EQ(executor.playback_snapshot().background_playhead, index(0));
    EXPECT_FALSE(executor.playback_snapshot().running);
    (void)executor.seek_playback(index(-100));
    executor.render_block(32, 48000);
    EXPECT_EQ(first_tick.sample_index, index(96));
    EXPECT_EQ(executor.playback_snapshot().background_playhead, index(-100));
    (void)executor.set_playback_running(true);
    executor.render_block(32, 48000);
    EXPECT_EQ(first_tick.sample_index, index(-100));
    EXPECT_EQ(executor.playback_snapshot().background_playhead, index(-68));
    EXPECT_EQ(first_tick.calls, 6u);
    EXPECT_EQ(first_tick.storage, storage);
    EXPECT_EQ(executor.active_generation(), 1u);
}

TEST_F(GraphExecutorFixture, StagesActivatesAndDispatchesOnlyActiveGeneration)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_first);
    auto second = compiled_graph(2, &observe_second);

    EXPECT_THROW(executor.tick_block(index(0), 64), std::logic_error);
    EXPECT_EQ(
        stage_graph(executor, first), iv::GraphExecutorStageResult::accepted);
    EXPECT_FALSE(executor.active_generation());
    EXPECT_EQ(executor.pending_generation(), 1u);
    EXPECT_TRUE(executor.activate_pending(index(0)));
    EXPECT_EQ(executor.active_generation(), 1u);
    EXPECT_FALSE(executor.pending_generation());

    executor.tick_block(index(17), 32);
    EXPECT_EQ(first_tick.calls, 1u);
    EXPECT_EQ(first_tick.sample_index, index(17));
    EXPECT_EQ(first_tick.block_size, 32u);
    ASSERT_NE(first_tick.invocation, nullptr);
    EXPECT_TRUE(first_tick.invocation->sequential_sample_inputs.empty());
    EXPECT_TRUE(first_tick.invocation->sequential_event_inputs.empty());
    EXPECT_TRUE(first_tick.invocation->random_access_sample_inputs.empty());
    EXPECT_TRUE(first_tick.invocation->random_access_event_inputs.empty());

    EXPECT_EQ(
        stage_graph(executor, second), iv::GraphExecutorStageResult::accepted);
    executor.tick_block(index(49), 16);
    EXPECT_EQ(first_tick.calls, 2u);
    EXPECT_EQ(second_tick.calls, 0u);

    EXPECT_TRUE(executor.activate_pending(index(65)));
    EXPECT_EQ(executor.active_generation(), 2u);
    EXPECT_EQ(executor.active_graph()->project_generation, 2u);
    executor.tick_block(index(65), 64);
    EXPECT_EQ(second_tick.calls, 1u);
    EXPECT_EQ(second_tick.sample_index, index(65));
    EXPECT_EQ(second_tick.block_size, 64u);
    EXPECT_FALSE(executor.activate_pending(index(129)));
}

TEST_F(GraphExecutorFixture, PendingPreparationKeepsThePreviousGraphPlayingUntilItsSnapshotIsReady)
{
    initial_playback_started.store(false, std::memory_order_relaxed);
    allow_initial_playback.store(false, std::memory_order_relaxed);
    iv::GraphExecutor executor;
    struct ReleaseOnExit {
        ~ReleaseOnExit() {
            allow_initial_playback.store(true, std::memory_order_release);
            allow_initial_playback.notify_all();
        }
    } release;
    auto first = compiled_graph(1, &observe_first);
    ASSERT_EQ(stage_graph(executor, first), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    auto next = std::const_pointer_cast<iv::CompiledGraph>(
        compiled_graph(2, &observe_second, iv::NodeLayoutBuilder(64).build(), &wait_for_initial_playback));
    next->background_evaluation_plan.requires_live_tick_coverage = true;
    ASSERT_EQ(stage_graph(executor, next), iv::GraphExecutorStageResult::accepted);
    auto const started_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!initial_playback_started.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < started_deadline) std::this_thread::yield();
    ASSERT_TRUE(initial_playback_started.load(std::memory_order_acquire));
    EXPECT_FALSE(executor.activate_pending(index(64)));
    EXPECT_EQ(executor.active_generation(), 1u);
    executor.tick_block(index(64), 64);
    EXPECT_EQ(first_tick.calls, 1u);
    EXPECT_EQ(second_tick.calls, 0u);
    allow_initial_playback.store(true, std::memory_order_release);
    allow_initial_playback.notify_all();
    auto const ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool activated = false;
    while (!(activated = executor.activate_pending(index(128)))
        && std::chrono::steady_clock::now() < ready_deadline) std::this_thread::yield();
    ASSERT_TRUE(activated);
    EXPECT_EQ(executor.active_generation(), 2u);
    executor.tick_block(index(128), 64);
    EXPECT_EQ(second_tick.calls, 1u);
    EXPECT_EQ(first_tick.calls, 1u);
}

TEST_F(GraphExecutorFixture, TerminalPreparationDetachesOnlyTheUnactivatedSuffix)
{
    iv::GraphExecutor executor;
    initial_playback_started = false;
    allow_initial_playback = false;
    struct ReleaseOnExit {
        ~ReleaseOnExit() {
            allow_initial_playback.store(true, std::memory_order_release);
            allow_initial_playback.notify_all();
        }
    } release;
    auto wait = [](auto ready) {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!ready() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return ready();
    };
    auto first = compiled_graph(1, &observe_first);
    ASSERT_EQ(stage_graph(executor, first), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    auto failed = std::const_pointer_cast<iv::CompiledGraph>(compiled_graph(
        2, &observe_second, iv::NodeLayoutBuilder(64).build(), &fail_initial_playback));
    failed->background_evaluation_plan.requires_live_tick_coverage = true;
    ASSERT_EQ(stage_graph(executor, failed), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(wait([] { return initial_playback_started.load(std::memory_order_acquire); }));
    EXPECT_FALSE(executor.reject_failed_pending_generation());
    EXPECT_FALSE(executor.activate_pending(index(64)));
    executor.tick_block(index(64), 64);
    EXPECT_EQ(first_tick.calls, 1u);
    allow_initial_playback.store(true, std::memory_order_release);
    allow_initial_playback.notify_all();
    ASSERT_TRUE(wait([&] { return executor.has_failed_pending_generation(); }));

    auto queued = [](std::uint64_t number) {
        auto graph = std::const_pointer_cast<iv::CompiledGraph>(compiled_graph(
            number, &observe_second, iv::NodeLayoutBuilder(64).build(), &no_op_background_evaluate));
        graph->background_evaluation_plan.requires_live_tick_coverage = true;
        graph->configured_graph = std::make_shared<iv::ConfiguredGraph>();
        return graph;
    };
    auto third = queued(3);
    auto fourth = queued(4);
    ASSERT_EQ(stage_graph(executor, third), iv::GraphExecutorStageResult::accepted);
    auto const old_third = executor.accepted_graph();
    ASSERT_EQ(stage_graph(executor, fourth), iv::GraphExecutorStageResult::accepted);
    auto recovery = executor.reject_failed_pending_generation();
    ASSERT_TRUE(recovery);
    EXPECT_EQ(recovery->failed->project_generation, 2u);
    EXPECT_EQ(recovery->message, "executable cannot prepare");
    EXPECT_EQ(recovery->predecessor, first);
    EXPECT_EQ(executor.accepted_graph(), first);
    EXPECT_EQ(executor.active_generation(), 1u);
    EXPECT_FALSE(executor.pending_generation());
    EXPECT_FALSE(executor.background_execution_failures().any());
    ASSERT_EQ(recovery->successors.size(), 2u);
    EXPECT_EQ(recovery->successors[0].graph, third->configured_graph);
    EXPECT_EQ(recovery->successors[1].graph, fourth->configured_graph);
    EXPECT_THROW((void)executor.stage(old_third), std::invalid_argument);

    ASSERT_EQ(stage_graph(executor, third), iv::GraphExecutorStageResult::accepted);
    auto const rebuilt_third = executor.accepted_graph();
    EXPECT_FALSE(executor.activate_pending(index(128)));
    ASSERT_EQ(stage_graph(executor, fourth), iv::GraphExecutorStageResult::accepted);
    EXPECT_EQ(executor.accepted_graph()->cutover->predecessor.lock(), rebuilt_third);
    recovery->publish();
    auto activate = [&](std::uint64_t generation, std::int64_t sample) {
        return wait([&] {
            if (executor.active_generation() == generation) return true;
            (void)executor.activate_pending(index(sample));
            return executor.active_generation() == generation;
        });
    };
    ASSERT_TRUE(activate(3, 128));
    ASSERT_TRUE(activate(4, 192));
}

TEST_F(GraphExecutorFixture, FailedFirstGenerationCanBeRetiredWithNoSuccessor)
{
    iv::GraphExecutor executor;
    auto failed = std::const_pointer_cast<iv::CompiledGraph>(compiled_graph(1, &observe_first));
    failed->background_evaluation_plan.requires_live_tick_coverage = true;
    ASSERT_EQ(stage_graph(executor, failed), iv::GraphExecutorStageResult::accepted);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!executor.has_failed_pending_generation() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_TRUE(executor.has_failed_pending_generation());
    auto recovery = executor.reject_failed_pending_generation();
    ASSERT_TRUE(recovery);
    EXPECT_EQ(recovery->failed, failed);
    EXPECT_FALSE(recovery->predecessor);
    EXPECT_TRUE(recovery->successors.empty());
    EXPECT_FALSE(executor.accepted_graph());
    EXPECT_FALSE(executor.active_generation());
    EXPECT_FALSE(executor.background_execution_failures().any());
    ASSERT_EQ(stage_graph(executor, compiled_graph(2, &observe_second)), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    EXPECT_EQ(executor.active_generation(), 2u);
}

TEST_F(GraphExecutorFixture, PrivateBackgroundPreparationDoesNotPublishSuccessorData)
{
    auto graph = persisted_tock_graph();
    auto states = initialize_tock_states(graph);
    iv::PersistedDataStore data;
    iv::TickMaterializationStore materializations;
    iv::BackgroundCoverageState coverage(graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation(graph.background_evaluation_plan, graph.specialization.sample_rate);
    auto reader = data.register_reader();
    auto old = reader.pin();
    iv::BackgroundTransactionInputs inputs;
    inputs.roots.locally_changed_nodes = {0};
    inputs.roots.output_demands.push_back({.port = 0, .required = index_coverage({{0, 4}})});
    inputs.prepare_only = true;
    iv::BackgroundEvaluationTransaction preparation{graph, states, coverage, propagation,
        data, materializations, std::move(inputs)};
    auto prepared = preparation.execute();
    ASSERT_TRUE(prepared) << prepared.error();
    ASSERT_NE(prepared->prepared_data, nullptr);
    ASSERT_NE(prepared->prepared_materialization, nullptr);
    EXPECT_FALSE(prepared->published_data);
    EXPECT_FALSE(prepared->promoted_tick_materialization);
    EXPECT_TRUE(data.is_current(old.snapshot()));
    EXPECT_EQ(old->sample_region_count(), 0u);
    EXPECT_GT(prepared->prepared_data->sample_region_count(), 0u);
    auto selected = iv::RealtimePersistedState::prepare_owned(graph.project_generation,
        std::move(prepared->prepared_data), std::move(prepared->prepared_materialization));
    EXPECT_EQ(selected->data().version(), selected->materialization().data_version());
    EXPECT_EQ(selected->generation(), graph.project_generation);
}

TEST_F(GraphExecutorFixture,
       CompatibleRealtimeCodeKeepsNodeStorageAndAppliesTheLatestTarget)
{
    iv::GraphExecutor executor;
    std::size_t state_offset = 0;
    auto initial = std::const_pointer_cast<iv::CompiledGraph>(compiled_graph(
        1, &observe_first, manual_control_layout(state_offset)));
    initial->manual_controls.push_back(iv::ManualControlRuntimeBinding{
        .configured = {
            .id = manual_control_id(),
            .input = {0, iv::PortKind::sample, 0},
        },
        .state_storage_offset = state_offset,
        .participates = true,
    });

    ASSERT_EQ(executor.stage(initial), iv::GraphExecutorStageResult::accepted);
    EXPECT_TRUE(executor.set_manual_control_target(
        1, manual_control_id(), iv::Sample{0.25f}));
    ASSERT_TRUE(executor.activate_pending(index(0)));
    executor.tick_block(index(0), 16);
    ASSERT_NE(first_tick.storage, nullptr);
    auto* initial_storage = first_tick.storage;
    iv::ManualControlRuntimeState state{};
    std::memcpy(
        &state,
        initial_storage + state_offset,
        sizeof(state));
    EXPECT_FLOAT_EQ(std::bit_cast<float>(state.target_bits), 0.25f);

    auto replacement = std::make_shared<iv::CompiledGraph>(*initial);
    replacement->root_operations.tick_block = {.function = &observe_second};
    ASSERT_EQ(
        executor.stage_realtime_code(replacement),
        iv::GraphExecutorStageResult::accepted);
    EXPECT_TRUE(executor.set_manual_control_target(
        1, manual_control_id(), iv::Sample{0.75f}));

    executor.tick_block(index(16), 16);
    EXPECT_EQ(first_tick.calls, 2u);
    EXPECT_EQ(second_tick.calls, 0u);
    ASSERT_TRUE(executor.activate_pending(index(32)));
    executor.tick_block(index(32), 16);
    EXPECT_EQ(second_tick.calls, 1u);
    EXPECT_EQ(second_tick.storage, initial_storage);
    std::memcpy(
        &state,
        second_tick.storage + state_offset,
        sizeof(state));
    EXPECT_FLOAT_EQ(std::bit_cast<float>(state.target_bits), 0.75f);

    auto metadata_only = std::make_shared<iv::CompiledGraph>(*replacement);
    metadata_only->manual_controls[0].participates = false;
    EXPECT_TRUE(iv::GraphExecutor::can_replace_realtime_code(*initial, *metadata_only));
    auto changed_slots = std::make_shared<iv::CompiledGraph>(*replacement);
    changed_slots->background_evaluation_plan.tick_runtime.sequential_sample_inputs.push_back({});
    EXPECT_FALSE(iv::GraphExecutor::can_replace_realtime_code(*initial, *changed_slots));
    EXPECT_THROW((void)executor.stage_realtime_code(changed_slots), std::invalid_argument);

    auto incompatible = std::make_shared<iv::CompiledGraph>(*replacement);
    incompatible->node_layout = iv::NodeLayoutBuilder(64).build();
    EXPECT_THROW(
        (void)executor.stage_realtime_code(std::move(incompatible)),
        std::invalid_argument);

    auto stale = std::make_shared<iv::CompiledGraph>(*replacement);
    stale->project_generation = 2;
    EXPECT_EQ(
        executor.stage_realtime_code(std::move(stale)),
        iv::GraphExecutorStageResult::ignored_stale);
}

TEST_F(GraphExecutorFixture, CodeOnlyReplacementRejectsChangedBackgroundFallback)
{
    auto original = std::const_pointer_cast<iv::CompiledGraph>(compiled_graph(1, &observe_first));
    auto replacement = std::const_pointer_cast<iv::CompiledGraph>(compiled_graph(1, &observe_second));
    original->background_evaluation_plan.ports = {{.sample_fallback_value = 0.5f}};
    replacement->background_evaluation_plan.ports = original->background_evaluation_plan.ports;
    ASSERT_TRUE(iv::GraphExecutor::can_replace_realtime_code(*original, *replacement));
    replacement->background_evaluation_plan.ports[0].sample_fallback_value = 0.75f;
    EXPECT_FALSE(iv::GraphExecutor::can_replace_realtime_code(*original, *replacement));
}

TEST_F(GraphExecutorFixture, DynamicSuccessorUsesItsStoredTargetAfterStateMigration)
{
    iv::GraphExecutor executor;
    std::size_t state_offset = 0;
    auto initial = std::const_pointer_cast<iv::CompiledGraph>(compiled_graph(
        1, &observe_first, manual_control_layout(state_offset)));
    initial->manual_controls.push_back(iv::ManualControlRuntimeBinding{
        .configured = {
            .id = manual_control_id(),
            .input = {0, iv::PortKind::sample, 0},
            .settled_value = .75f,
            .dynamic = true,
        },
        .state_storage_offset = state_offset,
        .participates = true,
    });
    ASSERT_EQ(stage_graph(executor, initial), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    ASSERT_TRUE(executor.set_manual_control_target(1, manual_control_id(), .75f));
    executor.tick_block(index(0), 16);
    ASSERT_NE(first_tick.storage, nullptr);
    iv::ManualControlRuntimeState state{};
    std::memcpy(&state, first_tick.storage + state_offset, sizeof(state));
    state.effective_bits = std::bit_cast<std::uint32_t>(.6f);
    std::memcpy(first_tick.storage + state_offset, &state, sizeof(state));

    // Undo can replace the stored value while the released knob remains
    // dynamic. Migrating its runtime state must not restore the old target.
    auto successor = std::make_shared<iv::CompiledGraph>(*initial);
    successor->project_generation = 2;
    successor->root_operations.tick_block = {.function = &observe_second};
    successor->manual_controls[0].configured.settled_value = .25f;
    ASSERT_EQ(stage_graph(executor, successor), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(16)));
    executor.tick_block(index(16), 16);
    ASSERT_NE(second_tick.storage, nullptr);
    std::memcpy(&state, second_tick.storage + state_offset, sizeof(state));
    EXPECT_FLOAT_EQ(std::bit_cast<float>(state.target_bits), .25f);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(state.effective_bits), .6f);

    // A live target sent after staging still wins over the compiled seed.
    auto later = std::make_shared<iv::CompiledGraph>(*successor);
    later->project_generation = 3;
    later->manual_controls[0].configured.settled_value = .5f;
    ASSERT_EQ(stage_graph(executor, later), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.set_manual_control_target(3, manual_control_id(), .9f));
    ASSERT_TRUE(executor.activate_pending(index(32)));
    executor.tick_block(index(32), 16);
    std::memcpy(&state, second_tick.storage + state_offset, sizeof(state));
    EXPECT_FLOAT_EQ(std::bit_cast<float>(state.target_bits), .9f);
}

TEST_F(GraphExecutorFixture, OpenGestureReachesEveryGenerationThroughOneLiveTarget)
{
    iv::GraphExecutor executor;
    std::size_t state_offset = 0;
    auto initial = std::const_pointer_cast<iv::CompiledGraph>(compiled_graph(
        1, &observe_first, manual_control_layout(state_offset)));
    initial->manual_controls.push_back(iv::ManualControlRuntimeBinding{
        .configured = {
            .id = manual_control_id(),
            .input = {0, iv::PortKind::sample, 0},
            .settled_value = .1f,
            .dynamic = true,
        },
        .state_storage_offset = state_offset,
        .participates = true,
    });
    auto target_in = [&](std::byte const* storage) {
        iv::ManualControlRuntimeState state{};
        std::memcpy(&state, storage + state_offset, sizeof(state));
        return std::bit_cast<float>(state.target_bits);
    };
    auto controls = [&](iv::ManualControlGesturePhase gesture, float value) {
        iv::ProjectGraphGraphExecutorControlsRequest request;
        request.targets.push_back({.project_generation = 1, .id = manual_control_id(),
            .value = value, .gesture = gesture});
        executor.handle_project_graph_controls(request);
        return request.targets[0].live_target;
    };
    ASSERT_EQ(stage_graph(executor, initial), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));

    auto const live = controls(iv::ManualControlGesturePhase::begin, .3f);
    ASSERT_TRUE(live);
    executor.tick_block(index(0), 16);
    EXPECT_FLOAT_EQ(target_in(first_tick.storage), .3f);
    // A gesture update is one store into the shared live target.
    live->target_bits.store(std::bit_cast<std::uint32_t>(.4f));
    executor.tick_block(index(16), 16);
    EXPECT_FLOAT_EQ(target_in(first_tick.storage), .4f);

    // A successor staged mid-gesture binds the same live target, so its
    // compiled seed cannot replace a value sent while it was being prepared.
    auto successor = std::make_shared<iv::CompiledGraph>(*initial);
    successor->project_generation = 2;
    successor->root_operations.tick_block = {.function = &observe_second};
    ASSERT_EQ(stage_graph(executor, successor), iv::GraphExecutorStageResult::accepted);
    live->target_bits.store(std::bit_cast<std::uint32_t>(.45f));
    ASSERT_TRUE(executor.activate_pending(index(32)));
    executor.tick_block(index(32), 16);
    EXPECT_FLOAT_EQ(target_in(second_tick.storage), .45f);

    EXPECT_EQ(controls(iv::ManualControlGesturePhase::end, .5f), nullptr);
    EXPECT_EQ(live->open_gestures.load(), 0u);
    // Model the final store of a callback that loaded the old live value just
    // before end committed .5. The following call must repair that stale copy
    // even though open_gestures is now zero.
    auto& late_target = *reinterpret_cast<std::uint32_t*>(
        const_cast<std::byte*>(second_tick.storage) + state_offset
        + offsetof(iv::ManualControlRuntimeState, target_bits));
    std::atomic_ref<std::uint32_t>(late_target).store(std::bit_cast<std::uint32_t>(.45f));
    executor.tick_block(index(48), 16);
    EXPECT_FLOAT_EQ(target_in(second_tick.storage), .5f);

    // A closed gesture no longer overrides a later generation's settled value,
    // as when undo restores a value while the released knob stays dynamic.
    auto restored = std::make_shared<iv::CompiledGraph>(*successor);
    restored->project_generation = 3;
    restored->manual_controls[0].configured.settled_value = .2f;
    ASSERT_EQ(stage_graph(executor, restored), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(64)));
    executor.tick_block(index(64), 16);
    EXPECT_FLOAT_EQ(target_in(second_tick.storage), .2f);
}

TEST_F(GraphExecutorFixture, CodeActivationPublishesDynamicTargetsAndDefersCodeReclamation)
{
    iv::GraphExecutor executor;
    std::size_t offset = 0;
    auto constant = std::const_pointer_cast<iv::CompiledGraph>(compiled_graph(
        1, &observe_first, manual_control_layout(offset)));
    constant->manual_controls.push_back({
        .configured = {.id = manual_control_id(), .input = {0, iv::PortKind::sample, 0}},
        .state_storage_offset = offset, .participates = true,
    });
    auto target_value = [&](std::byte const* storage) {
        iv::ManualControlRuntimeState state{};
        std::memcpy(&state, storage + offset, sizeof(state));
        return std::bit_cast<float>(state.target_bits);
    };
    ASSERT_EQ(stage_graph(executor, constant), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    iv::ProjectGraphGraphExecutorControlsRequest begin;
    begin.targets.push_back({.project_generation = 1, .id = manual_control_id(),
        .value = .3f, .gesture = iv::ManualControlGesturePhase::begin});
    executor.handle_project_graph_controls(begin);
    auto live = begin.targets[0].live_target;
    ASSERT_TRUE(live);
    live->target_bits.store(std::bit_cast<std::uint32_t>(.4f));
    executor.tick_block(index(0), 16);
    // An open gesture cannot make constant code poll a target prematurely.
    EXPECT_FLOAT_EQ(target_value(first_tick.storage), .3f);

    auto dynamic = std::make_shared<iv::CompiledGraph>(*constant);
    dynamic->manual_controls[0].configured.dynamic = true;
    dynamic->root_operations.tick_block = {.function = &observe_second};
    std::weak_ptr<iv::CompiledGraph const> dynamic_lifetime = dynamic;
    ASSERT_EQ(executor.stage_realtime_code(dynamic), iv::GraphExecutorStageResult::accepted);
    dynamic.reset();
    live->target_bits.store(std::bit_cast<std::uint32_t>(.45f));
    executor.tick_block(index(16), 16);
    EXPECT_FLOAT_EQ(target_value(first_tick.storage), .3f);
    ASSERT_TRUE(executor.activate_pending(index(32)));
    executor.tick_block(index(32), 16);
    EXPECT_FLOAT_EQ(target_value(second_tick.storage), .45f);

    iv::ProjectGraphGraphExecutorControlsRequest end;
    end.targets.push_back({.project_generation = 1, .id = manual_control_id(),
        .value = .5f, .gesture = iv::ManualControlGesturePhase::end});
    executor.handle_project_graph_controls(end);
    auto& state_bits = *reinterpret_cast<std::uint32_t*>(second_tick.storage + offset);
    std::atomic_ref<std::uint32_t>(state_bits).store(std::bit_cast<std::uint32_t>(.45f));
    executor.tick_block(index(48), 16);
    EXPECT_FLOAT_EQ(target_value(second_tick.storage), .5f);

    auto settled = std::make_shared<iv::CompiledGraph>(*constant);
    ASSERT_EQ(executor.stage_realtime_code(settled), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(64)));
    EXPECT_FALSE(dynamic_lifetime.expired()); // Activation retires; it cannot destroy code.
    executor.tick_block(index(64), 16);
    EXPECT_FLOAT_EQ(target_value(first_tick.storage), .5f);
    // Another ready variant can replace an unactivated one. Cold staging also
    // reclaims the retired dynamic variant and its binding list.
    ASSERT_EQ(executor.stage_realtime_code(settled), iv::GraphExecutorStageResult::accepted);
    EXPECT_TRUE(dynamic_lifetime.expired());
    ASSERT_EQ(executor.stage_realtime_code(settled), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(80)));
    executor.tick_block(index(80), 16);
    EXPECT_FLOAT_EQ(target_value(first_tick.storage), .5f);
}

TEST_F(GraphExecutorFixture, PreservesEveryAcceptedGenerationBeforeActivation)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_first);
    auto second = compiled_graph(2, &observe_second);

    EXPECT_EQ(
        stage_graph(executor, first), iv::GraphExecutorStageResult::accepted);
    EXPECT_EQ(
        stage_graph(executor, first), iv::GraphExecutorStageResult::ignored_stale);
    EXPECT_EQ(
        stage_graph(executor, second), iv::GraphExecutorStageResult::accepted);
    EXPECT_EQ(executor.pending_generation(), 1u);
    EXPECT_TRUE(executor.activate_pending(index(0)));
    EXPECT_EQ(executor.active_generation(), 1u);
    EXPECT_EQ(executor.pending_generation(), 2u);
    EXPECT_TRUE(executor.activate_pending(index(0)));
    EXPECT_EQ(executor.active_generation(), 2u);
    EXPECT_EQ(
        stage_graph(executor, first), iv::GraphExecutorStageResult::ignored_stale);
}

TEST_F(GraphExecutorFixture, ProjectGraphSubmissionMustAdvanceTheAcceptedTail)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_first);

    auto accepted = in_event_source([&] { return executor.handle_project_graph_transaction({.graph = first}); });
    EXPECT_TRUE(accepted.attempted);
    ASSERT_TRUE(accepted.prepared);
    EXPECT_FALSE(executor.pending_generation());
    EXPECT_EQ(std::move(accepted.prepared).publish(), iv::GraphExecutorStageResult::accepted);
    auto stale = in_event_source([&] { return executor.handle_project_graph_transaction({.graph = first}); });
    EXPECT_TRUE(stale.attempted);
    EXPECT_FALSE(stale.prepared);
    EXPECT_EQ(executor.pending_generation(), 1u);
}

TEST_F(GraphExecutorFixture, AppendsAcceptedGenerationsAfterTheAcceptedChainTail)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_first);
    auto second = compiled_graph(2, &observe_second);
    auto third = compiled_graph(3, &observe_second);

    ASSERT_EQ(stage_graph(executor, first), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    ASSERT_EQ(executor.active_generation(), 1u);

    ASSERT_EQ(stage_graph(executor, second), iv::GraphExecutorStageResult::accepted);
    ASSERT_EQ(stage_graph(executor, third), iv::GraphExecutorStageResult::accepted);
    EXPECT_EQ(executor.active_generation(), 1u);
    EXPECT_EQ(executor.pending_generation(), 2u);

    ASSERT_TRUE(executor.activate_pending(index(64)));
    EXPECT_EQ(executor.active_generation(), 2u);
    EXPECT_EQ(executor.pending_generation(), 3u);
    ASSERT_TRUE(executor.activate_pending(index(64)));
    EXPECT_EQ(executor.active_generation(), 3u);
}

TEST_F(GraphExecutorFixture, RejectsACutoverThatSkipsTheAcceptedChainTail)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_first);
    auto second = compiled_graph(2, &observe_second);
    auto third = compiled_graph(3, &observe_second);

    ASSERT_EQ(stage_graph(executor, first), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));

    ASSERT_EQ(stage_graph(executor, second), iv::GraphExecutorStageResult::accepted);
    auto skipped = std::make_shared<iv::CompiledGraph>(*third);
    skipped->cutover.emplace(iv::CompiledGraphCutover{
        .predecessor = first,
        .predecessor_phase = iv::CompiledGraphRealtimePhase::steady,
    });
    EXPECT_THROW(executor.stage(std::move(skipped)), std::invalid_argument);
    EXPECT_EQ(executor.pending_generation(), 2u);
    EXPECT_EQ(executor.active_generation(), 1u);
}

TEST_F(
    GraphExecutorFixture,
    TransitionRootExpiresBySequentialProgressAcrossGlobalJump)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_second);
    ASSERT_EQ(stage_graph(executor, first), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));

    auto second = transition_graph(
        2,
        first,
        iv::CompiledGraphRealtimePhase::steady,
        64,
        &observe_first_and_advance_transition_cursor,
        &observe_second);
    ASSERT_EQ(executor.stage(second), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(100)));

    first_tick = {};
    second_tick = {};
    executor.tick_block(index(100), 32);
    EXPECT_EQ(first_tick.calls, 1u);
    EXPECT_EQ(second_tick.calls, 0u);
    auto third = compiled_graph(3, &observe_second);
    ASSERT_EQ(stage_graph(executor, third), iv::GraphExecutorStageResult::accepted);
    EXPECT_FALSE(executor.activate_pending(index(132)));
    executor.tick_block(index(10'000), 32);
    EXPECT_EQ(first_tick.calls, 2u);
    EXPECT_TRUE(executor.activate_pending(index(10'032)));
    EXPECT_EQ(executor.active_generation(), 3u);

    executor.tick_block(index(-4'000), 32);
    EXPECT_EQ(first_tick.calls, 2u);
    EXPECT_EQ(second_tick.calls, 1u);
}

TEST_F(
    GraphExecutorFixture,
    EventTransitionExpiresByProcessedExtentAcrossGlobalJump)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_second);
    ASSERT_EQ(stage_graph(executor, first), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));

    auto second = transition_graph(
        2,
        first,
        iv::CompiledGraphRealtimePhase::steady,
        0,
        &observe_first,
        &observe_second,
        64);
    ASSERT_EQ(stage_graph(executor, second), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(100)));

    first_tick = {};
    second_tick = {};
    executor.tick_block(index(100), 32);
    EXPECT_EQ(first_tick.calls, 1u);
    EXPECT_EQ(second_tick.calls, 0u);
    executor.tick_block(index(10'000), 32);
    EXPECT_EQ(first_tick.calls, 2u);

    executor.tick_block(index(-4'000), 32);
    EXPECT_EQ(first_tick.calls, 2u);
    EXPECT_EQ(second_tick.calls, 1u);
}

TEST_F(
    GraphExecutorFixture,
    RejectsCutoverWhoseRequiredPredecessorPhaseCanExpireBeforeActivation)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_second);
    ASSERT_EQ(stage_graph(executor, first), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));

    auto second = transition_graph(
        2,
        first,
        iv::CompiledGraphRealtimePhase::steady,
        64,
        &observe_first_and_advance_transition_cursor,
        &observe_second);
    ASSERT_EQ(stage_graph(executor, second), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(100)));
    executor.tick_block(index(100), 32);

    auto third = transition_graph(
        3,
        second,
        iv::CompiledGraphRealtimePhase::transition,
        16,
        &observe_first_and_advance_transition_cursor,
        &observe_second);
    EXPECT_THROW(executor.stage(third), std::invalid_argument);
    EXPECT_FALSE(executor.pending_generation());
    EXPECT_EQ(executor.active_generation(), 2u);
}

TEST_F(
    GraphExecutorFixture,
    PreparesRealtimeMigrationWithoutSnapshottingMutableStorage)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(
        1, &write_raw_state, persistent_raw_layout(first_raw_offset));
    auto second = compiled_graph(
        2, &observe_raw_state, persistent_raw_layout(second_raw_offset));

    ASSERT_EQ(stage_graph(executor, first), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));

    ASSERT_EQ(stage_graph(executor, second), iv::GraphExecutorStageResult::accepted);
    EXPECT_EQ(executor.active_generation(), 1u);
    // Migration preparation must not snapshot mutable realtime bytes. The
    // value written after staging is the value copied at activation.
    executor.tick_block(index(0), 64);
    ASSERT_TRUE(executor.activate_pending(index(64)));
    executor.tick_block(index(64), 64);
    EXPECT_EQ(migrated_raw_value, 0x5au);
}

TEST_F(
    GraphExecutorFixture,
    AcceptedChainMigrationReadsEachPredecessorAtItsOwnCutover)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(
        1, &write_raw_state, persistent_raw_layout(first_raw_offset));
    auto second = compiled_graph(
        2, &replace_second_raw_state, persistent_raw_layout(second_raw_offset));
    auto third = compiled_graph(
        3, &observe_third_raw_state, persistent_raw_layout(third_raw_offset));

    ASSERT_EQ(stage_graph(executor, first), iv::GraphExecutorStageResult::accepted);
    ASSERT_EQ(stage_graph(executor, second), iv::GraphExecutorStageResult::accepted);
    ASSERT_EQ(stage_graph(executor, third), iv::GraphExecutorStageResult::accepted);

    ASSERT_TRUE(executor.activate_pending(index(0)));
    executor.tick_block(index(0), 64);
    ASSERT_TRUE(executor.activate_pending(index(64)));
    executor.tick_block(index(64), 64);
    ASSERT_EQ(migrated_raw_value, 0x5au);
    ASSERT_TRUE(executor.activate_pending(index(128)));
    executor.tick_block(index(128), 64);
    EXPECT_EQ(migrated_raw_value, 0x6bu);
}

TEST_F(GraphExecutorFixture, GenerationCutoverWakesTheBackgroundActor)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_first);
    ASSERT_EQ(
        stage_graph(executor, first),
        iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    auto second = compiled_graph(2, &observe_second);
    ASSERT_EQ(
        stage_graph(executor, second),
        iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));

    auto const deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{2};
    while (executor.background_generation() != 2u
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_EQ(executor.background_generation(), 2u);
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST_F(GraphExecutorFixture, BackgroundStateMovesOnlyAfterOrderedCutoverOnTheBackgroundOwner)
{
    using Probe = BackgroundLifecycleProbe;
    Probe::control_thread = std::this_thread::get_id();
    Probe::initialized = Probe::moved = Probe::released = Probe::live = 0;
    Probe::wrong_thread = false;
    auto wait_for = [](auto&& ready) {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!ready() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return ready();
    };
    {
        iv::GraphExecutor executor;
        ASSERT_EQ(stage_graph(executor, background_lifecycle_graph(1)), iv::GraphExecutorStageResult::accepted);
        ASSERT_TRUE(executor.activate_pending(index(0)));
        ASSERT_TRUE(wait_for([&] { return executor.background_generation() == 1u && Probe::initialized == 1; }));
        ASSERT_EQ(stage_graph(executor, background_lifecycle_graph(2)), iv::GraphExecutorStageResult::accepted);
        EXPECT_EQ(Probe::moved, 0);
        ASSERT_TRUE(executor.activate_pending(index(64)));
        ASSERT_TRUE(wait_for([&] { return executor.background_generation() == 2u; }));
        EXPECT_EQ(Probe::initialized, 1);
        EXPECT_EQ(Probe::moved, 1);
        EXPECT_FALSE(Probe::wrong_thread);
    }
    EXPECT_EQ(Probe::live, 0);
    EXPECT_EQ(Probe::released, 2);
    EXPECT_FALSE(Probe::wrong_thread);
}

TEST_F(GraphExecutorFixture, PrivateSuccessorPreparationStartsBackgroundStateFresh)
{
    using Probe = BackgroundLifecycleProbe;
    Probe::control_thread = std::this_thread::get_id();
    Probe::initialized = Probe::moved = Probe::released = Probe::live = 0;
    Probe::wrong_thread = false;
    initial_playback_started = false;
    allow_initial_playback = true;
    {
        iv::GraphExecutor executor;
        ASSERT_EQ(stage_graph(executor, background_lifecycle_graph(1)), iv::GraphExecutorStageResult::accepted);
        ASSERT_TRUE(executor.activate_pending(index(0)));
        auto next = background_lifecycle_graph(2);
        next->background_evaluation_plan.requires_live_tick_coverage = true;
        next->background_operations = {
            .propagate_forward = &no_op_background_evaluate,
            .propagate_reverse = &no_op_background_evaluate,
            .evaluate = &wait_for_initial_playback,
        };
        ASSERT_EQ(stage_graph(executor, next), iv::GraphExecutorStageResult::accepted);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool activated = false;
        while (!(activated = executor.activate_pending(index(64)))
            && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        ASSERT_TRUE(activated);
        EXPECT_EQ(Probe::initialized, 2);
        EXPECT_EQ(Probe::moved, 0);
    }
    EXPECT_EQ(Probe::live, 0);
    EXPECT_EQ(Probe::released, 2);
    EXPECT_FALSE(Probe::wrong_thread);
}

TEST_F(GraphExecutorFixture, RejectsInvalidGraphsAndBlockSizes)
{
    iv::GraphExecutor executor;
    EXPECT_THROW(executor.stage(nullptr), std::invalid_argument);

    auto first = compiled_graph(1, &observe_first);
    ASSERT_EQ(
        stage_graph(executor, first),
        iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    EXPECT_FALSE(executor.realtime_production_failures().any());
    EXPECT_FALSE(executor.realtime_capacity_maintenance_failures().any());
    EXPECT_THROW(executor.tick_block(index(0), 0), std::invalid_argument);
    EXPECT_THROW(executor.tick_block(index(0), 65), std::invalid_argument);
}

TEST_F(
    GraphExecutorFixture,
    BackgroundTransactionPublishesPersistedDataOnlyWhenItChanges)
{
    auto graph = persisted_tock_graph();
    auto tock_states = initialize_tock_states(graph);
    iv::BackgroundCoverageState coverage(
        graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();

    iv::BackgroundEvaluationTransaction first{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 7,
            .roots = {
                .locally_changed_nodes = {0},
                .output_demands = {{
                    .port = 0,
                    .required = index_coverage({{0, 8}}),
                }},
            },
        }};
    auto first_result = first.execute();
    ASSERT_TRUE(first_result.has_value()) << first_result.error();
    ASSERT_TRUE(first_result->published_data.has_value());
    EXPECT_EQ(
        *first_result->published_data,
        (iv::PersistedDataVersion{.semantic = 7, .revision = 1}));
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
    // execute() releases its base pin even while the transaction object lives.
    EXPECT_EQ(data.retired_snapshot_count(), 1u);
    EXPECT_EQ(data.reclaim_retired(), 1u);

    auto const output = data.resolve_output(iv::PersistedOutputId{
        iv::GenerationLocalPersistedOutputId{
            .generation = 1,
            .port = 0,
            .kind = iv::PortKind::sample,
        }});
    {
        auto pin = reader.pin();
        auto const* region = pin->find_sample_region(output, index(0));
        ASSERT_NE(region, nullptr);
        EXPECT_EQ(region->range, range(0, 8));
        ASSERT_EQ(region->storage_size_bytes(), 8u * sizeof(iv::Sample));
        EXPECT_FLOAT_EQ(region->at(index(0), 0).value, 100.0f);
        EXPECT_FLOAT_EQ(region->at(index(3), 0).value, 103.0f);
        EXPECT_FLOAT_EQ(region->at(index(4), 0).value, 104.0f);
        EXPECT_FLOAT_EQ(region->at(index(7), 0).value, 107.0f);
    }

    iv::BackgroundEvaluationTransaction unchanged{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 7,
            .roots = {
                .output_demands = {{
                    .port = 0,
                    .required = index_coverage({{0, 8}}),
                }},
            },
        }};
    auto unchanged_result = unchanged.execute();
    ASSERT_TRUE(unchanged_result.has_value()) << unchanged_result.error();
    EXPECT_FALSE(unchanged_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
    {
        auto pin = reader.pin();
        EXPECT_EQ(
            pin->version(),
            (iv::PersistedDataVersion{.semantic = 7, .revision = 1}));
    }

    persisted_probe_coverage = index_coverage({{0, 4}});
    iv::BackgroundEvaluationTransaction shrink{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 8,
            .roots = {.locally_changed_nodes = {0}},
        }};
    auto shrink_result = shrink.execute();
    ASSERT_TRUE(shrink_result.has_value()) << shrink_result.error();
    ASSERT_TRUE(shrink_result->published_data.has_value());
    EXPECT_EQ(
        *shrink_result->published_data,
        (iv::PersistedDataVersion{.semantic = 8, .revision = 2}));
    // A changed logical shape invalidates cached contents, but no consumer
    // requested new values. Publication updates metadata without evaluating.
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
    {
        auto pin = reader.pin();
        auto const* logical = pin->find_sample_logical_coverage(output);
        ASSERT_NE(logical, nullptr);
        EXPECT_EQ(*logical, index_coverage({{0, 4}}));
        EXPECT_EQ(pin->find_sample_region(output, index(0)), nullptr);
        EXPECT_EQ(pin->find_sample_region(output, index(4)), nullptr);
    }

    // Demand after invalidation computes exactly the requested content window
    // while retaining the whole producer-published logical Region.
    iv::BackgroundEvaluationTransaction refill{
        graph, tock_states, coverage, propagation, data,
        {.semantic_version = 8,
         .roots = {.output_demands = {{.port = 0, .required = index_coverage({{1, 3}})}}}}};
    auto refill_result = refill.execute();
    ASSERT_TRUE(refill_result.has_value()) << refill_result.error();
    ASSERT_TRUE(refill_result->published_data.has_value());
    EXPECT_EQ(*refill_result->published_data,
        (iv::PersistedDataVersion{.semantic = 8, .revision = 3}));
    EXPECT_EQ(persisted_probe_evaluate_calls, 2u);
    auto pin = reader.pin();
    auto const* logical = pin->find_sample_logical_coverage(output);
    ASSERT_NE(logical, nullptr);
    EXPECT_EQ(*logical, index_coverage({{0, 4}}));
    auto const* available = pin->find_sample_coverage(output, iv::mono_planar_channel_layout);
    ASSERT_NE(available, nullptr);
    EXPECT_EQ(*available, index_coverage({{1, 3}}));
    auto const* region = pin->find_sample_region(output, index(1));
    ASSERT_NE(region, nullptr);
    EXPECT_FLOAT_EQ(region->at(index(1), 0).value, 101.0f);
    EXPECT_FLOAT_EQ(region->at(index(2), 0).value, 102.0f);
}

TEST_F(GraphExecutorFixture, EvictedTockResultsReconstructTheirRequestedWindowAndKeepLogicalIdentity)
{
    for (bool computed : {false, true}) {
        SCOPED_TRACE(computed);
        persisted_probe_evaluate_calls = 0;
        auto graph = std::make_shared<iv::CompiledGraph>(
            computed ? ephemeral_tock_graph() : persisted_tock_graph());
        auto states = initialize_tock_states(*graph);
        auto const& plan = graph->background_evaluation_plan;
        iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
        iv::BackgroundPropagationWorkspace propagation(plan, graph->specialization.sample_rate);
        iv::PersistedDataStore data;
        auto reader = data.register_reader();
        auto handle = data.resolve_output(iv::persisted_output_id(plan, 0, graph->project_generation));
        auto evaluate = [&](bool changed, iv::IndexCoverage demand) {
            iv::CoveragePropagationRequest roots;
            if (changed) roots.locally_changed_nodes = {0};
            roots.output_demands.push_back({.port = 0, .required = std::move(demand)});
            iv::BackgroundEvaluationTransaction transaction{*graph, states, coverage, propagation, data,
                {.semantic_version = 7, .roots = std::move(roots), .reconstruction_graph = graph}};
            return transaction.execute();
        };
        auto first = evaluate(true, index_coverage({{0, 8}}));
        ASSERT_TRUE(first.has_value()) << first.error();
        EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
        auto reused = evaluate(false, index_coverage({{0, 8}}));
        ASSERT_TRUE(reused.has_value()) << reused.error();
        EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
        auto old = reader.pin();
        auto const version = old->version();
        data.reclaim_cached_payload({});
        auto other = data.register_reader();
        {
            auto empty = other.pin();
            EXPECT_EQ(empty->version(), version);
            EXPECT_TRUE(empty->shares_output_with(old.snapshot(), handle));
            EXPECT_EQ(*empty->find_sample_logical_coverage(handle), index_coverage({{0, 8}}));
            EXPECT_TRUE(empty->find_sample_coverage(handle, iv::mono_planar_channel_layout)->empty());
        }
        auto rebuilt = evaluate(false, index_coverage({{2, 3}}));
        ASSERT_TRUE(rebuilt.has_value()) << rebuilt.error();
        EXPECT_EQ(persisted_probe_evaluate_calls, 2u);
        auto current = other.pin();
        EXPECT_TRUE(current->shares_output_with(old.snapshot(), handle));
        EXPECT_EQ(*current->find_sample_coverage(handle, iv::mono_planar_channel_layout), index_coverage({{2, 3}}));
        ASSERT_NE(current->find_sample_region(handle, index(2)), nullptr);
        EXPECT_FLOAT_EQ(current->find_sample_region(handle, index(2))->at(index(2), 0).value, 102);
        EXPECT_FLOAT_EQ(old->find_sample_region(handle, index(0))->at(index(0), 0).value, 100);
        auto context = current->reconstruction(handle);
        ASSERT_NE(context, nullptr);
        auto canonical_version = current->version();
        auto historical = iv::BackgroundEvaluationTransaction::reconstruct(
            data, context, 0, index_coverage({{4, 2}}), resources_);
        ASSERT_TRUE(historical.has_value()) << historical.error();
        ASSERT_NE(historical->prepared_data, nullptr);
        auto const* reconstructed = historical->prepared_data->find_sample_region(handle, index(4));
        ASSERT_NE(reconstructed, nullptr);
        EXPECT_FLOAT_EQ(reconstructed->at(index(4), 0).value, 104);
        EXPECT_EQ(persisted_probe_evaluate_calls, 3u);
        current = {};
        auto latest = other.pin();
        EXPECT_EQ(latest->version(), canonical_version);
        EXPECT_EQ(*latest->find_sample_coverage(handle, iv::mono_planar_channel_layout), index_coverage({{2, 3}}));
    }
}

TEST_F(
    GraphExecutorFixture,
    FailedPersistedBackgroundEvaluationPromotesNeitherDataNorCoverage)
{
    auto graph = persisted_tock_graph();
    auto tock_states = initialize_tock_states(graph);
    iv::BackgroundCoverageState coverage(
        graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    auto const request = iv::BackgroundTransactionInputs{
        .semantic_version = 7,
        .roots = {
            .locally_changed_nodes = {0},
            .output_demands = {{
                .port = 0,
                .required = index_coverage({{0, 8}}),
            }},
        },
    };

    throw_persisted_probe_evaluate = true;
    iv::BackgroundEvaluationTransaction failed{
        graph, tock_states, coverage, propagation, data, request};
    auto failed_result = failed.execute();
    EXPECT_FALSE(failed_result.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
    {
        auto pin = reader.pin();
        EXPECT_EQ(pin->version(), (iv::PersistedDataVersion{}));
        EXPECT_EQ(pin->sample_region_count(), 0u);
    }

    throw_persisted_probe_evaluate = false;
    iv::BackgroundEvaluationTransaction demand_only{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 7,
            .roots = {
                .output_demands = {{
                    .port = 0,
                    .required = index_coverage({{0, 8}}),
                }},
            },
        }};
    auto demand_only_result = demand_only.execute();
    ASSERT_TRUE(demand_only_result.has_value()) << demand_only_result.error();
    EXPECT_FALSE(demand_only_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);

    iv::BackgroundEvaluationTransaction retry{
        graph, tock_states, coverage, propagation, data, request};
    auto retry_result = retry.execute();
    ASSERT_TRUE(retry_result.has_value()) << retry_result.error();
    ASSERT_TRUE(retry_result->published_data.has_value());
    EXPECT_EQ(retry_result->published_data->semantic, 7u);
    EXPECT_GT(retry_result->published_data->revision, 0u);
    auto published = reader.pin();
    EXPECT_EQ(published->version(), *retry_result->published_data);
    EXPECT_EQ(published->sample_region_count(), 1u);
    EXPECT_EQ(persisted_probe_evaluate_calls, 2u);
}

TEST_F(
    GraphExecutorFixture,
    IncompletePersistedBackgroundEvaluationPromotesNeitherDataNorCoverage)
{
    auto graph = persisted_tock_graph();
    auto tock_states = initialize_tock_states(graph);
    iv::BackgroundCoverageState coverage(
        graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    auto const request = iv::BackgroundTransactionInputs{
        .semantic_version = 7,
        .roots = {
            .locally_changed_nodes = {0},
            .output_demands = {{
                .port = 0,
                .required = index_coverage({{0, 8}}),
            }},
        },
    };

    omit_last_persisted_probe_sample = true;
    iv::BackgroundEvaluationTransaction incomplete{
        graph, tock_states, coverage, propagation, data, request};
    auto incomplete_result = incomplete.execute();
    ASSERT_FALSE(incomplete_result.has_value());
    EXPECT_NE(
        incomplete_result.error().find("did not initialize every selected"),
        std::string::npos);
    {
        auto pin = reader.pin();
        EXPECT_EQ(pin->version(), (iv::PersistedDataVersion{}));
        EXPECT_EQ(pin->sample_region_count(), 0u);
    }

    omit_last_persisted_probe_sample = false;
    iv::BackgroundEvaluationTransaction retry{
        graph, tock_states, coverage, propagation, data, request};
    auto retry_result = retry.execute();
    ASSERT_TRUE(retry_result.has_value()) << retry_result.error();
    ASSERT_TRUE(retry_result->published_data.has_value());
    EXPECT_EQ(retry_result->published_data->semantic, 7u);
    EXPECT_GT(retry_result->published_data->revision, 0u);
    auto published = reader.pin();
    EXPECT_EQ(published->version(), *retry_result->published_data);
    EXPECT_EQ(published->sample_region_count(), 1u);
}

TEST_F(
    GraphExecutorFixture,
    BackgroundTransactionRunsReplayOverUnionButCompletesOnlyDemandedOutputs)
{
    auto graph = replay_probe_graph();
    auto tock_states = initialize_tock_states(graph);
    iv::BackgroundCoverageState coverage(
        graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;

    iv::BackgroundEvaluationTransaction transaction{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 1,
            .roots = {
                .output_changes = {
                    {
                        .port = 0,
                        .coverage = index_coverage({{0, 8}}),
                    },
                    {
                        .port = 1,
                        .coverage = index_coverage({{0, 8}}),
                    },
                },
                .output_demands = {
                    {
                        .port = 0,
                        .required = index_coverage({{0, 4}}),
                    },
                    {
                        .port = 1,
                        .required = index_coverage({{4, 8}}),
                    },
                },
            },
        }};
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);
    EXPECT_FALSE(result->published_data.has_value());
    EXPECT_EQ(replay_probe_evaluate_calls, 1u);
    EXPECT_EQ(
        replay_probe_regions,
        (std::vector<iv::GlobalIndexRange>{range(0, 4), range(4, 8)}));
}

TEST_F(
    GraphExecutorFixture,
    BackgroundTransactionKeepsTickReplayBoundedToPositionalDemand)
{
    auto graph = replay_probe_graph();
    auto tock_states = initialize_tock_states(graph);
    iv::BackgroundCoverageState coverage(
        graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;

    iv::BackgroundEvaluationTransaction transaction{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 1,
            .roots = {
                .output_changes = {
                    {
                        .port = 0,
                        .coverage = index_coverage({{0, 1000}}),
                    },
                    {
                        .port = 1,
                        .coverage = index_coverage({{0, 1000}}),
                    },
                },
                .output_demands = {{
                    .port = 0,
                    .required = index_coverage({{123, 125}}),
                }},
            },
        }};
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);
    EXPECT_EQ(replay_probe_evaluate_calls, 1u);
    EXPECT_EQ(
        replay_probe_regions,
        (std::vector<iv::GlobalIndexRange>{range(123, 125)}));
}

TEST_F(
    GraphExecutorFixture,
    PersistedTickReplayPublishesOnlyTheDemandedPositionalRange)
{
    for (bool cache_computed : {false, true}) {
        SCOPED_TRACE(cache_computed);
        auto owner = std::make_shared<iv::CompiledGraph>(replay_probe_graph());
        auto& graph = *owner;
        auto const output_id = iv::graph_jit::StableOutputPortId{
            .node = {
                .graph = "persisted-replay-probe",
                .virtual_node = "source",
            },
            .kind = iv::PortKind::sample,
            .port_name = "output",
            .port_index = 0,
        };
        auto& output = graph.background_evaluation_plan.ports[0];
        output.persisted_tick_output = true;
        output.realization = iv::OutputRealization::buffered;
        output.stable_identity = output_id;
        output.sample_layout = iv::mono_interleaved_channel_layout;
        graph.background_evaluation_plan.storage.ports[0].storage =
            iv::graph_jit::PortStorageKind::persisted_data;
        graph.background_evaluation_plan.storage.ports[0].sample_layout =
            iv::mono_interleaved_channel_layout;

        auto tock_states = initialize_tock_states(graph);
        iv::BackgroundCoverageState coverage(
            graph.background_evaluation_plan.accumulators.output_change_count);
        iv::BackgroundPropagationWorkspace propagation{
            graph.background_evaluation_plan, graph.specialization.sample_rate};
        iv::PersistedDataStore data;

        iv::BackgroundEvaluationTransaction transaction{
            graph,
            tock_states,
            coverage,
            propagation,
            data,
            {
                .semantic_version = 1,
                .roots = {
                    .output_changes = {
                        {
                            .port = 0,
                            .coverage = index_coverage({{0, 1000}}),
                        },
                        {
                            .port = 1,
                            .coverage = index_coverage({{0, 1000}}),
                        },
                    },
                    .output_demands = {{
                        .port = 0,
                        .required = index_coverage({{123, 125}}),
                    }},
                },
                .reconstruction_graph = cache_computed ? owner : nullptr,
            }};
        auto result = transaction.execute();
        ASSERT_TRUE(result.has_value()) << result.error();
        ASSERT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);
        ASSERT_TRUE(result->published_data.has_value());
        EXPECT_EQ(
            replay_probe_regions,
            (std::vector<iv::GlobalIndexRange>{range(123, 125)}));

        auto reader = data.register_reader();
        auto pin = reader.pin();
        auto const output_handle = data.resolve_output(output_id);
        auto const* published = pin->find_sample_coverage(
            output_handle, iv::mono_interleaved_channel_layout);
        ASSERT_NE(published, nullptr);
        EXPECT_EQ(*published, index_coverage({{123, 125}}));
    }
}

TEST_F(
    GraphExecutorFixture,
    StalePersistedBackgroundEvaluationPromotesNeitherDataNorCoverage)
{
    auto graph = persisted_tock_graph();
    auto tock_states = initialize_tock_states(graph);
    iv::BackgroundCoverageState coverage(
        graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    auto const request = iv::BackgroundTransactionInputs{
        .semantic_version = 7,
        .roots = {
            .locally_changed_nodes = {0},
            .output_demands = {{
                .port = 0,
                .required = index_coverage({{0, 8}}),
            }},
        },
    };

    competing_persisted_store = &data;
    publish_competing_persisted_snapshot = true;
    iv::BackgroundEvaluationTransaction stale{
        graph, tock_states, coverage, propagation, data, request};
    auto stale_result = stale.execute();
    ASSERT_TRUE(stale_result.has_value()) << stale_result.error();
    EXPECT_EQ(stale_result->status, iv::BackgroundEvaluationStatus::stale_base);
    EXPECT_FALSE(stale_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
    {
        auto pin = reader.pin();
        ASSERT_TRUE(competing_persisted_version.has_value());
        EXPECT_EQ(competing_persisted_version->semantic, 99u);
        EXPECT_GT(competing_persisted_version->revision, 0u);
        EXPECT_EQ(pin->version(), *competing_persisted_version);
        EXPECT_EQ(pin->sample_region_count(), 0u);
    }

    // execute() has returned, so the rejected transaction must no longer pin
    // the retired base even while the transaction object itself remains alive.
    EXPECT_EQ(data.retired_snapshot_count(), 1u);
    EXPECT_EQ(data.reclaim_retired(), 1u);

    iv::BackgroundEvaluationTransaction demand_only{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 99,
            .roots = {
                .output_demands = {{
                    .port = 0,
                    .required = index_coverage({{0, 8}}),
                }},
            },
        }};
    auto demand_only_result = demand_only.execute();
    ASSERT_TRUE(demand_only_result.has_value()) << demand_only_result.error();
    EXPECT_FALSE(demand_only_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
}

TEST_F(
    GraphExecutorFixture,
    StaleDataFreeBackgroundEvaluationPromotesNoCoverage)
{
    auto graph = ephemeral_tock_graph();
    auto tock_states = initialize_tock_states(graph);
    iv::BackgroundCoverageState coverage(
        graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();

    competing_persisted_store = &data;
    publish_competing_persisted_snapshot = true;
    iv::BackgroundEvaluationTransaction stale{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 7,
            .roots = {
                .locally_changed_nodes = {0},
                .output_demands = {{
                    .port = 0,
                    .required = index_coverage({{0, 8}}),
                }},
            },
        }};
    auto stale_result = stale.execute();
    ASSERT_TRUE(stale_result.has_value()) << stale_result.error();
    EXPECT_EQ(stale_result->status, iv::BackgroundEvaluationStatus::stale_base);
    EXPECT_FALSE(stale_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
    {
        auto pin = reader.pin();
        ASSERT_TRUE(competing_persisted_version.has_value());
        EXPECT_EQ(competing_persisted_version->semantic, 99u);
        EXPECT_GT(competing_persisted_version->revision, 0u);
        EXPECT_EQ(pin->version(), *competing_persisted_version);
        EXPECT_EQ(pin->sample_region_count(), 0u);
    }

    EXPECT_EQ(data.retired_snapshot_count(), 1u);
    EXPECT_EQ(data.reclaim_retired(), 1u);

    iv::BackgroundEvaluationTransaction demand_only{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        {
            .semantic_version = 99,
            .roots = {
                .output_demands = {{
                    .port = 0,
                    .required = index_coverage({{0, 8}}),
                }},
            },
        }};
    auto demand_only_result = demand_only.execute();
    ASSERT_TRUE(demand_only_result.has_value())
        << demand_only_result.error();
    EXPECT_EQ(
        demand_only_result->status, iv::BackgroundEvaluationStatus::committed);
    EXPECT_FALSE(demand_only_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);
}

TEST_F(
    GraphExecutorFixture,
    RetainedDataFreeBackgroundEvaluationRejectsBackwardsSemanticVersion)
{
    auto persisted_graph = persisted_tock_graph();
    auto persisted_tock_states = initialize_tock_states(persisted_graph);
    iv::BackgroundCoverageState persisted_coverage(
        persisted_graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace persisted_propagation{
        persisted_graph.background_evaluation_plan,
        persisted_graph.specialization.sample_rate};
    iv::PersistedDataStore data;

    iv::BackgroundEvaluationTransaction seed{
        persisted_graph,
        persisted_tock_states,
        persisted_coverage,
        persisted_propagation,
        data,
        {
            .semantic_version = 7,
            .roots = {
                .locally_changed_nodes = {0},
                .output_demands = {{
                    .port = 0,
                    .required = index_coverage({{0, 8}}),
                }},
            },
        }};
    auto seed_result = seed.execute();
    ASSERT_TRUE(seed_result.has_value()) << seed_result.error();
    ASSERT_TRUE(seed_result->published_data.has_value());
    EXPECT_EQ(persisted_probe_evaluate_calls, 1u);

    auto ephemeral_graph = ephemeral_tock_graph();
    auto ephemeral_tock_states = initialize_tock_states(ephemeral_graph);
    iv::BackgroundCoverageState ephemeral_coverage(
        ephemeral_graph.background_evaluation_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace ephemeral_propagation{
        ephemeral_graph.background_evaluation_plan,
        ephemeral_graph.specialization.sample_rate};
    auto const demand = iv::CoveragePropagationRequest{
        .locally_changed_nodes = {0},
        .output_demands = {{
            .port = 0,
            .required = index_coverage({{0, 8}}),
        }},
    };

    iv::BackgroundEvaluationTransaction backwards_semantic{
        ephemeral_graph,
        ephemeral_tock_states,
        ephemeral_coverage,
        ephemeral_propagation,
        data,
        {
            .semantic_version = 6,
            .roots = demand,
        }};
    auto backwards_result = backwards_semantic.execute();
    ASSERT_FALSE(backwards_result.has_value());
    EXPECT_NE(
        backwards_result.error().find("semantic version cannot move backwards"),
        std::string::npos);

    ephemeral_graph.specialization.block_size = 8;
    iv::BackgroundEvaluationTransaction different_block_size{
        ephemeral_graph,
        ephemeral_tock_states,
        ephemeral_coverage,
        ephemeral_propagation,
        data,
        {
            .semantic_version = 7,
            .roots = demand,
        }};
    auto different_block_size_result = different_block_size.execute();
    ASSERT_TRUE(different_block_size_result.has_value())
        << different_block_size_result.error();
    EXPECT_EQ(different_block_size_result->status,
        iv::BackgroundEvaluationStatus::committed);
    EXPECT_FALSE(different_block_size_result->published_data.has_value());

    // The backwards semantic version is rejected before evaluation. Persisted
    // regions are independent of callback block size, so the second data-free
    // transaction evaluates and commits coverage normally.
    EXPECT_EQ(persisted_probe_evaluate_calls, 2u);
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    SampleReverseCallbackSeesTheSelectedEqualRangeRegion)
{
    auto const plan = probe_tock_plan(
        iv::OutputRealization::computed, iv::graph_jit::PortStorageKind::background);
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const region = range(10, 30);
    iv::CompiledGraphBackgroundOperations const operations{
        .propagate_forward = &no_op_background_evaluate,
        .propagate_reverse = +[](iv::graph_jit::BackgroundEvaluationCall* batch) {
            auto const nodes = static_cast<std::span<iv::graph_jit::BackgroundNodeCall>>(batch->nodes);
            ASSERT_EQ(nodes.size(), 1u);
            auto const outputs = static_cast<std::span<iv::OutputCoverageRequirement const>>(nodes.front().reverse.outputs);
            ASSERT_EQ(outputs.size(), 1u);
            iv::details::StaticOutputCoverageRequirement<
                iv::OutputProductionKind::coverage, iv::PortKind::sample> const output{outputs.front()};
            auto const selections = output.required_selections();
            ASSERT_EQ(selections.size(), 1u);
            EXPECT_TRUE(selections.front().region);
            EXPECT_EQ(iv::details::RegionRefAccess::index(selections.front().region), 1u);
            ASSERT_EQ(selections.front().runs.size(), 1u);
            EXPECT_EQ(selections.front().runs.front(), (iv::IndexRun{1, 3, 4}));
        },
        .evaluate = &no_op_background_evaluate,
    };
    auto prepared = workspace.prepare(operations, coverage, {
        .output_changes = {{.port = 0, .coverage = iv::IndexCoverage{region}, .regions = {region, region}}},
        .indexed_output_demands = {{.port = 0, .region_index = 1, .runs = {{1, 3, 4}}}},
    });
    EXPECT_TRUE(iv::graph_jit::has_activity(prepared.node_activity().front(),
        iv::graph_jit::BackgroundNodeActivity::reverse));
    workspace.discard(std::move(prepared));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    PreparedCoverageExposesEvaluationActivityWithoutCommitting)
{
    auto const plan = background_fanin_plan();
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };

    auto prepared = workspace.prepare(
        operations,
        coverage,
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {0, 1},
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = index_coverage({{11, 21}}),
                },
            },
        });

    EXPECT_EQ(prepared.result().output_changes.size(), 3u);
    EXPECT_EQ(prepared.result().input_requirements.size(), 1u);
    EXPECT_EQ(prepared.result().output_requirements.size(), 3u);
    ASSERT_EQ(prepared.node_activity().size(), 3u);
    for (auto const activity : prepared.node_activity()) {
        EXPECT_TRUE(iv::graph_jit::has_activity(
            activity, iv::graph_jit::BackgroundNodeActivity::forward));
        EXPECT_TRUE(iv::graph_jit::has_activity(
            activity, iv::graph_jit::BackgroundNodeActivity::reverse));
        EXPECT_TRUE(iv::graph_jit::has_activity(
            activity, iv::graph_jit::BackgroundNodeActivity::evaluate));
    }

    workspace.discard(std::move(prepared));

    background_observation = {};
    auto retry = workspace.prepare(
        operations,
        coverage,
        iv::CoveragePropagationRequest{
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = index_coverage({{10, 22}}),
                },
            },
        });
    EXPECT_TRUE(retry.result().output_requirements.empty());
    for (auto const activity : retry.node_activity()) {
        EXPECT_EQ(activity, iv::graph_jit::BackgroundNodeActivity::none);
    }
    workspace.discard(std::move(retry));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    BackgroundPropagationAccumulatesFaninAndReverseDemandOncePerNode)
{
    auto const plan = background_fanin_plan();
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };

    auto prepared = workspace.prepare(
        operations,
        coverage,
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {0, 1},
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = index_coverage({{11, 21}}),
                },
            },
        });
    auto const result = workspace.commit(coverage, std::move(prepared));

    EXPECT_EQ(background_observation.source_forward_calls[0], 1u);
    EXPECT_EQ(background_observation.source_forward_calls[1], 1u);
    EXPECT_EQ(background_observation.sink_forward_calls, 1u);
    EXPECT_EQ(
        background_observation.sink_input_coverage,
        index_coverage({{10, 13}, {20, 22}}));
    EXPECT_EQ(
        background_observation.sink_input_changed,
        background_observation.sink_input_coverage);
    EXPECT_EQ(background_observation.sink_reverse_calls, 1u);
    EXPECT_EQ(
        background_observation.sink_output_required,
        index_coverage({{11, 13}, {20, 21}}));

    ASSERT_EQ(result.output_changes.size(), 3u);
    EXPECT_EQ(
        result.output_changes[2].coverage,
        index_coverage({{10, 13}, {20, 22}}));
    ASSERT_EQ(result.input_requirements.size(), 1u);
    EXPECT_EQ(result.input_requirements[0].port, 2u);
    EXPECT_EQ(
        result.input_requirements[0].required,
        index_coverage({{11, 13}, {20, 21}}));
    ASSERT_EQ(result.output_requirements.size(), 3u);
    EXPECT_EQ(result.output_requirements[0].port, 0u);
    EXPECT_EQ(
        result.output_requirements[0].required,
        index_coverage({{11, 13}}));
    EXPECT_EQ(result.output_requirements[1].port, 1u);
    EXPECT_EQ(
        result.output_requirements[1].required,
        index_coverage({{20, 21}}));
    EXPECT_EQ(result.output_requirements[2].port, 3u);
    EXPECT_EQ(
        result.output_requirements[2].required,
        index_coverage({{11, 13}, {20, 21}}));

    background_observation = {};
    prepared = workspace.prepare(
        operations,
        coverage,
        iv::CoveragePropagationRequest{
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = index_coverage({{10, 22}}),
                },
            },
        });
    auto const demand_only = workspace.commit(
        coverage, std::move(prepared));
    EXPECT_EQ(background_observation.sink_forward_calls, 0u);
    EXPECT_EQ(background_observation.sink_reverse_calls, 1u);
    ASSERT_EQ(demand_only.output_requirements.size(), 3u);

    background_observation = {};
    prepared = workspace.prepare(
        operations,
        coverage,
        iv::CoveragePropagationRequest{
            .input_demands = {
                iv::InputCoverageRequest{
                    .port = 2,
                    .required = index_coverage({{10, 22}}),
                },
            },
        });
    auto const propagation_result = workspace.commit(
        coverage, std::move(prepared));
    EXPECT_EQ(background_observation.sink_reverse_calls, 0u);
    ASSERT_EQ(propagation_result.input_requirements.size(), 1u);
    EXPECT_EQ(propagation_result.input_requirements[0].port, 2u);
    ASSERT_EQ(propagation_result.output_requirements.size(), 2u);
    EXPECT_EQ(propagation_result.output_requirements[0].port, 0u);
    EXPECT_EQ(propagation_result.output_requirements[1].port, 1u);
}

TEST_F(BackgroundCoveragePropagationFixture,
       NormalizedSampleProjectionsPreserveSeparateRegionContributions)
{
    auto plan = background_fanin_plan();
    plan.ports[0].outgoing_connections = {0};
    plan.ports[1].outgoing_connections = {0};
    plan.ports[2].incoming_connections = {0};
    plan.connections = {{
        .kind = iv::PortKind::sample,
        .source_coverage_ports = {0, 1},
        .target_coverage_ports = {2},
        .sample_source_port_by_channel = {
            iv::graph_jit::BackgroundPortIndex{0},
            iv::graph_jit::BackgroundPortIndex{1},
        },
        .sample_read_latencies = {0, 0},
        .sample_target_port = 2,
        .sample_projections = {
            {
                .source_channel_indices = {0},
                .target_channels = {0},
            },
            {
                .source_channel_indices = {1},
                .target_channels = {0},
            },
        },
    }};

    iv::BackgroundCoverageState coverage(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto prepared = workspace.prepare(
        {
            .propagate_forward = &propagate_background_forward,
            .propagate_reverse = &propagate_background_reverse,
            .evaluate = &no_op_background_evaluate,
        },
        coverage,
        {.locally_changed_nodes = {0, 1}});

    EXPECT_EQ(
        std::ranges::to<std::vector>(
            prepared.input_regions(2) | std::views::transform(&iv::BackgroundInputRegion::range)),
        (std::vector<iv::GlobalIndexRange>{
            range(10, 13),
            range(20, 22),
        }));
    auto const regions = prepared.input_regions(2);
    ASSERT_EQ(regions.size(), 2u);
    EXPECT_EQ(regions[0].sample_projection, 0u);
    EXPECT_EQ(regions[1].sample_projection, 1u);
    EXPECT_EQ(regions[0].sources, (std::vector<iv::BackgroundInputRegionSource>{
        {.output = 0, .region_index = 0, .route_index = 0}}));
    EXPECT_EQ(regions[1].sources, (std::vector<iv::BackgroundInputRegionSource>{
        {.output = 1, .region_index = 0, .route_index = 1}}));
    EXPECT_EQ(
        background_observation.sink_input_coverage,
        index_coverage({{10, 13}, {20, 22}}));
    workspace.discard(std::move(prepared));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    StableOutputCoverageMigrationReadsThePredecessorAfterItsFinalDrain)
{
    auto previous_plan = background_fanin_plan();
    auto stable_output = [](std::string virtual_node) {
        return iv::graph_jit::StableOutputPortId{
            .node = {
                .graph = "coverage-migration",
                .virtual_node = std::move(virtual_node),
            },
            .kind = iv::PortKind::sample,
            .port_name = "output",
            .port_index = 0,
        };
    };
    previous_plan.ports[0].stable_identity = stable_output("source-a");
    previous_plan.ports[1].stable_identity = stable_output("source-b");
    previous_plan.ports[3].stable_identity = stable_output("sink");
    auto current_plan = previous_plan;

    iv::BackgroundCoverageState previous(
        previous_plan.accumulators.output_change_count);
    iv::BackgroundCoverageState current(
        current_plan.accumulators.output_change_count);
    // Preparation happens while the predecessor is still live. The descriptor
    // must not snapshot its mutable coverage at this point.
    auto migration = current.prepare_migration_from(
        previous, current_plan, previous_plan);

    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };
    iv::BackgroundPropagationWorkspace previous_workspace(
        previous_plan, 48000);
    auto drained = previous_workspace.prepare(
        operations,
        previous,
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {0, 1},
        });
    auto const final_drain = previous_workspace.commit(
        previous, std::move(drained));
    (void)final_drain;
    migration.commit();

    background_observation = {};
    iv::BackgroundPropagationWorkspace current_workspace(current_plan, 48000);
    auto reconciled = current_workspace.prepare(
        operations,
        current,
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {2},
        });
    EXPECT_EQ(
        background_observation.sink_input_coverage,
        index_coverage({{10, 13}, {20, 22}}));
    current_workspace.discard(std::move(reconciled));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    CoverageMigrationDoesNotTransferEventRegionsAcrossProductionKinds)
{
    auto previous_plan = probe_tock_plan(
        iv::OutputRealization::computed,
        iv::graph_jit::PortStorageKind::background);
    auto current_plan = previous_plan;
    auto const stable_output = iv::graph_jit::StableOutputPortId{
        .node = {
            .graph = "event-production-migration",
            .virtual_node = "source",
        },
        .kind = iv::PortKind::event,
        .port_name = "output",
        .port_index = 0,
    };
    for (auto* plan : {&previous_plan, &current_plan}) {
        plan->ports[0].configured_port.port_kind = iv::PortKind::event;
        plan->ports[0].kind = iv::PortKind::event;
        plan->ports[0].event_type = iv::EventTypeId::trigger;
        plan->ports[0].stable_identity = stable_output;
    }
    previous_plan.ports[0].output_production =
        iv::OutputProductionKind::coverage;
    current_plan.ports[0].output_production =
        iv::OutputProductionKind::region;

    iv::BackgroundCoverageState previous(
        previous_plan.accumulators.output_change_count);
    iv::BackgroundCoverageState current(
        current_plan.accumulators.output_change_count);
    auto migration = current.prepare_migration_from(
        previous, current_plan, previous_plan);

    EXPECT_TRUE(migration.outputs.empty());
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    CoverageMigrationSeedsOnlyNodesWhoseLocalSemanticsChanged)
{
    auto previous_plan = background_fanin_plan();
    auto current_plan = previous_plan;
    auto stable_node = [](std::string id) {
        return iv::graph_jit::StableConcreteNodeId{
            .graph = "semantic-reconciliation",
            .virtual_node = std::move(id),
        };
    };
    auto unchanged_source = std::make_shared<int>(1);
    auto changed_source_before = std::make_shared<int>(2);
    auto changed_source_after = std::make_shared<int>(3);
    auto unchanged_sink = std::make_shared<int>(4);
    for (std::size_t node = 0; node < previous_plan.nodes.size(); ++node) {
        previous_plan.nodes[node].stable_identity = stable_node(
            "node-" + std::to_string(node));
        current_plan.nodes[node].stable_identity =
            previous_plan.nodes[node].stable_identity;
        previous_plan.nodes[node].provider_version = 7;
        current_plan.nodes[node].provider_version = 7;
    }
    previous_plan.nodes[0].configuration_identity = unchanged_source;
    current_plan.nodes[0].configuration_identity = unchanged_source;
    previous_plan.nodes[1].configuration_identity = changed_source_before;
    current_plan.nodes[1].configuration_identity = changed_source_after;
    previous_plan.nodes[2].configuration_identity = unchanged_sink;
    current_plan.nodes[2].configuration_identity = unchanged_sink;
    auto stable_output = [&](std::size_t node) {
        return iv::graph_jit::StableOutputPortId{
            .node = stable_node("node-" + std::to_string(node)),
            .kind = iv::PortKind::sample,
            .port_name = "output",
            .port_index = 0,
        };
    };
    for (std::size_t output : {0u, 1u}) {
        previous_plan.ports[output].stable_identity = stable_output(output);
        current_plan.ports[output].stable_identity = stable_output(output);
    }
    previous_plan.ports[3].stable_identity = stable_output(2);
    current_plan.ports[3].stable_identity = stable_output(2);
    auto const stable_input = iv::graph_jit::StableInputPortId{
        .node = stable_node("node-2"),
        .kind = iv::PortKind::sample,
        .port_name = "input",
        .port_index = 0,
    };
    previous_plan.ports[2].stable_input_identity = stable_input;
    current_plan.ports[2].stable_input_identity = stable_input;
    auto complete_fanin_connections = [](auto& plan) {
        for (std::size_t source = 0; source < 2; ++source) {
            auto& connection = plan.connections[source];
            connection.sample_source_type = iv::ChannelTypeId::mono;
            connection.sample_target_type = iv::ChannelTypeId::mono;
            connection.sample_source_channels = {{source, 0, 0}};
            connection.sample_source_port_by_channel = {source};
            connection.sample_read_latencies = {0};
            connection.sample_deliveries = {
                iv::graph_jit::PlannedDeliveryMechanism::tock_to_random_access,
            };
            connection.sample_target_channels = {{2, 0, 0}};
            connection.sample_target_port = 2;
        }
    };
    complete_fanin_connections(previous_plan);
    complete_fanin_connections(current_plan);

    iv::BackgroundCoverageState previous(
        previous_plan.accumulators.output_change_count);
    iv::BackgroundCoverageState current(
        current_plan.accumulators.output_change_count);
    auto migration = current.prepare_migration_from(
        previous, current_plan, previous_plan);
    migration.commit();
    auto roots = migration.take_reconciliation_roots();

    ASSERT_EQ(roots.locally_changed_nodes.size(), 1u);
    EXPECT_EQ(roots.locally_changed_nodes.front(), 1u);
    EXPECT_TRUE(roots.input_changes.empty());
}

TEST_F(BackgroundCoveragePropagationFixture, ModuleInputResolutionKeepsItsSemanticIdentityAcrossBundleMoves)
{
    using namespace iv;
    using namespace iv::graph_jit;
    StableInputPortId origin{
        .node = {.graph = "public-inputs", .virtual_node = "module"},
        .kind = PortKind::sample, .port_name = "gain", .port_index = 3,
    };
    BackgroundEvaluationPlan previous_plan;
    previous_plan.nodes = {{.bundle = 7, .resolved_module_input = origin,
        .replays_tick = true, .full_live_tick_coverage = true,
        .replay_mode = TickReplayMode::full, .inputs = {0}, .outputs = {1}}};
    previous_plan.ports = {
        {.node = 0, .configured_port = {7, PortKind::sample, 0},
         .direction = PortDirection::input, .sample_fallback_value = 0.5f,
         .stable_input_identity = origin,
         .accumulators = {.input_change = 0, .input_requirement = 0}},
        {.node = 0, .configured_port = {7, PortKind::sample, 0},
         .direction = PortDirection::output, .full_live_tick_coverage = true,
         .resolved_module_input = origin,
         .accumulators = {.output_change = 0, .output_requirement = 0}},
    };
    previous_plan.accumulators = {.input_change_count = 1, .output_change_count = 1,
        .output_requirement_count = 1, .input_requirement_count = 1};
    auto current_plan = previous_plan;
    current_plan.nodes[0].bundle = 12;
    for (auto& port : current_plan.ports) port.configured_port.node_bundle_handle = 12;
    BackgroundCoverageState previous(1), current(1);
    auto unchanged = current.prepare_migration_from(previous, current_plan, previous_plan);
    ASSERT_EQ(unchanged.outputs.size(), 1u);
    unchanged.commit();
    auto roots = unchanged.take_reconciliation_roots();
    EXPECT_TRUE(roots.locally_changed_nodes.empty());
    EXPECT_TRUE(roots.input_changes.empty());
    current_plan.ports[0].sample_fallback_value = 0.75f;
    BackgroundCoverageState changed(1);
    auto updated = changed.prepare_migration_from(previous, current_plan, previous_plan);
    updated.commit();
    roots = updated.take_reconciliation_roots();
    EXPECT_EQ(roots.locally_changed_nodes, (std::vector<BackgroundNodeIndex>{0}));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    CoverageMigrationTreatsSampleRateChangeAsAWholePlanSemanticChange)
{
    auto previous_plan = background_fanin_plan();
    auto current_plan = previous_plan;
    std::array semantic_owners{
        std::make_shared<int>(1),
        std::make_shared<int>(2),
        std::make_shared<int>(3),
    };
    for (std::size_t node = 0; node < previous_plan.nodes.size(); ++node) {
        auto const stable = iv::graph_jit::StableConcreteNodeId{
            .graph = "sample-rate-reconciliation",
            .virtual_node = "node-" + std::to_string(node),
        };
        previous_plan.nodes[node].stable_identity = stable;
        current_plan.nodes[node].stable_identity = stable;
        previous_plan.nodes[node].provider_version = 11;
        current_plan.nodes[node].provider_version = 11;
        previous_plan.nodes[node].configuration_identity = semantic_owners[node];
        current_plan.nodes[node].configuration_identity = semantic_owners[node];
    }

    iv::BackgroundCoverageState previous(
        previous_plan.accumulators.output_change_count);
    iv::BackgroundCoverageState current(
        current_plan.accumulators.output_change_count);
    auto migration = current.prepare_migration_from(
        previous, current_plan, previous_plan, true);
    migration.commit();
    auto roots = migration.take_reconciliation_roots();

    EXPECT_EQ(
        roots.locally_changed_nodes,
        (std::vector<iv::graph_jit::BackgroundNodeIndex>{0, 1, 2}));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    CoverageMigrationTurnsStableInputRewiringIntoAnExactInputChangeRoot)
{
    auto previous_plan = background_fanin_plan();
    auto current_plan = previous_plan;
    auto stable_node = [](std::string id) {
        return iv::graph_jit::StableConcreteNodeId{
            .graph = "connection-reconciliation",
            .virtual_node = std::move(id),
        };
    };
    auto stable_output = [&](std::size_t node) {
        return iv::graph_jit::StableOutputPortId{
            .node = stable_node("node-" + std::to_string(node)),
            .kind = iv::PortKind::sample,
            .port_name = "output",
            .port_index = 0,
        };
    };
    std::array semantic_owners{
        std::make_shared<int>(1),
        std::make_shared<int>(2),
        std::make_shared<int>(3),
    };
    for (std::size_t node = 0; node < previous_plan.nodes.size(); ++node) {
        previous_plan.nodes[node].stable_identity = stable_node(
            "node-" + std::to_string(node));
        current_plan.nodes[node].stable_identity =
            previous_plan.nodes[node].stable_identity;
        previous_plan.nodes[node].provider_version = 9;
        current_plan.nodes[node].provider_version = 9;
        previous_plan.nodes[node].configuration_identity = semantic_owners[node];
        current_plan.nodes[node].configuration_identity = semantic_owners[node];
    }
    for (std::size_t output : {0u, 1u}) {
        previous_plan.ports[output].stable_identity = stable_output(output);
        current_plan.ports[output].stable_identity = stable_output(output);
    }
    previous_plan.ports[3].stable_identity = stable_output(2);
    current_plan.ports[3].stable_identity = stable_output(2);
    auto const stable_input = iv::graph_jit::StableInputPortId{
        .node = stable_node("node-2"),
        .kind = iv::PortKind::sample,
        .port_name = "input",
        .port_index = 0,
    };
    previous_plan.ports[2].stable_input_identity = stable_input;
    current_plan.ports[2].stable_input_identity = stable_input;

    auto connect_source = [](iv::graph_jit::BackgroundEvaluationPlan& plan,
                             std::size_t source) {
        plan.connections.resize(1);
        plan.connections[0] = iv::graph_jit::BackgroundConnectionPlan{
            .kind = iv::PortKind::sample,
            .source_coverage_ports = {source},
            .target_coverage_ports = {2},
            .sample_source_type = iv::ChannelTypeId::mono,
            .sample_target_type = iv::ChannelTypeId::mono,
            .sample_source_channels = {{source, 0, 0}},
            .sample_source_port_by_channel = {source},
            .sample_read_latencies = {0},
            .sample_deliveries = {
                iv::graph_jit::PlannedDeliveryMechanism::tock_to_random_access},
            .sample_target_channels = {{2, 0, 0}},
            .sample_target_port = 2,
        };
        plan.ports[0].outgoing_connections.clear();
        plan.ports[1].outgoing_connections.clear();
        plan.ports[source].outgoing_connections = {0};
        plan.ports[2].incoming_connections = {0};
    };
    connect_source(previous_plan, 0);
    connect_source(current_plan, 1);

    iv::BackgroundCoverageState previous(
        previous_plan.accumulators.output_change_count);
    iv::BackgroundCoverageState current(
        current_plan.accumulators.output_change_count);
    auto migration = current.prepare_migration_from(
        previous, current_plan, previous_plan);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };
    iv::BackgroundPropagationWorkspace previous_workspace(
        previous_plan, 48000);
    auto populated = previous_workspace.prepare(
        operations,
        previous,
        iv::CoveragePropagationRequest{.locally_changed_nodes = {0, 1}});
    (void)previous_workspace.commit(previous, std::move(populated));

    migration.commit();
    auto roots = migration.take_reconciliation_roots();
    EXPECT_TRUE(roots.locally_changed_nodes.empty());
    ASSERT_EQ(roots.input_changes.size(), 1u);
    EXPECT_EQ(roots.input_changes.front().port, 2u);
    EXPECT_EQ(
        roots.input_changes.front().coverage,
        index_coverage({{20, 22}}));
    EXPECT_EQ(
        roots.input_changes.front().regions,
        (std::vector<iv::GlobalIndexRange>{range(20, 22)}));
    EXPECT_EQ(
        roots.input_changes.front().changed,
        index_coverage({{10, 13}, {20, 22}}));

    background_observation = {};
    iv::BackgroundPropagationWorkspace current_workspace(current_plan, 48000);
    auto reconciled = current_workspace.prepare(
        operations, current, roots);
    EXPECT_EQ(
        background_observation.sink_input_coverage,
        index_coverage({{20, 22}}));
    EXPECT_EQ(
        background_observation.sink_input_changed,
        index_coverage({{10, 13}, {20, 22}}));
    current_workspace.discard(std::move(reconciled));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    CoverageMigrationTurnsReadLatencyChangeIntoAnExactInputChangeRoot)
{
    auto previous_plan = background_fanin_plan();
    auto current_plan = previous_plan;
    auto const stable_source = iv::graph_jit::StableOutputPortId{
        .node = {
            .graph = "read-latency-reconciliation",
            .virtual_node = "source",
        },
        .kind = iv::PortKind::sample,
        .port_name = "output",
        .port_index = 0,
    };
    auto const stable_input = iv::graph_jit::StableInputPortId{
        .node = {
            .graph = "read-latency-reconciliation",
            .virtual_node = "sink",
        },
        .kind = iv::PortKind::sample,
        .port_name = "input",
        .port_index = 0,
    };
    std::array semantic_owners{
        std::make_shared<int>(1),
        std::make_shared<int>(2),
        std::make_shared<int>(3),
    };
    std::array stable_node_names{
        "source",
        "unused-source",
        "sink",
    };
    for (std::size_t node = 0; node < previous_plan.nodes.size(); ++node) {
        auto const stable_node = iv::graph_jit::StableConcreteNodeId{
            .graph = "read-latency-reconciliation",
            .virtual_node = std::string{stable_node_names[node]},
        };
        previous_plan.nodes[node].stable_identity = stable_node;
        current_plan.nodes[node].stable_identity = stable_node;
        previous_plan.nodes[node].provider_version = 1;
        current_plan.nodes[node].provider_version = 1;
        previous_plan.nodes[node].configuration_identity = semantic_owners[node];
        current_plan.nodes[node].configuration_identity = semantic_owners[node];
    }
    auto stable_output = [&](std::string virtual_node) {
        return iv::graph_jit::StableOutputPortId{
            .node = {
                .graph = "read-latency-reconciliation",
                .virtual_node = std::move(virtual_node),
            },
            .kind = iv::PortKind::sample,
            .port_name = "output",
            .port_index = 0,
        };
    };
    for (auto* plan : {&previous_plan, &current_plan}) {
        plan->ports[0].stable_identity = stable_source;
        plan->ports[1].stable_identity = stable_output("unused-source");
        plan->ports[3].stable_identity = stable_output("sink");
        plan->ports[2].stable_input_identity = stable_input;
    }
    auto connect = [&](iv::graph_jit::BackgroundEvaluationPlan& plan,
                       std::size_t read_latency) {
        plan.ports[0].outgoing_connections = {0};
        plan.ports[1].outgoing_connections.clear();
        plan.ports[2].incoming_connections = {0};
        plan.connections.resize(1);
        plan.connections[0] = iv::graph_jit::BackgroundConnectionPlan{
            .kind = iv::PortKind::sample,
            .source_coverage_ports = {0},
            .target_coverage_ports = {2},
            .sample_source_type = iv::ChannelTypeId::mono,
            .sample_target_type = iv::ChannelTypeId::mono,
            .sample_source_channels = {{0, 0, 0}},
            .sample_source_port_by_channel = {0},
            .sample_read_latencies = {read_latency},
            .sample_deliveries = {
                iv::graph_jit::PlannedDeliveryMechanism::
                    tock_to_random_access},
            .sample_target_channels = {{2, 0, 0}},
            .sample_target_port = 2,
        };
    };
    connect(previous_plan, 0);
    connect(current_plan, 5);

    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };
    iv::BackgroundCoverageState previous(
        previous_plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace previous_workspace(
        previous_plan, 48000);
    auto populated = previous_workspace.prepare(
        operations,
        previous,
        iv::CoveragePropagationRequest{.locally_changed_nodes = {0}});
    (void)previous_workspace.commit(previous, std::move(populated));

    iv::BackgroundCoverageState current(
        current_plan.accumulators.output_change_count);
    auto migration = current.prepare_migration_from(
        previous, current_plan, previous_plan);
    migration.commit();
    auto roots = migration.take_reconciliation_roots();

    EXPECT_TRUE(roots.locally_changed_nodes.empty());
    ASSERT_EQ(roots.input_changes.size(), 1u);
    EXPECT_EQ(roots.input_changes.front().port, 2u);
    EXPECT_EQ(
        roots.input_changes.front().coverage,
        index_coverage({{15, 18}}));
    EXPECT_EQ(
        roots.input_changes.front().regions,
        (std::vector<iv::GlobalIndexRange>{range(15, 18)}));
    EXPECT_EQ(
        roots.input_changes.front().changed,
        index_coverage({{10, 13}, {15, 18}}));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    FailedForwardPreparationLeavesCommittedCoverageUnchanged)
{
    auto const plan = background_fanin_plan();
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };

    background_observation.throw_from_sink = true;
    EXPECT_THROW(
        static_cast<void>(workspace.prepare(
            operations,
            coverage,
            iv::CoveragePropagationRequest{
                .locally_changed_nodes = {0, 1},
            })),
        std::runtime_error);

    background_observation = {};
    auto prepared = workspace.prepare(
        operations,
        coverage,
        iv::CoveragePropagationRequest{
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = index_coverage({{10, 22}}),
                },
            },
        });
    auto const& result = prepared.result();
    EXPECT_TRUE(result.output_requirements.empty());
    EXPECT_EQ(background_observation.sink_reverse_calls, 0u);
    workspace.discard(std::move(prepared));
}

TEST_F(BackgroundCoveragePropagationFixture,
       ContinuousOutputPreservesOverlappingAndDuplicateRegionShape)
{
    auto plan = probe_tock_plan(
        iv::OutputRealization::computed,
        iv::graph_jit::PortStorageKind::background);
    iv::BackgroundCoverageState coverage(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_region_shape,
        .propagate_reverse = &no_op_background_evaluate,
        .evaluate = &no_op_background_evaluate,
    };
    propagated_region_shape = {
        range(10, 13),
        range(11, 14),
        range(10, 13),
    };

    auto prepared = workspace.prepare(
        operations,
        coverage,
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {0},
        });
    EXPECT_EQ(
        std::vector<iv::GlobalIndexRange>(
            prepared.output_regions(0).begin(),
            prepared.output_regions(0).end()),
        propagated_region_shape);
    ASSERT_EQ(prepared.result().output_changes.size(), 1u);
    EXPECT_EQ(
        prepared.result().output_changes[0].coverage,
        index_coverage({{10, 14}}));
    EXPECT_EQ(
        prepared.result().output_changes[0].changed,
        index_coverage({{10, 14}}));
    (void)workspace.commit(coverage, std::move(prepared));

    propagated_region_shape = {range(10, 14)};
    auto reshaped = workspace.prepare(
        operations,
        coverage,
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {0},
        });
    ASSERT_EQ(reshaped.result().output_changes.size(), 1u);
    EXPECT_EQ(
        reshaped.result().output_changes[0].coverage,
        index_coverage({{10, 14}}));
    EXPECT_EQ(
        reshaped.result().output_changes[0].changed,
        index_coverage({{10, 14}}));
    workspace.discard(std::move(reshaped));
}

TEST_F(BackgroundCoveragePropagationFixture,
       RegionOutputRequiresExactlyOnePublishedRegion)
{
    auto plan = probe_tock_plan(
        iv::OutputRealization::computed,
        iv::graph_jit::PortStorageKind::background);
    plan.ports[0].output_production = iv::OutputProductionKind::region;
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_region_shape,
        .propagate_reverse = &no_op_background_evaluate,
        .evaluate = &no_op_background_evaluate,
    };
    auto rejects = [&](std::vector<iv::GlobalIndexRange> regions) {
        propagated_region_shape = std::move(regions);
        iv::BackgroundCoverageState coverage(
            plan.accumulators.output_change_count);
        iv::BackgroundPropagationWorkspace workspace(plan, 48000);
        EXPECT_THROW(
            (void)workspace.prepare(
                operations,
                coverage,
                {.locally_changed_nodes = {0}}),
            std::invalid_argument);
    };

    rejects({});
    rejects({range(0, 1), range(2, 3)});

    propagated_region_shape = {range(0, 1)};
    iv::BackgroundCoverageState coverage(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto prepared = workspace.prepare(
        operations,
        coverage,
        {.locally_changed_nodes = {0}});
    ASSERT_EQ(prepared.output_regions(0).size(), 1u);
    EXPECT_EQ(prepared.output_regions(0).front(), range(0, 1));
    workspace.discard(std::move(prepared));
}

TEST_F(BackgroundCoveragePropagationFixture,
       CoverageOutputMayPublishAnEmptyRegionList)
{
    auto const plan = probe_tock_plan(
        iv::OutputRealization::computed,
        iv::graph_jit::PortStorageKind::background);
    propagated_region_shape.clear();
    iv::BackgroundCoverageState coverage(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto prepared = workspace.prepare(
        {
            .propagate_forward = &propagate_region_shape,
            .propagate_reverse = &no_op_background_evaluate,
            .evaluate = &no_op_background_evaluate,
        },
        coverage,
        {.locally_changed_nodes = {0}});
    EXPECT_TRUE(prepared.output_regions(0).empty());
    workspace.discard(std::move(prepared));
}

TEST_F(BackgroundCoveragePropagationFixture,
       TickRecordingDerivesProducedRegionsFromCanonicalIndexCoverage)
{
    auto plan = probe_tock_plan(
        iv::OutputRealization::buffered,
        iv::graph_jit::PortStorageKind::persisted_data);
    auto& output = plan.ports[0];
    output.output_production = iv::OutputProductionKind::sequential;
    output.authored_tock_output = false;
    output.persisted_tick_output = true;

    iv::BackgroundCoverageState coverage(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto prepared = workspace.prepare(
        {
            .propagate_forward = &no_op_background_evaluate,
            .propagate_reverse = &no_op_background_evaluate,
            .evaluate = &no_op_background_evaluate,
        },
        coverage,
        {.output_changes = {{
            .port = 0,
            .coverage = index_coverage({
                {10, 13},
                {13, 16},
                {15, 18},
                {30, 31},
            }),
        }}});

    EXPECT_EQ(
        std::vector<iv::GlobalIndexRange>(
            prepared.output_regions(0).begin(),
            prepared.output_regions(0).end()),
        (std::vector<iv::GlobalIndexRange>{
            range(10, 18),
            range(30, 31),
        }));
    workspace.discard(std::move(prepared));
}

TEST_F(BackgroundCoveragePropagationFixture,
       ContinuousOutputRejectsPositionalCoveragePublication)
{
    auto const plan = probe_tock_plan(
        iv::OutputRealization::computed,
        iv::graph_jit::PortStorageKind::background);
    iv::BackgroundCoverageState coverage(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    EXPECT_THROW(
        (void)workspace.prepare(
            {
                .propagate_forward = &propagate_positional_continuous_shape,
                .propagate_reverse = &no_op_background_evaluate,
                .evaluate = &no_op_background_evaluate,
            },
            coverage,
            {.locally_changed_nodes = {0}}),
        std::invalid_argument);
}

TEST_F(BackgroundCoveragePropagationFixture,
       SortedContinuousOutputRejectsNoncanonicalEqualBeginOrder)
{
    auto plan = probe_tock_plan(
        iv::OutputRealization::computed,
        iv::graph_jit::PortStorageKind::background);
    plan.ports[0].ordering = iv::CoverageOrdering::sorted;
    iv::BackgroundCoverageState coverage(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_region_shape,
        .propagate_reverse = &no_op_background_evaluate,
        .evaluate = &no_op_background_evaluate,
    };
    propagated_region_shape = {
        range(10, 12),
        range(10, 14),
    };

    EXPECT_THROW(
        (void)workspace.prepare(
            operations,
            coverage,
            iv::CoveragePropagationRequest{
                .locally_changed_nodes = {0},
            }),
        std::invalid_argument);
}

TEST_F(BackgroundCoveragePropagationFixture,
       SortedCoverageInputKWayMergesProducerRegionLists)
{
    using namespace iv::graph_jit;
    BackgroundEvaluationPlan plan;
    plan.nodes = {
        {
            .bundle = 0,
            .authored_tock_execution = true,
            .outputs = {0},
            .accumulators = {
                .output_change_begin = 0,
                .output_change_count = 1,
                .output_requirement_begin = 0,
                .output_requirement_count = 1,
            },
        },
        {
            .bundle = 1,
            .authored_tock_execution = true,
            .outputs = {1},
            .accumulators = {
                .output_change_begin = 1,
                .output_change_count = 1,
                .output_requirement_begin = 1,
                .output_requirement_count = 1,
            },
        },
        {
            .bundle = 2,
            .authored_tock_execution = true,
            .inputs = {2},
            .accumulators = {
                .input_change_begin = 0,
                .input_change_count = 1,
                .input_requirement_begin = 0,
                .input_requirement_count = 1,
            },
        },
    };
    plan.ports = {
        {
            .node = 0,
            .configured_port = {0, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .output_production = iv::OutputProductionKind::coverage,
            .authored_tock_output = true,
            .ordering = iv::CoverageOrdering::sorted,
            .outgoing_connections = {0},
            .accumulators = {
                .output_change = 0,
                .output_requirement = 0,
            },
        },
        {
            .node = 1,
            .configured_port = {1, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .output_production = iv::OutputProductionKind::coverage,
            .authored_tock_output = true,
            .ordering = iv::CoverageOrdering::sorted,
            .outgoing_connections = {1},
            .accumulators = {
                .output_change = 1,
                .output_requirement = 1,
            },
        },
        {
            .node = 2,
            .configured_port = {2, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::input,
            .input_access = iv::InputAccessKind::coverage,
            .random_access_input = true,
            .ordering = iv::CoverageOrdering::sorted,
            .incoming_connections = {0, 1},
            .accumulators = {
                .input_change = 0,
                .input_requirement = 0,
            },
        },
    };
    plan.connections = {
        {
            .kind = iv::PortKind::sample,
            .source_coverage_ports = {0},
            .target_coverage_ports = {2},
            .sample_source_port_by_channel = {BackgroundPortIndex{0}},
            .sample_read_latencies = {0},
            .sample_target_port = 2,
        },
        {
            .kind = iv::PortKind::sample,
            .source_coverage_ports = {1},
            .target_coverage_ports = {2},
            .sample_source_port_by_channel = {BackgroundPortIndex{1}},
            .sample_read_latencies = {0},
            .sample_target_port = 2,
        },
    };
    plan.accumulators = {
        .input_change_count = 1,
        .output_change_count = 2,
        .output_requirement_count = 2,
        .input_requirement_count = 1,
    };
    sorted_source_region_shapes = {{
        {range(-5, -3), range(0, 10), range(5, 7)},
        {range(-4, -2), range(0, 12), range(0, 8), range(6, 9)},
    }};

    iv::BackgroundCoverageState coverage(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto prepared = workspace.prepare(
        {
            .propagate_forward = &propagate_sorted_source_regions,
            .propagate_reverse = &no_op_background_evaluate,
            .evaluate = &no_op_background_evaluate,
        },
        coverage,
        {.locally_changed_nodes = {0, 1}});

    EXPECT_EQ(
        std::ranges::to<std::vector>(
            prepared.input_regions(2) | std::views::transform(&iv::BackgroundInputRegion::range)),
        (std::vector<iv::GlobalIndexRange>{
            range(-5, -3),
            range(-4, -2),
            range(0, 12),
            range(0, 10),
            range(0, 8),
            range(5, 7),
            range(6, 9),
        }));
    workspace.discard(std::move(prepared));
}

TEST_F(BackgroundCoveragePropagationFixture,
       SortedEventFanInPreservesEachRegionInsideCommonCompleteness)
{
    using namespace iv::graph_jit;
    auto plan = background_fanin_plan();

    plan.nodes[2].outputs.clear();
    plan.nodes[2].accumulators.output_change_count = 0;
    plan.nodes[2].accumulators.output_requirement_count = 0;
    plan.ports.resize(3);
    plan.accumulators.output_change_count = 2;
    plan.accumulators.output_requirement_count = 2;

    for (auto& port : plan.ports) {
        port.configured_port.port_kind = iv::PortKind::event;
        port.kind = iv::PortKind::event;
        port.event_type = iv::EventTypeId::trigger;
        port.ordering = iv::CoverageOrdering::sorted;
    }
    plan.ports[0].outgoing_connections = {0};
    plan.ports[1].outgoing_connections = {0};
    plan.ports[2].incoming_connections = {0};
    plan.connections = {{
        .kind = iv::PortKind::event,
        .source_coverage_ports = {0, 1},
        .target_coverage_ports = {2},
        .event_source_type = iv::EventTypeId::trigger,
        .event_target_type = iv::EventTypeId::trigger,
        .event_deliveries = {
            {
                .source_type = iv::EventTypeId::trigger,
                .source_port = BackgroundPortIndex{0},
                .target_port = BackgroundPortIndex{2},
                .semantic_source_index = 0,
                .mechanism = PlannedDeliveryMechanism::tock_to_random_access,
            },
            {
                .source_type = iv::EventTypeId::trigger,
                .source_port = BackgroundPortIndex{1},
                .target_port = BackgroundPortIndex{2},
                .semantic_source_index = 1,
                .mechanism = PlannedDeliveryMechanism::tock_to_random_access,
            },
        },
    }};
    sorted_source_region_shapes = {{
        {range(0, 10), range(2, 6)},
        {range(4, 12), range(4, 8)},
    }};

    iv::BackgroundCoverageState coverage(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto prepared = workspace.prepare(
        {
            .propagate_forward = &propagate_sorted_source_event_regions,
            .propagate_reverse = &no_op_background_evaluate,
            .evaluate = &no_op_background_evaluate,
        },
        coverage,
        {.locally_changed_nodes = {0, 1}});

    // The merged event stream is authoritative only on [4, 10), where both
    // sources are complete. Each source Region remains an independent Region
    // after clipping to that common completeness, including the duplicate
    // [4, 10) contribution.
    EXPECT_EQ(
        std::ranges::to<std::vector>(
            prepared.input_regions(2) | std::views::transform(&iv::BackgroundInputRegion::range)),
        (std::vector<iv::GlobalIndexRange>{
            range(4, 10),
            range(4, 10),
            range(4, 8),
            range(4, 6),
        }));
    auto const regions = prepared.input_regions(2);
    ASSERT_EQ(regions.size(), 4u);
    ASSERT_EQ(regions[0].sources.size(), 1u);
    ASSERT_EQ(regions[1].sources.size(), 1u);
    EXPECT_EQ(regions[0].sources.front(), (iv::BackgroundInputRegionSource{
        .output = 0, .region_index = 0, .route_index = 0}));
    EXPECT_EQ(regions[1].sources.front(), (iv::BackgroundInputRegionSource{
        .output = 1, .region_index = 0, .route_index = 1}));
    workspace.discard(std::move(prepared));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    FailedReversePreparationLeavesCommittedCoverageUnchanged)
{
    auto const plan = background_fanin_plan();
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };

    background_observation.throw_from_reverse = true;
    EXPECT_THROW(
        static_cast<void>(workspace.prepare(
            operations,
            coverage,
            iv::CoveragePropagationRequest{
                .locally_changed_nodes = {0, 1},
                .output_demands = {
                    iv::OutputCoverageRequest{
                        .port = 3,
                        .required = index_coverage({{10, 22}}),
                    },
                },
            })),
        std::runtime_error);

    background_observation = {};
    auto prepared = workspace.prepare(
        operations,
        coverage,
        iv::CoveragePropagationRequest{
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 3,
                    .required = index_coverage({{10, 22}}),
                },
            },
        });
    auto const& result = prepared.result();
    EXPECT_TRUE(result.output_requirements.empty());
    EXPECT_EQ(background_observation.sink_reverse_calls, 0u);
    workspace.discard(std::move(prepared));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    BackgroundInputRootRepresentsConnectionSetChange)
{
    auto const plan = background_fanin_plan();
    iv::BackgroundCoverageState committed(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate,
    };

    auto const selected_coverage = index_coverage({{30, 34}});
    auto prepared = workspace.prepare(
        operations,
        committed,
        iv::CoveragePropagationRequest{
            .input_changes = {
                iv::InputCoverageChangeRequest{
                    .port = 2,
                    .regions = coverage_regions(selected_coverage),
                    .coverage = selected_coverage,
                    .changed = selected_coverage,
                },
            },
        });
    auto const& result = prepared.result();

    EXPECT_EQ(background_observation.source_forward_calls[0], 0u);
    EXPECT_EQ(background_observation.source_forward_calls[1], 0u);
    EXPECT_EQ(background_observation.sink_forward_calls, 1u);
    EXPECT_EQ(background_observation.sink_input_coverage, selected_coverage);
    ASSERT_EQ(result.output_changes.size(), 1u);
    EXPECT_EQ(result.output_changes[0].port, 3u);
    EXPECT_EQ(result.output_changes[0].coverage, selected_coverage);
    EXPECT_EQ(result.output_changes[0].changed, selected_coverage);
    workspace.discard(std::move(prepared));
}

TEST_F(
    BackgroundCoveragePropagationFixture,
    BackgroundCallbackBindingsExcludeNonTockOutputs)
{
    auto const plan = background_mixed_output_plan();
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    auto const operations = iv::CompiledGraphBackgroundOperations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &no_op_background_evaluate,
        .evaluate = &no_op_background_evaluate,
    };

    auto prepared = workspace.prepare(
        operations,
        coverage,
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {0},
        });
    auto const& result = prepared.result();

    ASSERT_EQ(result.output_changes.size(), 1u);
    EXPECT_EQ(result.output_changes[0].port, 1u);
    EXPECT_EQ(
        result.output_changes[0].coverage,
        index_coverage({{10, 13}}));
    workspace.discard(std::move(prepared));
}

} // namespace

TEST_F(BackgroundCoveragePropagationFixture,
       RequestedInputRegionsPreserveLogicalMetadataWithoutExpandingContents)
{
    using namespace iv::graph_jit;
    for (auto const kind : {iv::PortKind::sample, iv::PortKind::event}) {
        auto plan = background_fanin_plan();
        plan.connections.resize(1);
        plan.ports[1].outgoing_connections.clear();
        plan.ports[2].incoming_connections = {0};
        for (auto& port : plan.ports) {
            port.kind = kind;
            port.configured_port.port_kind = kind;
            if (kind == iv::PortKind::event) port.event_type = iv::EventTypeId::trigger;
        }
        if (kind == iv::PortKind::event) {
            auto& connection = plan.connections[0];
            connection.kind = kind;
            connection.event_source_type = iv::EventTypeId::trigger;
            connection.event_target_type = iv::EventTypeId::trigger;
            connection.event_deliveries = {{
                .source_type = iv::EventTypeId::trigger,
                .source_port = BackgroundPortIndex{0},
                .target_port = BackgroundPortIndex{2},
                .semantic_source_index = 0,
                .mechanism = PlannedDeliveryMechanism::tock_to_random_access,
            }};
        }
        sorted_source_region_shapes = {{
            {range(0, 10), range(0, 10), range(5, 15), range(20, 30)},
            {},
        }};
        iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
        iv::BackgroundPropagationWorkspace workspace(plan, 48000);
        auto const operations = iv::CompiledGraphBackgroundOperations{
            .propagate_forward = kind == iv::PortKind::event
                ? &propagate_sorted_source_event_regions : &propagate_sorted_source_regions,
            .propagate_reverse = &no_op_background_evaluate,
            .evaluate = &no_op_background_evaluate,
        };
        auto prepared = workspace.prepare(operations, coverage, {
            .locally_changed_nodes = {0, 1},
            .input_demands = {{.port = 2, .required = index_coverage({{0, 1}})}},
        });
        ASSERT_EQ(prepared.result().input_requirements.size(), 1u);
        EXPECT_EQ(prepared.result().input_requirements[0].required, index_coverage({{0, 1}}));
        auto const selected = prepared.requested_input_regions(2);
        ASSERT_EQ(selected.size(), 2u);
        EXPECT_EQ(selected[0].range, range(0, 10));
        EXPECT_EQ(selected[1].range, range(0, 10));
        ASSERT_EQ(selected[0].sources.size(), 1u);
        ASSERT_EQ(selected[1].sources.size(), 1u);
        EXPECT_EQ(selected[0].sources[0].region_index, 0u);
        EXPECT_EQ(selected[1].sources[0].region_index, 1u);
        // Logical metadata retains both selected duplicate Regions, while
        // computation stays within the explicitly requested content window.
        auto const producer = std::ranges::find(prepared.result().output_requirements,
            BackgroundPortIndex{0}, &iv::RequiredOutputCoverage::port);
        ASSERT_NE(producer, prepared.result().output_requirements.end());
        EXPECT_EQ(producer->required, index_coverage({{0, 1}}));
        (void)workspace.commit(coverage, std::move(prepared));

        auto retained = workspace.prepare(operations, coverage, {
            .input_demands = {{.port = 2, .required = index_coverage({{20, 21}})}},
        });
        auto const retained_regions = retained.requested_input_regions(2);
        ASSERT_EQ(retained_regions.size(), 1u);
        EXPECT_EQ(retained_regions.front().range, range(20, 30));
        workspace.discard(std::move(retained));
        auto empty = workspace.prepare(operations, coverage, {});
        EXPECT_TRUE(empty.requested_input_regions(2).empty());
        EXPECT_TRUE(empty.result().input_requirements.empty());
        workspace.discard(std::move(empty));
    }
}

namespace {

iv::CoverageTask discover_event_control(
    iv::ReflectedNodeForwardCoverageContext& context)
{
    auto const metadata_id = context.planning.request_input(
        true, 0, iv::CoverageInformation::coverage);
    auto const& metadata = co_await iv::CoverageRequest<iv::CoverageInputInfo>{
        context.planning.access, metadata_id};
    if (metadata.coverage.empty()) {
        auto outputs = static_cast<std::span<iv::OutputCoverageChange>>(context.event_outputs);
        outputs[0].publish_regions({});
        co_return;
    }
    auto positions = index_coverage({{10, 11}});
    auto const contents_id = context.planning.request_input(
        true, 0, iv::CoverageInformation::contents, {.positional = &positions});
    auto const& input = co_await iv::CoverageRequest<iv::RandomAccessEventInputPort>{
        context.planning.access, contents_id};
    bool active = false;
    for (std::size_t region = 0; region < input.region_count; ++region) {
        input.for_each(region, input.regions()[region], [&](iv::TimedEvent const& event) {
            active = std::get<iv::SampleEvent>(event.value).value.value > 0.0f;
        });
    }
    auto outputs = static_cast<std::span<iv::OutputCoverageChange>>(context.event_outputs);
    auto regions = active ? std::vector{range(100, 104)} : std::vector<iv::GlobalIndexRange>{};
    outputs[0].publish_regions(regions);
    outputs[0].change(index_coverage({{100, 104}}));
}

void propagate_discovery_forward(iv::graph_jit::BackgroundEvaluationCall* call)
{
    auto nodes = static_cast<std::span<iv::graph_jit::BackgroundNodeCall>>(call->nodes);
    for (std::size_t node = 0; node < nodes.size(); ++node) {
        auto& frame = nodes[node];
        if (!iv::graph_jit::has_activity(frame.activity,
                iv::graph_jit::BackgroundNodeActivity::forward)) continue;
        if (node == 0) {
            auto outputs = static_cast<std::span<iv::OutputCoverageChange>>(frame.forward.event_outputs);
            std::array regions{range(0, 4), range(10, 14)};
            outputs[0].publish_regions(regions);
            outputs[0].change(index_coverage({{0, 4}, {10, 14}}));
            frame.forward.planning.finish();
        } else if (node == 2) {
            auto context = std::make_unique<iv::ReflectedNodeForwardCoverageContext>(frame.forward);
            auto task = discover_event_control(*context);
            task.own_context(context.release());
            frame.forward.planning.submit(std::move(task));
        }
    }
}

void propagate_discovery_reverse(iv::graph_jit::BackgroundEvaluationCall* call)
{
    auto nodes = static_cast<std::span<iv::graph_jit::BackgroundNodeCall>>(call->nodes);
    if (iv::graph_jit::has_activity(nodes[2].activity,
            iv::graph_jit::BackgroundNodeActivity::reverse)) {
        auto inputs = static_cast<std::span<iv::InputCoverageRequirement>>(nodes[2].reverse.event_inputs);
        inputs[0].require(range(0, 1));
        nodes[2].reverse.planning.finish();
    }
}

struct DiscoveryEvents {
    iv::IndexCoverage coverage{};
    std::vector<iv::GlobalIndexRange> regions{};
    iv::TimedEvent event{};
};

TEST_F(BackgroundCoveragePropagationFixture,
       ContentDiscoveryDoesNotWidenFinalTockInputsAndInvalidatesOnAnInputEdit)
{
    using namespace iv::graph_jit;
    auto plan = background_fanin_plan();
    plan.connections.resize(1);
    plan.ports[1].outgoing_connections.clear();
    plan.ports[2].incoming_connections = {0};
    for (auto& port : plan.ports) {
        port.kind = iv::PortKind::event;
        port.configured_port.port_kind = iv::PortKind::event;
        port.event_type = iv::EventTypeId::sample;
    }
    auto& connection = plan.connections[0];
    connection.kind = iv::PortKind::event;
    connection.event_deliveries = {{
        .source_type = iv::EventTypeId::sample,
        .source_port = BackgroundPortIndex{0},
        .target_port = BackgroundPortIndex{2},
        .semantic_source_index = 0,
        .mechanism = PlannedDeliveryMechanism::tock_to_random_access,
    }};
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace{plan, 48000};
    iv::CompiledGraphBackgroundOperations operations{
        .propagate_forward = &propagate_discovery_forward,
        .propagate_reverse = &propagate_discovery_reverse,
        .evaluate = &no_op_background_evaluate,
    };
    float selected_value = 1.0f;
    iv::BackgroundCoverageDiscovery discovery{
        .data = &selected_value,
        .materialize = +[](void* opaque, iv::PreparedCoveragePropagation&& prepared,
            BackgroundPortIndex input) {
            EXPECT_EQ(input, 2u);
            auto selected = prepared.requested_input_regions(input);
            EXPECT_EQ(selected.size(), 1u);
            if (selected.size() == 1) EXPECT_EQ(selected.front().range, range(10, 14));
            EXPECT_FALSE(has_activity(prepared.node_activity()[2], BackgroundNodeActivity::evaluate));
            auto events = std::make_shared<DiscoveryEvents>();
            for (auto const& region : selected) {
                events->regions.push_back(region.range);
                events->coverage.include(region.range);
            }
            events->event = {.time = index(10),
                .value = iv::SampleEvent{iv::Sample{*static_cast<float*>(opaque)}}};
            return iv::BackgroundDiscoveredInput{
                .owner = events,
                .events = {
                    .data = events.get(), .region_ranges = events->regions.data(),
                    .region_count = events->regions.size(), .coverage_value = &events->coverage,
                    .for_each_event = +[](void const* opaque, std::size_t,
                        iv::GlobalIndexRange requested, void* target,
                        iv::RandomAccessEventInputPort::VisitEvent visit) {
                        auto const& events = *static_cast<DiscoveryEvents const*>(opaque);
                        if (requested.contains(events.event.time)) visit(target, events.event);
                    },
                },
            };
        },
    };
    iv::CoveragePropagationRequest request{
        .locally_changed_nodes = {0},
        .output_demands = {{.port = 3, .required = index_coverage({{100, 101}})}},
    };
    auto first = workspace.prepare(operations, coverage, request, {}, discovery);
    ASSERT_EQ(first.requested_input_regions(2).size(), 1u);
    EXPECT_EQ(first.requested_input_regions(2).front().range, range(0, 4));
    EXPECT_TRUE(std::ranges::any_of(first.read_dependencies(), [](auto const& dependency) {
        return dependency.node == 2 && !dependency.reverse && dependency.input == 2
            && dependency.information == iv::CoverageInformation::contents
            && dependency.positions == index_coverage({{10, 11}});
    }));
    EXPECT_EQ(first.result().output_changes.back().coverage, index_coverage({{100, 104}}));
    (void)workspace.commit(coverage, std::move(first));

    // Same input Region ranges, different contents. Successful discovery reads
    // remain semantic dependencies even though their old waits are gone.
    selected_value = 0.0f;
    auto second = workspace.prepare(operations, coverage, request, {}, discovery);
    auto output = std::ranges::find(second.result().output_changes, BackgroundPortIndex{3},
        &iv::PropagatedOutputChange::port);
    ASSERT_NE(output, second.result().output_changes.end());
    EXPECT_TRUE(output->coverage.empty());
    EXPECT_TRUE(second.requested_input_regions(2).empty());
    workspace.discard(std::move(second));
}

} // namespace

namespace {

iv::CoverageTask discover_sample_output(iv::ReflectedNodeForwardCoverageContext& context)
{
    auto const metadata_id = context.planning.request_input(false, 0, iv::CoverageInformation::coverage);
    auto const& metadata = co_await iv::CoverageRequest<iv::CoverageInputInfo>{
        context.planning.access, metadata_id};
    auto const data_id = context.planning.request_input(false, 0,
        iv::CoverageInformation::contents, {.positional = &metadata.coverage});
    auto const& input = co_await iv::CoverageRequest<iv::RandomAccessContinuousInputPort>{
        context.planning.access, data_id};
    auto region = iv::ContinuousRegion<iv::Sample>{input.region_views()[0]};
    auto const value = static_cast<std::int64_t>(region.at(index(10)).value);
    std::array regions{range(value, value + 4)};
    auto outputs = static_cast<std::span<iv::OutputCoverageChange>>(context.outputs);
    outputs[0].publish_regions(regions);
}

void propagate_sample_discovery(iv::graph_jit::BackgroundEvaluationCall* call)
{
    auto nodes = static_cast<std::span<iv::graph_jit::BackgroundNodeCall>>(call->nodes);
    for (std::size_t node = 0; node < nodes.size(); ++node) {
        auto& frame = nodes[node];
        if (!iv::graph_jit::has_activity(frame.activity,
                iv::graph_jit::BackgroundNodeActivity::forward)) continue;
        if (node == 0) {
            auto outputs = static_cast<std::span<iv::OutputCoverageChange>>(frame.forward.outputs);
            std::array regions{range(10, 14)};
            outputs[0].publish_regions(regions);
            frame.forward.planning.finish();
        } else {
            auto context = std::make_unique<iv::ReflectedNodeForwardCoverageContext>(frame.forward);
            auto task = discover_sample_output(*context);
            task.own_context(context.release());
            frame.forward.planning.submit(std::move(task));
        }
    }
}

void reverse_sample_discovery(iv::graph_jit::BackgroundEvaluationCall* call)
{
    auto nodes = static_cast<std::span<iv::graph_jit::BackgroundNodeCall>>(call->nodes);
    if (!iv::graph_jit::has_activity(nodes[1].activity,
            iv::graph_jit::BackgroundNodeActivity::reverse)) return;
    auto inputs = static_cast<std::span<iv::InputCoverageRequirement>>(nodes[1].reverse.inputs);
    inputs[0].require(range(10, 11));
    nodes[1].reverse.planning.finish();
}

TEST_F(GraphExecutorFixture, ForwardDiscoveryRunsOrdinaryTockInsideTheSelectedTransaction)
{
    using namespace iv::graph_jit;
    auto graph = ephemeral_tock_graph();
    auto& plan = graph.background_evaluation_plan;
    plan.nodes.push_back({.bundle = 1, .tock_state_node = 1,
        .authored_tock_execution = true, .inputs = {1}, .outputs = {2},
        .accumulators = {.input_change_count = 1, .output_change_begin = 1,
            .output_change_count = 1, .output_requirement_begin = 1,
            .output_requirement_count = 1, .input_requirement_count = 1}});
    graph.tock_state_layout.nodes.emplace_back();
    auto source = plan.ports[0];
    plan.ports[0].outgoing_connections = {0};
    auto input = source;
    input.node = 1;
    input.configured_port = {1, iv::PortKind::sample, 0};
    input.direction = PortDirection::input;
    input.input_access = iv::InputAccessKind::coverage;
    input.random_access_input = true;
    input.authored_tock_output = false;
    input.realization.reset();
    input.incoming_connections = {0};
    input.accumulators = {.input_change = 0, .input_requirement = 0};
    plan.ports.push_back(input);
    auto output = source;
    output.node = 1;
    output.configured_port = {1, iv::PortKind::sample, 0};
    output.accumulators = {.output_change = 1, .output_requirement = 1};
    plan.ports.push_back(output);
    plan.connections = {{.kind = iv::PortKind::sample,
        .source_coverage_ports = {0}, .target_coverage_ports = {1}}};
    plan.sample_target_subsets = {{.port = input.configured_port,
        .target_layout = input.sample_layout, .geometry = input.continuous_geometry,
        .access = PlannedDestinationAccess::random_access,
        .channels = {{.bundle = 1, .port = 0, .channel = 0}}}};
    plan.storage.ports[0].target_subsets = {0};
    auto destination_storage = plan.storage.ports[0];
    destination_storage.source_port = output.configured_port;
    destination_storage.output_port = 2;
    destination_storage.target_subsets.clear();
    plan.storage.ports.push_back(destination_storage);
    plan.runtime.bindings.push_back({.node = 1, .port = 1,
        .kind = iv::PortKind::sample, .direction = PortDirection::input, .storage = {0}});
    plan.runtime.bindings.push_back({.node = 1, .port = 2,
        .kind = iv::PortKind::sample, .direction = PortDirection::output, .storage = {1}});
    plan.runtime.port_bindings = {0, 1, 2};
    plan.runtime.node_operations.resize(2);
    plan.runtime.node_replay_invocations.resize(2);
    plan.accumulators = {.input_change_count = 1, .output_change_count = 2,
        .output_requirement_count = 2, .input_requirement_count = 1};
    graph.background_operations = {.propagate_forward = &propagate_sample_discovery,
        .propagate_reverse = &reverse_sample_discovery, .evaluate = &evaluate_persisted_probe};
    auto states = initialize_tock_states(graph);
    iv::BackgroundCoverageState coverage{2};
    iv::BackgroundPropagationWorkspace propagation{plan, 48000};
    iv::PersistedDataStore data;
    iv::BackgroundEvaluationTransaction transaction{graph, states, coverage, propagation, data,
        {.roots = {.locally_changed_nodes = {0},
            .output_demands = {{.port = 2, .required = index_coverage({{110, 111}})}}}}};
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);
    auto output_change = std::ranges::find(result->coverage.output_changes, BackgroundPortIndex{2},
        &iv::PropagatedOutputChange::port);
    ASSERT_NE(output_change, result->coverage.output_changes.end());
    EXPECT_EQ(output_change->coverage, index_coverage({{110, 114}}));
    EXPECT_EQ(persisted_probe_evaluate_calls, 3u);
}

} // namespace

TEST_F(BackgroundCoveragePropagationFixture, BufferedReverseDependenciesAreActivatedWhenTheirDemandIsDiscovered)
{
    using namespace iv::graph_jit;
    auto plan = background_fanin_plan();
    for (auto& port : plan.ports)
        if (port.direction == PortDirection::output) port.realization = iv::OutputRealization::buffered;
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(plan, 48000);
    iv::CompiledGraphBackgroundOperations operations{
        .propagate_forward = &propagate_background_forward,
        .propagate_reverse = &propagate_background_reverse,
        .evaluate = &no_op_background_evaluate};
    auto prepared = workspace.prepare(operations, coverage,
        {.locally_changed_nodes = {0, 1},
         .output_demands = {{.port = 3, .required = index_coverage({{11, 21}})}}},
        {.expand_output = +[](void*, BackgroundPortIndex, iv::IndexCoverage const&,
            std::span<iv::GlobalIndexRange const>, iv::IndexCoverage const&,
            iv::IndexCoverage const& requested, iv::IndexCoverage& missing) {
            // Cold buffered storage has no valid contents yet. The sink's
            // reverse callback will discover the source demands later.
            missing.include(requested);
        }});
    for (std::size_t node : {0u, 1u, 2u})
        EXPECT_TRUE(has_activity(prepared.node_activity()[node], BackgroundNodeActivity::evaluate));
    EXPECT_EQ(background_observation.sink_reverse_calls, 1u);
    workspace.discard(std::move(prepared));
}

TEST_F(GraphExecutorFixture, ObservationStampsFollowActivationRatherThanCodePreparationOrder)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_first);
    auto second = compiled_graph(2, &observe_first);
    auto third = compiled_graph(3, &observe_first);
    ASSERT_EQ(stage_graph(executor, first), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    executor.tick_block(index(0), 16);
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    auto const first_activation = frame->activation_id;
    ASSERT_EQ(stage_graph(executor, second), iv::GraphExecutorStageResult::accepted);
    ASSERT_EQ(stage_graph(executor, third), iv::GraphExecutorStageResult::accepted);
    // This replacement is prepared after generation 3, but executes first.
    auto replacement = std::make_shared<iv::CompiledGraph>(*second);
    replacement->root_operations.tick_block = {.function = &observe_second};
    ASSERT_EQ(executor.stage_realtime_code(replacement), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(16)));
    executor.tick_block(index(16), 16);
    frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->project_generation, 2u);
    auto const second_activation = frame->activation_id;
    EXPECT_GT(second_activation, first_activation);
    ASSERT_TRUE(executor.activate_pending(index(32)));
    executor.tick_block(index(32), 16);
    frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->project_generation, 3u);
    EXPECT_GT(frame->activation_id, second_activation);
    EXPECT_EQ(frame->count, 0u);
    executor.tick_block(index(48), 16);
    EXPECT_EQ(executor.collect_observations(), nullptr); // No idle probe work.
}


TEST_F(GraphExecutorFixture, PreparedGenerationCanBeDiscardedWithoutPublishingAChainLink)
{
    iv::GraphExecutor executor;
    auto first = compiled_graph(1, &observe_first);
    {
        auto discarded = executor.prepare(first);
        ASSERT_TRUE(discarded);
        EXPECT_FALSE(executor.pending_generation());
        EXPECT_FALSE(executor.active_generation());
        EXPECT_FALSE(executor.accepted_graph());
    }
    auto prepared = executor.prepare(first);
    ASSERT_TRUE(prepared);
    EXPECT_EQ(std::move(prepared).publish(), iv::GraphExecutorStageResult::accepted);
    EXPECT_EQ(executor.pending_generation(), 1u);
    EXPECT_TRUE(executor.activate_pending(index(0)));
    iv::GraphExecutorExecutionReadRequest read;
    executor.handle_execution_read(read);
    EXPECT_EQ(read.active_generation, 1u);
    EXPECT_EQ(std::move(prepared).publish(), iv::GraphExecutorStageResult::ignored_stale);
}


TEST_F(GraphExecutorFixture, PreparedOutputGainsRejectCodeOnlyReplacementAndIgnoreLiveTargets)
{
    std::size_t state_offset = 0;
    auto graph = std::const_pointer_cast<iv::CompiledGraph>(compiled_graph(
        1, &observe_first, manual_control_layout(state_offset)));
    auto id = manual_control_id();
    id.kind = iv::ManualControlKind::output_gain;
    iv::ConfiguredManualControl configured{.id = id, .settled_value = .5f, .requires_preparation = true};
    auto configuration = std::make_shared<iv::ConfiguredGraph>();
    configuration->manual_controls.push_back(configured);
    graph->configured_graph = configuration;
    graph->manual_controls.push_back({.configured = configured, .state_storage_offset = state_offset, .participates = true});
    auto changed = std::make_shared<iv::CompiledGraph>(*graph);
    auto successor = std::make_shared<iv::ConfiguredGraph>(*configuration);
    successor->manual_controls[0].settled_value = .75f;
    changed->configured_graph = successor;
    EXPECT_FALSE(iv::GraphExecutor::can_replace_realtime_code(*graph, *changed));
    EXPECT_TRUE(iv::GraphExecutor::can_replace_realtime_code(*graph, *graph));
    iv::GraphExecutor executor;
    ASSERT_EQ(stage_graph(executor, graph), iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    executor.tick_block(index(0), 16);
    ASSERT_NE(first_tick.storage, nullptr);
    iv::ManualControlRuntimeState before;
    std::memcpy(&before, first_tick.storage + state_offset, sizeof(before));
    EXPECT_FALSE(executor.set_manual_control_target(1, id, .9f));
    executor.tick_block(index(16), 16);
    iv::ManualControlRuntimeState after;
    std::memcpy(&after, first_tick.storage + state_offset, sizeof(after));
    EXPECT_EQ(after.target_bits, before.target_bits);
}


TEST_F(BackgroundCoveragePropagationFixture, InputUnionReplayPreservesGapsWhileFullReplayCoversThem)
{
    using namespace iv;
    using namespace iv::graph_jit;
    for (auto mode : {TickReplayMode::input_union, TickReplayMode::full}) {
        BackgroundEvaluationPlan plan;
        plan.nodes = {{.bundle = 0, .replays_tick = true,
            .uses_replay_forward_coverage = true, .replay_mode = mode,
            .inputs = {0, 1}, .outputs = {2}}};
        plan.ports = {
            {.node = 0, .configured_port = {0, PortKind::sample, 0},
                .kind = PortKind::sample, .direction = PortDirection::input,
                .sample_fallback_value = .25f,
                .accumulators = {.input_change = 0, .input_requirement = 0}},
            {.node = 0, .configured_port = {0, PortKind::sample, 1},
                .kind = PortKind::sample, .direction = PortDirection::input,
                .sample_fallback_value = 1.f,
                .accumulators = {.input_change = 1, .input_requirement = 1}},
            {.node = 0, .configured_port = {0, PortKind::sample, 0},
                .kind = PortKind::sample, .direction = PortDirection::output,
                .replayed_tick_output = true,
                .accumulators = {.output_change = 0, .output_requirement = 0}},
        };
        plan.accumulators = {.input_change_count = 2, .output_change_count = 1,
            .output_requirement_count = 1, .input_requirement_count = 2};
        BackgroundCoverageState state(1);
        BackgroundPropagationWorkspace workspace(plan, 48000);
        CompiledGraphBackgroundOperations operations{
            .propagate_forward = +[](BackgroundEvaluationCall* call) {
                for (auto const& node : std::span<BackgroundNodeCall>(call->nodes)) {
                    if (!has_activity(node.activity, BackgroundNodeActivity::forward)) continue;
                    node.replay_forward(node.replay_context, node.forward);
                }
            },
            .propagate_reverse = &no_op_background_evaluate, .evaluate = &no_op_background_evaluate};
        auto prepared = workspace.prepare(operations, state, {
            .locally_changed_nodes = {0},
            .input_changes = {{.port = 0, .coverage = index_coverage({{20, 24}}),
                .changed = index_coverage({{20, 24}})}, {.port = 1}},
        });
        auto result = workspace.commit(state, std::move(prepared));
        ASSERT_EQ(result.output_changes.size(), 1u);
        if (mode == TickReplayMode::input_union)
            EXPECT_EQ(result.output_changes[0].coverage, index_coverage({{20, 24}}));
        else EXPECT_EQ(result.output_changes[0].coverage, full_index_coverage());
    }
}
