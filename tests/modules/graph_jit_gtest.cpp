#include "../module_test_utils.h"

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
#include <intravenous/runtime/tick_invocation_frame.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
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

iv::ReflectedContinuousPortStorageBinding sample_storage_binding(
    std::span<iv::Sample> samples)
{
    auto geometry = iv::continuous_value_storage_geometry(
        iv::ContinuousValueTypeId::sample, std::nullopt);
    IV_ASSERT(geometry.has_value(),
        "Sample must have a registered continuous storage geometry");
    iv::ReflectedContinuousPortStorageBinding result{
        .geometry = *geometry,
        .value_capacity = samples.size(),
        .channel_layout = iv::mono_planar_channel_layout,
    };
    result.channels[0] = {
        .storage = reinterpret_cast<std::byte*>(samples.data()),
        .value_capacity = samples.size(),
        .value_stride_bytes = sizeof(iv::Sample),
    };
    return result;
}

std::vector<std::byte> sample_value_bytes(iv::Sample value)
{
    std::vector<std::byte> result(sizeof(value));
    std::memcpy(result.data(), &value, sizeof(value));
    return result;
}

iv::Sample sample_value_from_bytes(std::span<std::byte const> bytes)
{
    EXPECT_EQ(bytes.size(), sizeof(iv::Sample));
    iv::Sample result{};
    if (bytes.size() == sizeof(result)) {
        std::memcpy(&result, bytes.data(), sizeof(result));
    }
    return result;
}

bool configured_sample_connection_is_detached(
    iv::ConfiguredSampleConnection const& connection)
{
    return std::ranges::any_of(
        connection.source_channels,
        [](iv::ConfiguredSampleConnectionSource const& source) {
            return source.detach.has_value();
        });
}

std::expected<iv::graph_jit::detail::ConnectionAnalysisPlan, std::string>
build_connection_analysis_plan(
    iv::ConfiguredGraph const& graph,
    std::size_t kernel_block_size,
    iv::RealtimeStorageCostModel const& cost_model = {})
{
    auto port_realization = iv::graph_jit::resolve_port_realization(graph);
    if (!port_realization) {
        return std::unexpected(std::move(port_realization.error()));
    }
    return iv::graph_jit::detail::build_connection_analysis_plan(
        graph, *port_realization, kernel_block_size, cost_model);
}

iv::graph_jit::TickInvocationCall const empty_tick_invocation{};
constexpr char graph_jit_state_package_id[] = "iv.test.graph_jit.state_context.package";
constexpr char graph_jit_stateful_module_id[] = "iv.test.graph_jit.state_context.stateful_module";
constexpr char graph_jit_state_only_module_id[] = "iv.test.graph_jit.state_context.state_only_module";
constexpr char graph_jit_background_only_module_id[] = "iv.test.graph_jit.state_context.background_only_module";
constexpr char graph_jit_background_execution_module_id[] = "iv.test.graph_jit.state_context.background_execution_module";
constexpr char graph_jit_native_batch_tick_module_id[] =
    "iv.test.graph_jit.state_context.native_batch_tick_module";
constexpr char graph_jit_native_batch_aliasing_module_id[] =
    "iv.test.graph_jit.state_context.native_batch_aliasing_module";
constexpr char graph_jit_native_batch_replay_module_id[] =
    "iv.test.graph_jit.state_context.native_batch_replay_module";
constexpr char graph_jit_native_batch_background_module_id[] =
    "iv.test.graph_jit.state_context.native_batch_background_module";
constexpr char graph_jit_native_batch_activity_module_id[] =
    "iv.test.graph_jit.state_context.native_batch_activity_module";
constexpr char graph_jit_native_batch_reverse_module_id[] =
    "iv.test.graph_jit.state_context.native_batch_reverse_module";
constexpr char graph_jit_stateless_module_id[] = "iv.test.graph_jit.state_context.stateless_module";
constexpr char graph_jit_configured_module_id[] = "iv.test.graph_jit.state_context.configured_module";
constexpr char graph_jit_pointer_configured_module_id[] = "iv.test.graph_jit.state_context.pointer_configured_module";
constexpr char graph_jit_multiple_module_id[] = "iv.test.graph_jit.state_context.multiple_module";
constexpr char graph_jit_skippable_pair_module_id[] = "iv.test.graph_jit.state_context.skippable_pair_module";
constexpr char graph_jit_limited_block_module_id[] = "iv.test.graph_jit.state_context.limited_block_module";
constexpr char graph_jit_activity_module_id[] =
    "iv.test.graph_jit.state_context.activity_module";
constexpr char graph_jit_activity_disabled_module_id[] =
    "iv.test.graph_jit.state_context.activity_disabled_module";
constexpr char graph_jit_activity_type_default_module_id[] =
    "iv.test.graph_jit.state_context.activity_type_default_module";
constexpr char graph_jit_event_activity_module_id[] =
    "iv.test.graph_jit.state_context.event_activity_module";
constexpr char graph_jit_ported_module_id[] = "iv.test.graph_jit.state_context.ported_module";
constexpr char graph_jit_direct_sample_module_id[] = "iv.test.graph_jit.state_context.direct_sample_module";
constexpr char graph_jit_disconnected_sample_input_module_id[] = "iv.test.graph_jit.state_context.disconnected_sample_input_module";
constexpr char graph_jit_disconnected_sample_output_module_id[] = "iv.test.graph_jit.state_context.disconnected_sample_output_module";
constexpr char graph_jit_transient_sample_module_id[] = "iv.test.graph_jit.state_context.transient_sample_module";
constexpr char graph_jit_reused_sample_arena_module_id[] = "iv.test.graph_jit.state_context.reused_sample_arena_module";
constexpr char graph_jit_sample_fanout_conversion_module_id[] = "iv.test.graph_jit.state_context.sample_fanout_conversion_module";
constexpr char graph_jit_sample_revision_module_id[] = "iv.test.graph_jit.state_context.sample_revision_module";
constexpr char graph_jit_persistent_sample_revision_module_id[] = "iv.test.graph_jit.state_context.persistent_sample_revision_module";
constexpr char graph_jit_composed_sample_revision_module_id[] = "iv.test.graph_jit.state_context.composed_sample_revision_module";
constexpr char graph_jit_tick_fallback_sample_module_id[] = "iv.test.graph_jit.state_context.tick_fallback_sample_module";
constexpr char graph_jit_stereo_conversion_module_id[] = "iv.test.graph_jit.state_context.stereo_conversion_module";
constexpr char graph_jit_stereo_history_conversion_module_id[] = "iv.test.graph_jit.state_context.stereo_history_conversion_module";
constexpr char graph_jit_history_fanout_module_id[] = "iv.test.graph_jit.state_context.history_fanout_module";
constexpr char graph_jit_persistent_history_module_id[] = "iv.test.graph_jit.state_context.persistent_history_module";
constexpr char graph_jit_large_history_rewire_module_id[] =
    "iv.test.graph_jit.state_context.large_history_rewire_module";
constexpr char graph_jit_latency_compensation_module_id[] = "iv.test.graph_jit.state_context.latency_compensation_module";
constexpr char graph_jit_latency_conversion_fanout_module_id[] = "iv.test.graph_jit.state_context.latency_conversion_fanout_module";
constexpr char graph_jit_composed_latency_module_id[] = "iv.test.graph_jit.state_context.composed_latency_module";
constexpr char graph_jit_composed_history_module_id[] = "iv.test.graph_jit.state_context.composed_history_module";
constexpr char graph_jit_projected_composition_module_id[] = "iv.test.graph_jit.state_context.projected_composition_module";
constexpr char graph_jit_fft_value_module_id[] = "iv.test.graph_jit.state_context.fft_value_module";
constexpr char graph_jit_fractional_index_value_module_id[] = "iv.test.graph_jit.state_context.fractional_index_value_module";
constexpr char graph_jit_persisted_fft_value_module_id[] = "iv.test.graph_jit.state_context.persisted_fft_value_module";
constexpr char graph_jit_persisted_non_project_rate_sample_module_id[] = "iv.test.graph_jit.state_context.persisted_non_project_rate_sample_module";
constexpr char graph_jit_direct_event_module_id[] = "iv.test.graph_jit.state_context.direct_event_module";
constexpr char graph_jit_transient_event_module_id[] = "iv.test.graph_jit.state_context.transient_event_module";
constexpr char graph_jit_converted_event_fanout_module_id[] = "iv.test.graph_jit.state_context.converted_event_fanout_module";
constexpr char graph_jit_retained_event_module_id[] = "iv.test.graph_jit.state_context.retained_event_module";
constexpr char graph_jit_persistent_event_ring_module_id[] = "iv.test.graph_jit.state_context.persistent_event_ring_module";
constexpr char graph_jit_retained_converted_event_fanout_module_id[] = "iv.test.graph_jit.state_context.retained_converted_event_fanout_module";
constexpr char graph_jit_tick_capture_module_id[] = "iv.test.graph_jit.state_context.tick_capture_module";
constexpr char graph_jit_declaration_layout_module_id[] =
    "iv.test.graph_jit.state_context.declaration_layout_module";
constexpr char graph_jit_sample_feedback_a_id[] = "iv.test.graph_jit.state_context.sample_feedback_a";
constexpr char graph_jit_sample_feedback_b_id[] = "iv.test.graph_jit.state_context.sample_feedback_b";
constexpr char graph_jit_multi_branch_sample_feedback_id[] = "iv.test.graph_jit.state_context.multi_branch_sample_feedback";
constexpr char graph_jit_temporal_sample_feedback_id[] = "iv.test.graph_jit.state_context.temporal_sample_feedback";
constexpr char graph_jit_revising_sample_feedback_id[] = "iv.test.graph_jit.state_context.revising_sample_feedback";
constexpr char graph_jit_projected_revising_sample_feedback_id[] = "iv.test.graph_jit.state_context.projected_revising_sample_feedback";
constexpr char graph_jit_converted_sample_feedback_id[] = "iv.test.graph_jit.state_context.converted_sample_feedback";
constexpr char graph_jit_event_feedback_a_id[] = "iv.test.graph_jit.state_context.event_feedback_a";
constexpr char graph_jit_retained_fan_in_event_feedback_a_id[] = "iv.test.graph_jit.state_context.retained_fan_in_event_feedback_a";
constexpr char graph_jit_latent_event_feedback_a_id[] = "iv.test.graph_jit.state_context.latent_event_feedback_a";
constexpr char graph_jit_persistent_latent_event_feedback_a_id[] = "iv.test.graph_jit.state_context.persistent_latent_event_feedback_a";
constexpr char graph_jit_persistent_latent_boundary_event_feedback_a_id[] = "iv.test.graph_jit.state_context.persistent_latent_boundary_event_feedback_a";
constexpr char graph_jit_event_feedback_b_id[] = "iv.test.graph_jit.state_context.event_feedback_b";
constexpr char graph_jit_boundary_event_feedback_a_id[] = "iv.test.graph_jit.state_context.boundary_event_feedback_a";
constexpr char graph_jit_boundary_event_feedback_b_id[] = "iv.test.graph_jit.state_context.boundary_event_feedback_b";
constexpr char graph_jit_event_feedback_burst_a_id[] = "iv.test.graph_jit.state_context.event_feedback_burst_a";
constexpr char graph_jit_event_feedback_fanout_a_id[] = "iv.test.graph_jit.state_context.event_feedback_fanout_a";
constexpr char graph_jit_retained_dual_event_feedback_a_id[] = "iv.test.graph_jit.state_context.retained_dual_event_feedback_a";
constexpr char graph_jit_dual_event_feedback_b_id[] = "iv.test.graph_jit.state_context.dual_event_feedback_b";

TEST(ContinuousValueActivity, RegistryOwnsSemanticEqualityAndClearTests)
{
    auto const& sample = iv::continuous_value_type_info(
        iv::ContinuousValueTypeId::sample);
    iv::Sample const positive_zero{0.0f};
    iv::Sample const negative_zero{-0.0f};
    ASSERT_NE(sample.values_equal, nullptr);
    ASSERT_NE(sample.value_is_clear, nullptr);
    EXPECT_TRUE(sample.values_equal(&positive_zero, &negative_zero, 1));
    EXPECT_TRUE(sample.value_is_clear(&negative_zero, 1));

    auto const& fft = iv::continuous_value_type_info(
        iv::ContinuousValueTypeId::fft_block);
    std::array<iv::FFTBin, 2> first{};
    std::array<iv::FFTBin, 2> second{};
    ASSERT_NE(fft.values_equal, nullptr);
    ASSERT_NE(fft.value_is_clear, nullptr);
    EXPECT_TRUE(fft.values_equal(first.data(), second.data(), first.size()));
    EXPECT_TRUE(fft.value_is_clear(first.data(), first.size()));
    second[1] = iv::FFTBin{1.0f, -2.0f};
    EXPECT_FALSE(fft.values_equal(first.data(), second.data(), first.size()));
    EXPECT_FALSE(fft.value_is_clear(second.data(), second.size()));
}

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

struct SingleSpanProbeMirror {
    std::uint64_t calls = 0;
    std::uint64_t observed_state_extent = 0;
    std::uint64_t observed_background_extent = 0;
};

struct BackgroundExecutionProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t sample_rate = 0;
};

struct NativeBatchTickProbeStateMirror {
    std::uint64_t tick_batch_calls = 0;
    std::uint64_t skip_batch_calls = 0;
    std::uint64_t last_lane_count = 0;
    std::uint64_t configured_id = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
};

struct NativeBatchAliasingProbeStateMirror {
    std::uint64_t tick_batch_calls = 0;
    std::uint64_t last_lane_count = 0;
    std::uint64_t configured_id = 0;
};

struct NativeBatchActivityProbeStateMirror {
    std::uint64_t tick_batch_calls = 0;
    std::uint64_t skip_batch_calls = 0;
    std::uint64_t tick_lane_total = 0;
    std::uint64_t skip_lane_total = 0;
};

struct NativeBatchBackgroundProbeStateMirror {
    std::uint64_t tock_batch_calls = 0;
    std::uint64_t last_lane_count = 0;
    std::uint64_t configured_id = 0;
    std::uint64_t sample_rate = 0;
};

struct DeclarationLayoutParentStateMirror {
    std::span<std::uint64_t> local;
    std::span<std::span<std::byte>> nested;
    std::uint64_t calls = 0;
    std::uint64_t local_sum = 0;
    std::uint64_t nested_sum = 0;
};

struct DeclarationLayoutLeafStateMirror {
    std::uint64_t marker = 0;
};

struct DeclarationLayoutLeafTockStateMirror {
    std::uint64_t marker = 0;
};

struct DeclarationLayoutProducerStateMirror {
    std::span<std::uint64_t> values;
    std::uint64_t calls = 0;
};

struct DeclarationLayoutConsumerStateMirror {
    std::span<std::uint64_t> imported;
    std::uint64_t initialize_sum = 0;
    std::uint64_t tick_sum = 0;
    std::uint64_t calls = 0;
};

struct alignas(256) FFTValueConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t root_block_size = 0;
    std::uint64_t input_block_size = 0;
    std::uint64_t extended_block_size = 0;
    std::uint64_t value_index = 0;
    float preceding_bin = 0.0f;
    float first_bin = 0.0f;
    float last_bin = 0.0f;
};

struct alignas(512) FractionalIndexConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t root_block_size = 0;
    std::uint64_t input_block_size = 0;
    std::uint64_t value_index = 0;
    std::uint64_t first_whole = 0;
    std::uint64_t first_fraction = 0;
    std::uint64_t last_whole = 0;
    std::uint64_t last_fraction = 0;
};

struct BackgroundOperationHookCapture {
    BackgroundExecutionProbeStateMirror const* state = nullptr;
    std::size_t prepare_calls = 0;
    std::size_t finalize_calls = 0;
    std::uint64_t calls_seen_by_prepare = 0;
    std::uint64_t calls_seen_by_finalize = 0;
};

void prepare_background_operations(void* opaque)
{
    auto& capture = *static_cast<BackgroundOperationHookCapture*>(opaque);
    ++capture.prepare_calls;
    capture.calls_seen_by_prepare = capture.state->calls;
}

void finalize_background_operations(void* opaque)
{
    auto& capture = *static_cast<BackgroundOperationHookCapture*>(opaque);
    ++capture.finalize_calls;
    capture.calls_seen_by_finalize = capture.state->calls;
}

struct BackgroundExecutionCapture {
    std::vector<iv::GlobalIndexRange> regions{};
    iv::IndexCoverage coverage{};
    iv::IndexCoverage changed{};
    std::vector<std::pair<iv::GlobalIndex, float>> samples{};
};

struct GeneratedTickCaptureProbe {
    std::size_t sample_calls = 0;
    std::size_t event_calls = 0;
    iv::GlobalIndex sample_index{};
    std::size_t block_size = 0;
    std::size_t sample_history = 0;
    std::size_t sample_latency = 0;
    float first_sample = 0.0f;
    float last_sample = 0.0f;
    std::size_t event_history = 0;
    std::size_t event_latency = 0;
    std::size_t event_count = 0;
    iv::GlobalIndex first_event_time{};
};

void capture_generated_tick_samples(
    void* opaque,
    iv::ReflectedContinuousOutputPortBinding const* output,
    iv::GlobalIndex sample_index,
    std::size_t block_size) noexcept
{
    auto& probe = *static_cast<GeneratedTickCaptureProbe*>(opaque);
    ++probe.sample_calls;
    probe.sample_index = sample_index;
    probe.block_size = block_size;
    if (!output || block_size == 0) return;
    probe.sample_history = output->history;
    probe.sample_latency = output->latency;
    auto const& storage = output->storage;
    if (storage.channel_layout.channel_type != iv::ChannelTypeId::mono
        || storage.geometry.value_type != iv::ContinuousValueTypeId::sample
        || storage.value_capacity == 0
        || !std::has_single_bit(storage.value_capacity)) {
        return;
    }
    auto read = [&](std::uint64_t ordinal) {
        auto const& channel = storage.channels[0];
        if (!channel.storage || channel.value_capacity == 0
            || !std::has_single_bit(channel.value_capacity)
            || channel.value_stride_bytes < sizeof(iv::Sample)
            || channel.value_delay >= channel.value_capacity) {
            return 0.0f;
        }
        auto const storage_ordinal = ordinal
            + storage.storage_latency - channel.value_delay;
        auto const slot = static_cast<std::size_t>(storage_ordinal)
            & (channel.value_capacity - 1);
        return static_cast<float>(*reinterpret_cast<iv::Sample const*>(
            channel.storage + slot * channel.value_stride_bytes));
    };
    probe.first_sample = read(output->sequential_begin_ordinal);
    probe.last_sample = read(
        output->sequential_begin_ordinal + block_size - 1);
}

void capture_generated_tick_events(
    void* opaque,
    iv::ReflectedEventOutputPortBinding const* output,
    iv::GlobalIndex,
    std::size_t) noexcept
{
    auto& probe = *static_cast<GeneratedTickCaptureProbe*>(opaque);
    ++probe.event_calls;
    if (!output) return;
    probe.event_history = output->history;
    probe.event_latency = output->latency;
    auto const& storage = output->storage;
    if (!storage.storage || storage.event_capacity == 0
        || !std::has_single_bit(storage.event_capacity)) {
        return;
    }
    std::size_t read = 0;
    auto write = *reinterpret_cast<std::size_t const*>(
        storage.storage + storage.count_offset);
    if (storage.persistent_ring) {
        read = *reinterpret_cast<std::size_t const*>(
            storage.storage + storage.read_index_offset);
        write = *reinterpret_cast<std::size_t const*>(
            storage.storage + storage.write_index_offset);
    }
    if (write < read) return;
    probe.event_count = std::min(write - read, storage.event_capacity);
    if (probe.event_count == 0) return;
    auto const* events = reinterpret_cast<iv::TimedEvent const*>(
        storage.storage + storage.events_offset);
    probe.first_event_time = events[
        read & (storage.event_capacity - 1)].time;
}

void capture_background_regions(
    void* opaque,
    iv::GlobalIndexRange const* regions,
    std::size_t region_count)
{
    auto& capture = *static_cast<BackgroundExecutionCapture*>(opaque);
    capture.regions.clear();
    if (region_count != 0) {
        capture.regions.assign(regions, regions + region_count);
    }
    capture.coverage = {};
    for (auto const region : capture.regions) {
        capture.coverage.include(region);
    }
}

void capture_background_change(
    void* opaque, iv::IndexCoverage const& changed)
{
    static_cast<BackgroundExecutionCapture*>(opaque)->changed = changed;
}

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

struct SampleFeedbackAStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t scc_feedback_latency = 0;
    std::array<std::uint64_t, 24> indices{};
    std::array<std::uint64_t, 24> block_sizes{};
    std::array<float, 24> first_inputs{};
    std::array<float, 24> last_inputs{};
    std::uint32_t marker = 0;
};

struct SampleFeedbackBStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t scc_feedback_latency = 0;
    std::array<std::uint64_t, 24> indices{};
    std::array<std::uint64_t, 24> block_sizes{};
    std::array<float, 24> first_inputs{};
    std::array<float, 24> last_inputs{};
    std::uint64_t marker = 0;
    std::uint64_t distinct_padding = 0;
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

struct TemporalSampleFeedbackStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t scc_feedback_latency = 0;
    std::array<std::uint64_t, 24> indices{};
    std::array<std::uint64_t, 24> block_sizes{};
    std::array<float, 24> current_inputs{};
    std::array<float, 24> history_3_inputs{};
    std::array<float, 24> first_inputs{};
    std::array<float, 24> last_inputs{};
    std::uint32_t marker = 0;
};

struct RevisingSampleFeedbackStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 8> indices{};
    std::array<float, 8> first_inputs{};
    std::array<float, 8> last_inputs{};
    std::uint32_t marker = 0;
};

struct ProjectedRevisingSampleFeedbackStateMirror {
    std::uint64_t calls = 0;
    std::array<std::uint64_t, 8> indices{};
    std::array<float, 8> first_left{};
    std::array<float, 8> first_right{};
    std::array<float, 8> last_left{};
    std::array<float, 8> last_right{};
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

std::vector<iv::NodeLayout::RegionHandle> sample_feedback_alignment_regions(
    iv::NodeStorage const& storage)
{
    if (!storage.layout) return {};

    std::vector<iv::NodeLayout::RegionHandle> result;
    for (std::size_t region_index = 0;
         region_index < storage.layout->regions.size(); ++region_index) {
        auto const& region = storage.layout->regions[region_index];
        if (region.kind != iv::NodeLayout::Region::Kind::raw
            || !region.migration_identity.starts_with(
                "graphjit.sample.routed_feedback:")) {
            continue;
        }
        if (region.migration_identity.find(":alignment=") == std::string::npos)
            continue;
        result.push_back(iv::NodeLayout::RegionHandle{.index = region_index});
    }
    return result;
}

struct SampleFeedbackAlignmentRegionSnapshot {
    std::string migration_identity{};
    std::vector<std::byte> bytes{};
};

std::vector<SampleFeedbackAlignmentRegionSnapshot>
snapshot_sample_feedback_alignment_regions(iv::NodeStorage const& storage)
{
    std::vector<SampleFeedbackAlignmentRegionSnapshot> result;
    for (auto const handle : sample_feedback_alignment_regions(storage)) {
        auto const bytes = storage.region_bytes(handle);
        result.push_back(SampleFeedbackAlignmentRegionSnapshot{
            .migration_identity =
                storage.layout->regions[handle.index].migration_identity,
            .bytes = std::vector<std::byte>(bytes.begin(), bytes.end()),
        });
    }
    return result;
}

void expect_sample_feedback_alignment_regions_equal(
    std::span<SampleFeedbackAlignmentRegionSnapshot const> expected,
    iv::NodeStorage const& actual)
{
    auto const actual_regions = sample_feedback_alignment_regions(actual);
    ASSERT_EQ(actual_regions.size(), expected.size());
    for (auto const& expected_region : expected) {
        auto const actual_region = std::ranges::find_if(
            actual_regions,
            [&](iv::NodeLayout::RegionHandle handle) {
                return actual.layout->regions[handle.index].migration_identity
                    == expected_region.migration_identity;
            });
        ASSERT_NE(actual_region, actual_regions.end());
        auto const actual_bytes = actual.region_bytes(*actual_region);
        ASSERT_EQ(actual_bytes.size(), expected_region.bytes.size());
        EXPECT_TRUE(std::ranges::equal(actual_bytes, expected_region.bytes));
    }
}

ConvertedSampleFeedbackStateMirror* converted_sample_feedback_state(
    iv::NodeStorage const& storage)
{
    if (!storage.layout) return nullptr;
    ConvertedSampleFeedbackStateMirror* result = nullptr;
    for (std::size_t i = 0; i < storage.layout->nodes.size(); ++i) {
        if (storage.layout->nodes[i].state_size
            != sizeof(ConvertedSampleFeedbackStateMirror)) {
            continue;
        }
        auto* candidate = static_cast<ConvertedSampleFeedbackStateMirror*>(
            storage.state_ptr(i));
        if (candidate == nullptr || candidate->marker != 0xc04e7ed1u) continue;
        if (result != nullptr) return nullptr;
        result = candidate;
    }
    return result;
}

void expect_converted_feedback_suffix_equal(
    ConvertedSampleFeedbackStateMirror const& reference,
    std::size_t reference_begin,
    ConvertedSampleFeedbackStateMirror const& actual)
{
    ASSERT_GE(reference.calls, reference_begin);
    auto const suffix_calls = reference.calls - reference_begin;
    std::size_t actual_begin = 0;
    if (actual.calls == reference.calls) {
        // A future node-state migration policy may preserve the probe state.
        // In that case compare only observations produced after migration.
        actual_begin = reference_begin;
    } else {
        ASSERT_EQ(actual.calls, suffix_calls);
    }
    ASSERT_LE(actual_begin + suffix_calls, actual.calls);
    EXPECT_EQ(actual.scc_feedback_latency, reference.scc_feedback_latency);
    EXPECT_EQ(actual.marker, reference.marker);
    for (std::size_t i = 0; i < suffix_calls; ++i) {
        auto const reference_i = reference_begin + i;
        auto const actual_i = actual_begin + i;
        EXPECT_EQ(actual.indices[actual_i], reference.indices[reference_i]);
        EXPECT_EQ(actual.block_sizes[actual_i], reference.block_sizes[reference_i]);
        EXPECT_FLOAT_EQ(
            actual.first_left[actual_i], reference.first_left[reference_i]);
        EXPECT_FLOAT_EQ(
            actual.first_right[actual_i], reference.first_right[reference_i]);
        EXPECT_FLOAT_EQ(
            actual.last_left[actual_i], reference.last_left[reference_i]);
        EXPECT_FLOAT_EQ(
            actual.last_right[actual_i], reference.last_right[reference_i]);
    }
}

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

struct DisconnectedSampleInputProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
    float current_left = 0.0f;
    float current_right = 0.0f;
    float history_left = 0.0f;
    float history_right = 0.0f;
    float block_first = 0.0f;
    float block_last = 0.0f;
    float block_sum = 0.0f;
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

struct RetainedDualEventFeedbackStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t scc_feedback_latency = 0;
    std::array<std::uint64_t, 24> indices{};
    std::array<std::uint64_t, 24> block_sizes{};
    std::array<std::uint64_t, 24> exact_counts{};
    std::array<std::uint64_t, 24> exact_first_times{};
    std::array<std::uint64_t, 24> exact_last_times{};
    std::array<std::uint64_t, 24> converted_counts{};
    std::array<std::uint64_t, 24> converted_first_times{};
    std::array<std::uint64_t, 24> converted_last_times{};
    std::uint64_t marker = 0;
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
    iv::TockStateLayout const& tock_layout,
    std::size_t state_size,
    std::size_t background_state_size)
{
    ASSERT_EQ(layout.nodes.size(), 1u);
    ASSERT_EQ(tock_layout.nodes.size(), 1u);
    auto const& node = layout.nodes.front();
    auto const& tock_node = tock_layout.nodes.front();
    EXPECT_EQ(node.state_size, state_size);
    EXPECT_EQ(tock_node.state_size, background_state_size);
    EXPECT_TRUE(layout.imported_arrays.empty());
    EXPECT_TRUE(layout.exported_arrays.empty());
    EXPECT_EQ(layout.regions.size(), 1u);

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

    EXPECT_EQ(std::ranges::count_if(
        layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::state;
        }), 1u);
}
} // namespace

TEST(GraphJitEventFeedbackRuntime, AppendsEachSourceSuffixExactlyOnce)
{
    std::array<iv::TimedEvent, 4> const source{
        iv::TimedEvent{.time = index(0), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(1), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(5), .value = iv::TriggerEvent{}},
    };
    std::array<iv::TimedEvent, 8> ring{};
    std::size_t read_index = 0;
    std::size_t write_index = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback(
        source.data(), nullptr, 0, 0, 0, 2, index(0), 5,
        ring.data(), ring.size(), &read_index, &write_index);
    iv::graph_jit::detail::iv_graph_jit_append_event_feedback(
        source.data(), nullptr, 0, 0, 2, source.size(), index(4), 5,
        ring.data(), ring.size(), &read_index, &write_index);

    EXPECT_EQ(read_index, 0u);
    ASSERT_EQ(write_index, source.size());
    EXPECT_EQ(ring[0].time, index(5));
    EXPECT_EQ(ring[1].time, index(6));
    EXPECT_EQ(ring[2].time, index(9));
    EXPECT_EQ(ring[3].time, index(10));
}

TEST(GraphJitEventFeedbackRuntime, AppendsWrappedPersistentSourceSuffixExactlyOnce)
{
    std::array<iv::TimedEvent, 4> source{};
    // Monotonic source indices [3, 7) wrap once through this four-index ring.
    source[3] = iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}};
    source[0] = iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}};
    source[1] = iv::TimedEvent{.time = index(5), .value = iv::TriggerEvent{}};
    source[2] = iv::TimedEvent{.time = index(6), .value = iv::TriggerEvent{}};
    std::array<iv::TimedEvent, 8> ring{};
    std::size_t read_index = 0;
    std::size_t write_index = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_ring_source(
        source.data(), nullptr, 0, 0, source.size(), 3, 5, index(3), 5,
        ring.data(), ring.size(), &read_index, &write_index);
    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_ring_source(
        source.data(), nullptr, 0, 0, source.size(), 5, 7, index(5), 5,
        ring.data(), ring.size(), &read_index, &write_index);

    EXPECT_EQ(read_index, 0u);
    ASSERT_EQ(write_index, 4u);
    EXPECT_EQ(ring[0].time, index(8));
    EXPECT_EQ(ring[1].time, index(9));
    EXPECT_EQ(ring[2].time, index(10));
    EXPECT_EQ(ring[3].time, index(11));
}

TEST(GraphJitEventFeedbackRuntime, AppendsLinearFeedbackSuffixExactlyOnce)
{
    std::array<iv::TimedEvent, 4> const source{
        iv::TimedEvent{.time = index(0), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(1), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(5), .value = iv::TriggerEvent{}},
    };
    std::array<iv::TimedEvent, 8> sequence{};
    std::size_t count = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_sequence(
        source.data(), nullptr, 0, 0, 0, 2, index(0), 5,
        sequence.data(), sequence.size(), &count);
    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_sequence(
        source.data(), nullptr, 0, 0, 2, source.size(), index(4), 5,
        sequence.data(), sequence.size(), &count);

    ASSERT_EQ(count, source.size());
    EXPECT_EQ(sequence[0].time, index(5));
    EXPECT_EQ(sequence[1].time, index(6));
    EXPECT_EQ(sequence[2].time, index(9));
    EXPECT_EQ(sequence[3].time, index(10));
}

TEST(GraphJitEventFeedbackRuntime, AppendsWrappedRingSourceIntoLinearFeedback)
{
    std::array<iv::TimedEvent, 4> source{};
    source[3] = iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}};
    source[0] = iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}};
    source[1] = iv::TimedEvent{.time = index(5), .value = iv::TriggerEvent{}};
    source[2] = iv::TimedEvent{.time = index(6), .value = iv::TriggerEvent{}};
    std::array<iv::TimedEvent, 8> sequence{};
    std::size_t count = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_sequence_ring_source(
        source.data(), nullptr, 0, 0, source.size(), 3, 5, index(3), 5,
        sequence.data(), sequence.size(), &count);
    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_sequence_ring_source(
        source.data(), nullptr, 0, 0, source.size(), 5, 7, index(5), 5,
        sequence.data(), sequence.size(), &count);

    ASSERT_EQ(count, 4u);
    EXPECT_EQ(sequence[0].time, index(8));
    EXPECT_EQ(sequence[1].time, index(9));
    EXPECT_EQ(sequence[2].time, index(10));
    EXPECT_EQ(sequence[3].time, index(11));
}

TEST(GraphJitEventFeedbackRuntime, FiltersLinearSourceIntoRingFeedback)
{
    std::array<iv::TimedEvent, 5> const source{
        iv::TimedEvent{.time = index(0), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(1), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(2), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}},
    };
    std::array<std::size_t, 5> const source_indices{0, 1, 0, 1, 0};
    std::array<iv::TimedEvent, 8> ring{};
    std::size_t read_index = 0;
    std::size_t write_index = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback(
        source.data(), source_indices.data(), 1, 1,
        0, source.size(), index(0), 5,
        ring.data(), ring.size(), &read_index, &write_index);

    EXPECT_EQ(read_index, 0u);
    ASSERT_EQ(write_index, 2u);
    EXPECT_EQ(ring[0].time, index(6));
    EXPECT_EQ(ring[1].time, index(8));
}

TEST(GraphJitEventFeedbackRuntime, FiltersLinearSourceIntoSequenceFeedback)
{
    std::array<iv::TimedEvent, 5> const source{
        iv::TimedEvent{.time = index(0), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(1), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(2), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}},
    };
    std::array<std::size_t, 5> const source_indices{0, 1, 0, 1, 0};
    std::array<iv::TimedEvent, 8> sequence{};
    std::size_t count = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_sequence(
        source.data(), source_indices.data(), 1, 1,
        0, source.size(), index(0), 5,
        sequence.data(), sequence.size(), &count);

    ASSERT_EQ(count, 2u);
    EXPECT_EQ(sequence[0].time, index(6));
    EXPECT_EQ(sequence[1].time, index(8));
}

TEST(GraphJitEventFeedbackRuntime, FiltersRingSourceIntoRingFeedback)
{
    std::array<iv::TimedEvent, 4> source{};
    std::array<std::size_t, 4> source_indices{};
    source[3] = iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}};
    source_indices[3] = 0;
    source[0] = iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}};
    source_indices[0] = 1;
    source[1] = iv::TimedEvent{.time = index(5), .value = iv::TriggerEvent{}};
    source_indices[1] = 0;
    source[2] = iv::TimedEvent{.time = index(6), .value = iv::TriggerEvent{}};
    source_indices[2] = 1;
    std::array<iv::TimedEvent, 8> ring{};
    std::size_t read_index = 0;
    std::size_t write_index = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_ring_source(
        source.data(), source_indices.data(), 1, 1,
        source.size(), 3, 7, index(3), 5,
        ring.data(), ring.size(), &read_index, &write_index);

    EXPECT_EQ(read_index, 0u);
    ASSERT_EQ(write_index, 2u);
    EXPECT_EQ(ring[0].time, index(9));
    EXPECT_EQ(ring[1].time, index(11));
}

TEST(GraphJitEventFeedbackRuntime, FiltersRingSourceIntoSequenceFeedback)
{
    std::array<iv::TimedEvent, 4> source{};
    std::array<std::size_t, 4> source_indices{};
    source[3] = iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}};
    source_indices[3] = 0;
    source[0] = iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}};
    source_indices[0] = 1;
    source[1] = iv::TimedEvent{.time = index(5), .value = iv::TriggerEvent{}};
    source_indices[1] = 0;
    source[2] = iv::TimedEvent{.time = index(6), .value = iv::TriggerEvent{}};
    source_indices[2] = 1;
    std::array<iv::TimedEvent, 8> sequence{};
    std::size_t count = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_sequence_ring_source(
        source.data(), source_indices.data(), 1, 1,
        source.size(), 3, 7, index(3), 5,
        sequence.data(), sequence.size(), &count);

    ASSERT_EQ(count, 2u);
    EXPECT_EQ(sequence[0].time, index(9));
    EXPECT_EQ(sequence[1].time, index(11));
}

TEST(GraphJitEventFeedbackRuntime, MaterializesOneSelectedSource)
{
    std::array<iv::TimedEvent, 4> const source{
        iv::TimedEvent{.time = index(0), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(1), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(2), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}},
    };
    std::array<std::size_t, 4> const source_indices{0, 1, 0, 1};
    std::array<iv::TimedEvent, 4> target{};

    auto const count =
        iv::graph_jit::detail::iv_graph_jit_materialize_event_sequence(
            static_cast<std::uint32_t>(iv::EventTypeId::trigger),
            static_cast<std::uint32_t>(iv::EventTypeId::trigger),
            0, 0, 0, 0,
            source.data(), source_indices.data(), 1, 1,
            source.size(), 0, source.size(), index(0), 4, 0,
            target.data(), target.size());

    ASSERT_EQ(count, 2u);
    EXPECT_EQ(target[0].time, index(1));
    EXPECT_EQ(target[1].time, index(3));
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

TEST(GraphJit, EmptyGraphCompilesAndMaterializesRootOperation)
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
    EXPECT_TRUE(result.compiled_graph->background_evaluation_plan.empty());
    EXPECT_TRUE(result.compiled_graph->root_operations.valid());
    EXPECT_FALSE(result.compiled_graph->background_operations.valid());

    EXPECT_NO_THROW(result.compiled_graph->root_operations.tick_block(nullptr, &empty_tick_invocation, index(0), 256));
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
    // historical index width does not add another 3 * 256 bytes.
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
    // surrounding ranges are still live; a whole-index allocator would append.
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

TEST(GraphJitTransientArenaPlan, InclusiveLifetimePortsNeverAlias)
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

TEST(GraphJitSampleStoragePlan, PacksExactTransientByteRangesAcrossLifetimes)
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
        result.storage_plan = iv::SampleConnectionStoragePlan{
            iv::RealtimeBufferStorageKind::transient_stack};
        result.live_interval = ConnectionLiveIntervalPlan{
            .begin = begin,
            .end = end,
        };
        return result;
    };

    ConnectionAnalysisPlan connections;
    // A large early value, followed by two simultaneous half-sized values. The
    // latter should split the dead large range instead of reserving one large
    // historical index plus a second small index.
    connections.sample_producer_groups.push_back(group(stereo, 0, 0));
    connections.sample_producer_groups.push_back(group(mono, 1, 2));
    connections.sample_producer_groups.push_back(group(mono, 1, 2));

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 3u);
    ASSERT_EQ(storage->transient_allocations.size(), 3u);

    for (auto const& producer : storage->producer_groups) {
        ASSERT_TRUE(producer.has_value());
        ASSERT_LT(
            producer->canonical_representation,
            storage->representations.size());
        auto const allocation = storage->representations[
            producer->canonical_representation].transient_allocation;
        ASSERT_NE(allocation, no_sample_transient_allocation);
        ASSERT_LT(allocation, storage->transient_allocations.size());
    }
    auto allocation_for_group = [&](std::size_t group_index)
        -> SampleTransientAllocationPlan const& {
        auto const representation_index =
            storage->producer_groups[group_index]->canonical_representation;
        auto const allocation =
            storage->representations[representation_index].transient_allocation;
        return storage->transient_allocations[allocation];
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
    EXPECT_EQ(storage->transient_arena_size, stereo_bytes);

    iv::NodeLayoutBuilder builder(64);
    auto declared = declare_sample_storage(builder, *storage);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *storage);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    EXPECT_TRUE(storage->persistent_allocations.empty());
    ASSERT_EQ(storage->sequential_pace_cursors.size(), 1u);
    EXPECT_EQ(layout.regions.size(), storage->sequential_pace_cursors.size());
    EXPECT_EQ(storage->sequential_pace_cursors[0].absolute_pace_exponent, 0);
    ASSERT_TRUE(storage->sequential_pace_cursors[0].region.valid());
    ASSERT_LT(
        storage->sequential_pace_cursors[0].region.index,
        layout.regions.size());
}

TEST(GraphJitSampleStoragePlan, LeavesBackgroundAccessBranchesUnresolved)
{
    using namespace iv::graph_jit::detail;

    ConnectionAnalysisPlan connections;
    SampleConnectionPlan realtime;
    realtime.source_channel_timings = {SampleSourceChannelTimingPlan{}};
    realtime.target_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    connections.sample_connections.push_back(std::move(realtime));
    SampleConnectionPlan background;
    background.destination_access = PlannedDestinationAccess::random_access;
    background.source_channel_timings = {SampleSourceChannelTimingPlan{
        .destination_access = PlannedDestinationAccess::random_access,
        .delivery = PlannedDeliveryMechanism::tock_to_random_access,
    }};
    connections.sample_connections.push_back(std::move(background));

    SampleProducerGroupPlan realtime_group;
    realtime_group.canonical_source_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    realtime_group.connection_indices = {0};
    realtime_group.has_realtime_connections = true;
    realtime_group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::transient_stack};
    realtime_group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
    };
    connections.sample_producer_groups.push_back(std::move(realtime_group));

    SampleProducerGroupPlan background_group;
    background_group.connection_indices = {1};
    background_group.has_background_connections = true;
    connections.sample_producer_groups.push_back(std::move(background_group));

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->connection_representations.size(), 2u);
    ASSERT_TRUE(storage->connection_representations[0].has_value());
    EXPECT_FALSE(storage->connection_representations[1].has_value());
    ASSERT_EQ(storage->producer_groups.size(), 2u);
    ASSERT_TRUE(storage->producer_groups[0].has_value());
    EXPECT_FALSE(storage->producer_groups[1].has_value());
    EXPECT_EQ(
        *storage->connection_representations[0],
        storage->producer_groups[0]->canonical_representation);
}

TEST(GraphJitSampleStoragePlan, AliasesLayoutAndMonoToStereoConvertedFanout)
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
    identity.source_channel_timings = {SampleSourceChannelTimingPlan{}};
    identity.canonical_source_layout = mono;
    identity.target_layout = mono;
    identity.target_port = iv::NodeBundlePortId{2, iv::PortKind::sample, 0};
    connections.sample_connections.push_back(identity);

    SampleConnectionPlan converted_a;
    converted_a.source_channel_timings = {SampleSourceChannelTimingPlan{}};
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
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::transient_stack};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 3,
    };
    connections.sample_producer_groups.push_back(std::move(group));

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 1u);
    EXPECT_TRUE(storage->materializations.empty());
    ASSERT_EQ(storage->connection_representations.size(), 3u);
    ASSERT_EQ(storage->connection_channel_bindings.size(), 3u);
    ASSERT_TRUE(storage->producer_groups[0].has_value());
    auto const canonical =
        storage->producer_groups[0]->canonical_representation;
    ASSERT_TRUE(storage->connection_representations[0].has_value());
    EXPECT_EQ(*storage->connection_representations[0], canonical);
    EXPECT_FALSE(storage->connection_representations[1].has_value());
    EXPECT_FALSE(storage->connection_representations[2].has_value());
    ASSERT_TRUE(storage->connection_channel_bindings[1].has_value());
    ASSERT_TRUE(storage->connection_channel_bindings[2].has_value());
    for (auto connection_index : {1u, 2u}) {
        auto const& channels =
            *storage->connection_channel_bindings[connection_index];
        ASSERT_EQ(channels.size(), 2u);
        for (auto const& channel : channels) {
            EXPECT_EQ(channel.representation, canonical);
            EXPECT_EQ(channel.representation_channel, 0u);
            EXPECT_EQ(channel.frame_delay, 0u);
        }
    }
    EXPECT_TRUE(storage->representations[canonical]
                    .canonical_producer_representation);
    EXPECT_EQ(storage->representations[canonical].channel_layout, mono);
    EXPECT_EQ(storage->representations[canonical].live_interval.begin, 0u);
    EXPECT_EQ(storage->representations[canonical].live_interval.end, 3u);

    auto const canonical_allocation = storage->representations[canonical]
        .transient_allocation;
    ASSERT_LT(canonical_allocation, storage->transient_allocations.size());
    EXPECT_EQ(storage->transient_allocations[canonical_allocation].size_bytes,
        64u * sizeof(iv::Sample));
    EXPECT_EQ(storage->transient_arena_size, 64u * sizeof(iv::Sample));
}

TEST(GraphJitSampleStoragePlan, RealizesCompactPersistentCarryExactly)
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
    group.storage_requirements = iv::SampleConnectionStorageRequirements{
        .current_block_frames = 64,
        .retained_frames = 7,
        .channel_count = 1,
    };
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 1u);
    ASSERT_EQ(storage->transient_allocations.size(), 1u);
    ASSERT_EQ(storage->persistent_allocations.size(), 1u);
    ASSERT_EQ(storage->carry_operations.size(), 1u);
    ASSERT_TRUE(storage->producer_groups[0].has_value());
    auto const representation_index =
        storage->producer_groups[0]->canonical_representation;
    auto const& representation = storage->representations[representation_index];
    EXPECT_EQ(representation.frame_capacity, 128u);
    EXPECT_NE(
        representation.transient_allocation,
        no_sample_transient_allocation);
    EXPECT_NE(
        representation.persistent_allocation,
        no_sample_persistent_allocation);

    auto const& persistent = storage->persistent_allocations[
        representation.persistent_allocation];
    EXPECT_EQ(persistent.kind, SamplePersistentStorageKind::compact_carry);
    EXPECT_EQ(persistent.retained_frames, 7u);
    EXPECT_EQ(persistent.frame_capacity, 128u);
    EXPECT_EQ(persistent.size_bytes, 7u * sizeof(iv::Sample));
    EXPECT_FALSE(persistent.migration_identity.empty());
    EXPECT_EQ(
        storage->carry_operations[0].persistent_allocation,
        representation.persistent_allocation);
    EXPECT_EQ(storage->carry_operations[0].retained_frames, 7u);

    iv::NodeLayoutBuilder builder(64);
    auto declared = declare_sample_storage(builder, *storage);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *storage);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    ASSERT_EQ(layout.regions.size(), 2u);
    auto const& persistent_region = layout.regions[persistent.region.index];
    EXPECT_EQ(
        persistent_region.migration_identity,
        persistent.migration_identity);
    EXPECT_EQ(persistent_region.size, 7u * sizeof(iv::Sample));
}

TEST(GraphJitSampleStoragePlan, RealizesLargeRetentionAsPersistentRing)
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
    group.storage_requirements = iv::SampleConnectionStorageRequirements{
        .current_block_frames = 64,
        .retained_frames = 5000,
        .channel_count = 1,
    };
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::full_node_storage};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto first = build_sample_storage_plan(connections, 64);
    auto second = build_sample_storage_plan(connections, 64);
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
    auto const& storage = first->representations[representation_index];
    EXPECT_EQ(storage.frame_capacity, 8192u);
    EXPECT_EQ(
        storage.transient_allocation,
        no_sample_transient_allocation);
    ASSERT_NE(
        storage.persistent_allocation,
        no_sample_persistent_allocation);
    auto const& persistent = first->persistent_allocations[
        storage.persistent_allocation];
    EXPECT_EQ(persistent.kind, SamplePersistentStorageKind::ring);
    EXPECT_EQ(persistent.retained_frames, 5000u);
    EXPECT_EQ(persistent.frame_capacity, 8192u);
    EXPECT_EQ(persistent.size_bytes, 8192u * sizeof(iv::Sample));
    EXPECT_EQ(
        persistent.migration_identity,
        second->persistent_allocations[0].migration_identity);

    iv::NodeLayoutBuilder builder(64);
    auto declared = declare_sample_storage(builder, *first);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *first);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    ASSERT_EQ(layout.regions.size(), 2u);
    auto const& persistent_region = layout.regions[persistent.region.index];
    EXPECT_EQ(persistent_region.size, 8192u * sizeof(iv::Sample));
    EXPECT_FALSE(persistent_region.migration_identity.empty());
}

TEST(GraphJitSampleStoragePlan, RealizesDetachedBranchAsPersistentFeedbackRing)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::SampleOutputChannelId const source_channel{
        .bundle = 1,
        .port = 0,
        .channel = 0,
    };
    iv::NodeBundlePortId const source_port{1, iv::PortKind::sample, 0};

    ConnectionAnalysisPlan connections;
    connections.schedule.bundle_execution_position.resize(3);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;

    SampleConnectionPlan feedback;
    feedback.source_type = iv::ChannelTypeId::mono;
    feedback.source_channels = {source_channel};
    feedback.source_channel_timings = {SampleSourceChannelTimingPlan{
        .source = source_channel,
        .source_layout = mono,
        .source_latency = 2,
        .read_latency = 2,
    }};
    feedback.canonical_source_port = source_port;
    feedback.canonical_source_layout = mono;
    feedback.target_type = iv::ChannelTypeId::mono;
    feedback.target_layout = mono;
    feedback.target_channels = {iv::SampleInputChannelId{
        .bundle = 2,
        .port = 0,
        .channel = 0,
    }};
    feedback.target_port = iv::NodeBundlePortId{2, iv::PortKind::sample, 0};
    feedback.source_latency = 2;
    feedback.read_latency = 2;
    feedback.target_history = 3;
    feedback.detach = iv::ConfiguredSampleConnectionDetach{
        .loop_extra_latency = 5,
        .initial_value_override = iv::Sample{0.25f},
    };
    feedback.detach_initial_value = sample_value_bytes(iv::Sample{0.25f});
    connections.sample_connections.push_back(feedback);

    SampleProducerGroupPlan group;
    group.source_port = source_port;
    group.source_type = iv::ChannelTypeId::mono;
    group.source_channels = {source_channel};
    group.canonical_source_layout = mono;
    group.connection_indices = {0};
    group.has_realtime_connections = true;
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::transient_stack};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(connections, 8);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 2u);
    ASSERT_EQ(storage->transient_allocations.size(), 1u);
    ASSERT_EQ(storage->persistent_allocations.size(), 1u);
    ASSERT_EQ(storage->feedback_timelines.size(), 1u);
    ASSERT_EQ(storage->producer_authored_extents.size(), 1u);
    EXPECT_EQ(
        storage->producer_authored_extents.front().maximum_revision_frames,
        2u);
    ASSERT_TRUE(storage->connection_representations[0].has_value());

    auto const canonical = storage->producer_groups[0]->canonical_representation;
    auto const ring = *storage->connection_representations[0];
    ASSERT_NE(canonical, ring);
    EXPECT_EQ(
        storage->representations[canonical].storage,
        iv::RealtimeBufferStorageKind::transient_stack);
    EXPECT_EQ(
        storage->representations[ring].storage,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(storage->representations[ring].frame_capacity, 32u);
    EXPECT_EQ(
        storage->representations[ring].transient_allocation,
        no_sample_transient_allocation);
    ASSERT_NE(
        storage->representations[ring].persistent_allocation,
        no_sample_persistent_allocation);

    auto const& persistent = storage->persistent_allocations[
        storage->representations[ring].persistent_allocation];
    EXPECT_EQ(persistent.kind, SamplePersistentStorageKind::ring);
    EXPECT_EQ(persistent.retained_frames, 10u);
    EXPECT_EQ(persistent.frame_capacity, 32u);
    EXPECT_EQ(persistent.size_bytes, 32u * sizeof(iv::Sample));
    ASSERT_TRUE(persistent.initialize_value.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(
            *persistent.initialize_value)),
        0.25f);
    EXPECT_NE(
        persistent.migration_identity.find("graphjit.sample.feedback:"),
        std::string::npos);

    auto const& timeline = storage->feedback_timelines[0];
    EXPECT_EQ(timeline.connection_index, 0u);
    EXPECT_EQ(timeline.timeline_representation, ring);
    EXPECT_EQ(timeline.channel_layout, mono);
    EXPECT_EQ(timeline.retained_frames, 10u);
    EXPECT_EQ(timeline.loop_extra_latency, 5u);
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(timeline.initial_value)),
        0.25f);
    EXPECT_EQ(timeline.writer.kind, SampleFeedbackTimelineWriterKind::copy);
    EXPECT_EQ(timeline.writer.revision_frames, 2u);
    EXPECT_EQ(timeline.writer.source_representation, canonical);
    EXPECT_EQ(timeline.writer.after_execution_position, 0u);
    EXPECT_TRUE(timeline.writer.composition_contributions.empty());
    EXPECT_EQ(
        timeline.storage_plan.candidate_costs.full_node_storage.copied_bytes,
        10u * sizeof(iv::Sample));
    EXPECT_EQ(
        timeline.storage_plan.candidate_costs.full_node_storage
            .ring_addressed_bytes,
        10u * sizeof(iv::Sample));

    iv::NodeLayoutBuilder builder(8);
    auto declared = declare_sample_storage(builder, *storage);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *storage);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    ASSERT_EQ(layout.regions.size(), 3u);
    auto const& persistent_region = layout.regions[persistent.region.index];
    EXPECT_EQ(persistent_region.size, 32u * sizeof(iv::Sample));
    EXPECT_EQ(
        persistent_region.migration_identity,
        persistent.migration_identity);
    ASSERT_NE(persistent_region.raw_initialize_fn, nullptr);
    EXPECT_EQ(persistent_region.raw_initialize_data.size(), sizeof(iv::Sample));

    iv::ResourceContext resources;
    auto node_storage = layout.create_storage(resources);
    node_storage.initialize();
    auto const bytes = node_storage.region_bytes(persistent.region);
    ASSERT_EQ(bytes.size(), 32u * sizeof(iv::Sample));
    auto const initialized = std::span<iv::Sample const>{
        reinterpret_cast<iv::Sample const*>(bytes.data()),
        bytes.size() / sizeof(iv::Sample)};
    for (auto const sample : initialized) {
        EXPECT_FLOAT_EQ(static_cast<float>(sample), 0.25f);
    }
}

TEST(GraphJitSampleStoragePlan, ZeroInitializedFeedbackUsesProducerHomeAndCopiesOnlyIncompatibleBranches)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::SampleOutputChannelId const source_channel{
        .bundle = 1,
        .port = 0,
        .channel = 0,
    };
    iv::NodeBundlePortId const source_port{1, iv::PortKind::sample, 0};

    ConnectionAnalysisPlan connections;
    connections.schedule.bundle_execution_position.resize(5);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;
    connections.schedule.bundle_execution_position[3] = 2;
    connections.schedule.bundle_execution_position[4] = 3;

    auto make_feedback = [&](std::size_t target_bundle,
                             std::size_t latency,
                             iv::Sample initial_value) {
        SampleConnectionPlan feedback;
        feedback.source_type = iv::ChannelTypeId::mono;
        feedback.source_channels = {source_channel};
        feedback.source_channel_timings = {SampleSourceChannelTimingPlan{
            .source = source_channel,
            .source_layout = mono,
        }};
        feedback.canonical_source_port = source_port;
        feedback.canonical_source_layout = mono;
        feedback.target_type = iv::ChannelTypeId::mono;
        feedback.target_layout = mono;
        feedback.target_channels = {iv::SampleInputChannelId{
            .bundle = target_bundle,
            .port = 0,
            .channel = 0,
        }};
        feedback.target_port = iv::NodeBundlePortId{
            target_bundle, iv::PortKind::sample, 0};
        feedback.detach = iv::ConfiguredSampleConnectionDetach{
            .loop_extra_latency = latency,
            .initial_value_override = initial_value,
        };
        feedback.detach_initial_value = sample_value_bytes(initial_value);
        return feedback;
    };

    connections.sample_connections.push_back(
        make_feedback(2, 5, iv::Sample{0.0f}));
    connections.sample_connections.push_back(
        make_feedback(3, 7, iv::Sample{0.5f}));
    connections.sample_connections.push_back(
        make_feedback(4, 12, iv::Sample{0.0f}));

    SampleProducerGroupPlan group;
    group.source_port = source_port;
    group.source_type = iv::ChannelTypeId::mono;
    group.source_channels = {source_channel};
    group.canonical_source_layout = mono;
    group.connection_indices = {0, 1, 2};
    group.has_realtime_connections = true;
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::transient_stack};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 3,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(connections, 8);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 2u);
    ASSERT_EQ(storage->transient_allocations.size(), 1u);
    ASSERT_EQ(storage->persistent_allocations.size(), 2u);
    ASSERT_EQ(storage->feedback_timelines.size(), 3u);

    auto const canonical =
        storage->producer_groups[0]->canonical_representation;
    ASSERT_LT(canonical, storage->representations.size());
    EXPECT_TRUE(
        storage->representations[canonical].canonical_producer_representation);
    EXPECT_EQ(
        storage->representations[canonical].storage,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(storage->representations[canonical].frame_capacity, 32u);
    ASSERT_TRUE(storage->connection_representations[0].has_value());
    ASSERT_TRUE(storage->connection_representations[2].has_value());
    EXPECT_EQ(*storage->connection_representations[0], canonical);
    EXPECT_EQ(*storage->connection_representations[2], canonical);

    auto const copied_ring = *storage->connection_representations[1];
    ASSERT_NE(copied_ring, canonical);
    EXPECT_EQ(
        storage->representations[copied_ring].storage,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    auto const copied_persistent_index =
        storage->representations[copied_ring].persistent_allocation;
    ASSERT_LT(copied_persistent_index, storage->persistent_allocations.size());
    EXPECT_EQ(
        storage->persistent_allocations[copied_persistent_index].kind,
        SamplePersistentStorageKind::compact_carry);
    EXPECT_EQ(
        storage->persistent_allocations[copied_persistent_index].retained_frames,
        7u);
    auto const copied = std::ranges::find_if(
        storage->feedback_timelines,
        [](SampleFeedbackTimelinePlan const& timeline) {
            return timeline.connection_index == 1;
        });
    ASSERT_NE(copied, storage->feedback_timelines.end());
    EXPECT_EQ(copied->timeline_representation, copied_ring);
    EXPECT_EQ(copied->writer.kind, SampleFeedbackTimelineWriterKind::copy);
    EXPECT_EQ(copied->writer.source_representation, canonical);
    EXPECT_EQ(copied->writer.after_execution_position, 0u);
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(copied->initial_value)),
        0.5f);

    auto const producer_home_count = std::ranges::count_if(
        storage->feedback_timelines,
        [](SampleFeedbackTimelinePlan const& timeline) {
            return timeline.writer.kind
                == SampleFeedbackTimelineWriterKind::producer_home;
        });
    EXPECT_EQ(producer_home_count, 2u);

    auto const home_persistent_index =
        storage->representations[canonical].persistent_allocation;
    ASSERT_LT(home_persistent_index, storage->persistent_allocations.size());
    auto const& home = storage->persistent_allocations[home_persistent_index];
    EXPECT_EQ(home.retained_frames, 12u);
    EXPECT_EQ(home.frame_capacity, 32u);
    ASSERT_TRUE(home.initialize_value.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(*home.initialize_value)),
        0.0f);
    EXPECT_NE(home.migration_identity.find("graphjit.sample:"), std::string::npos);
    EXPECT_EQ(
        home.migration_identity.find("graphjit.sample.feedback:"),
        std::string::npos);

    iv::NodeLayoutBuilder builder(8);
    auto declared = declare_sample_storage(builder, *storage);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *storage);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    auto const& home_region = layout.regions[
        storage->persistent_allocations[home_persistent_index].region.index];
    ASSERT_NE(home_region.raw_initialize_fn, nullptr);
    EXPECT_EQ(home_region.raw_initialize_data.size(), sizeof(iv::Sample));
}

TEST(GraphJitSampleStoragePlan, DetachedCompositionUsesPersistentShiftedTimeline)
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
    iv::SampleOutputChannelId const left_source{
        .bundle = 1,
        .port = 0,
        .channel = 0,
    };
    iv::SampleOutputChannelId const right_source{
        .bundle = 2,
        .port = 0,
        .channel = 0,
    };

    ConnectionAnalysisPlan connections;
    connections.schedule.bundle_execution_position.resize(4);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;
    connections.schedule.bundle_execution_position[3] = 2;

    SampleConnectionPlan feedback;
    feedback.source_type = iv::ChannelTypeId::stereo;
    feedback.source_channels = {left_source, right_source};
    feedback.source_channel_timings = {
        SampleSourceChannelTimingPlan{
            .source = left_source,
            .source_layout = mono,
            .source_latency = 7,
            .read_latency = 7,
        },
        SampleSourceChannelTimingPlan{
            .source = right_source,
            .source_layout = mono,
            .source_latency = 2,
            .read_latency = 2,
        },
    };
    feedback.projection_contributions = {
        SampleProjectionContributionPlan{
            .source_type = iv::ChannelTypeId::mono,
            .source_channel_indices = {0},
            .target_type = iv::ChannelTypeId::mono,
            .target_channels = {0},
        },
        SampleProjectionContributionPlan{
            .source_type = iv::ChannelTypeId::mono,
            .source_channel_indices = {1},
            .target_type = iv::ChannelTypeId::mono,
            .target_channels = {1},
        },
    };
    feedback.target_type = iv::ChannelTypeId::stereo;
    feedback.target_layout = stereo;
    feedback.target_channels = {
        iv::SampleInputChannelId{.bundle = 3, .port = 0, .channel = 0},
        iv::SampleInputChannelId{.bundle = 3, .port = 0, .channel = 1},
    };
    feedback.target_port = iv::NodeBundlePortId{3, iv::PortKind::sample, 0};
    feedback.source_latency = 7;
    feedback.read_latency = 7;
    feedback.target_history = 3;
    feedback.requires_conversion = true;
    feedback.detach = iv::ConfiguredSampleConnectionDetach{
        .loop_extra_latency = 5,
        .initial_value_override = iv::Sample{0.25f},
    };
    feedback.detach_initial_value = sample_value_bytes(iv::Sample{0.25f});
    connections.sample_connections.push_back(feedback);

    auto append_group = [&](iv::SampleOutputChannelId source,
                            std::size_t begin) {
        SampleProducerGroupPlan group;
        group.source_port = iv::NodeBundlePortId{
            source.bundle, iv::PortKind::sample, source.port};
        group.source_type = iv::ChannelTypeId::mono;
        group.source_channels = {source};
        group.canonical_source_layout = mono;
        group.connection_indices = {0};
        group.has_realtime_connections = true;
        group.storage_plan = iv::SampleConnectionStoragePlan{
            iv::RealtimeBufferStorageKind::transient_stack};
        group.live_interval = ConnectionLiveIntervalPlan{
            .begin = begin,
            .end = 2,
            .crosses_kernel_invocations = true,
        };
        connections.sample_producer_groups.push_back(std::move(group));
    };
    append_group(left_source, 0);
    append_group(right_source, 1);

    auto storage = build_sample_storage_plan(connections, 8);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->producer_groups.size(), 2u);
    EXPECT_TRUE(storage->compositions.empty());
    ASSERT_EQ(storage->feedback_timelines.size(), 1u);
    ASSERT_EQ(storage->persistent_allocations.size(), 1u);
    ASSERT_TRUE(storage->connection_representations[0].has_value());

    auto const composed = *storage->connection_representations[0];
    ASSERT_LT(composed, storage->representations.size());
    auto const& representation = storage->representations[composed];
    EXPECT_FALSE(representation.canonical_producer_representation);
    EXPECT_EQ(
        representation.storage,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(representation.channel_layout, stereo);
    EXPECT_EQ(representation.frame_capacity, 32u);
    EXPECT_EQ(
        representation.transient_allocation,
        no_sample_transient_allocation);
    ASSERT_NE(
        representation.persistent_allocation,
        no_sample_persistent_allocation);

    auto const& persistent = storage->persistent_allocations[
        representation.persistent_allocation];
    EXPECT_EQ(persistent.kind, SamplePersistentStorageKind::ring);
    EXPECT_EQ(persistent.channel_layout, stereo);
    EXPECT_EQ(persistent.retained_frames, 15u);
    EXPECT_EQ(persistent.frame_capacity, 32u);
    ASSERT_TRUE(persistent.initialize_value.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(
            *persistent.initialize_value)),
        0.25f);
    EXPECT_NE(
        persistent.migration_identity.find("graphjit.sample.composed_feedback:"),
        std::string::npos);

    auto const& timeline = storage->feedback_timelines.front();
    EXPECT_EQ(timeline.connection_index, 0u);
    EXPECT_EQ(timeline.timeline_representation, composed);
    EXPECT_EQ(timeline.channel_layout, stereo);
    EXPECT_EQ(timeline.retained_frames, 15u);
    EXPECT_EQ(timeline.loop_extra_latency, 5u);
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(timeline.initial_value)),
        0.25f);
    EXPECT_EQ(
        timeline.writer.kind,
        SampleFeedbackTimelineWriterKind::composition);
    EXPECT_EQ(timeline.writer.revision_frames, 7u);
    EXPECT_EQ(timeline.writer.after_execution_position, 1u);
    EXPECT_EQ(
        timeline.writer.source_representation,
        no_sample_representation);
    ASSERT_EQ(timeline.writer.composition_contributions.size(), 2u);
    auto const& left = timeline.writer.composition_contributions[0];
    EXPECT_EQ(left.source_layout.channel_type, iv::ChannelTypeId::mono);
    EXPECT_EQ(left.converted_layout.channel_type, iv::ChannelTypeId::mono);
    ASSERT_EQ(left.sources.size(), 1u);
    EXPECT_EQ(left.sources[0].source_channel, 0u);
    EXPECT_EQ(left.sources[0].read_latency, 7u);
    EXPECT_EQ(left.target_channels, std::vector<std::size_t>{0u});
    auto const& right = timeline.writer.composition_contributions[1];
    EXPECT_EQ(right.source_layout.channel_type, iv::ChannelTypeId::mono);
    EXPECT_EQ(right.converted_layout.channel_type, iv::ChannelTypeId::mono);
    ASSERT_EQ(right.sources.size(), 1u);
    EXPECT_EQ(right.sources[0].source_channel, 0u);
    EXPECT_EQ(right.sources[0].read_latency, 2u);
    EXPECT_EQ(right.target_channels, std::vector<std::size_t>{1u});

    iv::NodeLayoutBuilder builder(8);
    auto declared = declare_sample_storage(builder, *storage);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *storage);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    auto const& region = layout.regions[persistent.region.index];
    ASSERT_NE(region.raw_initialize_fn, nullptr);

    iv::ResourceContext resources;
    auto node_storage = layout.create_storage(resources);
    node_storage.initialize();
    auto const bytes = node_storage.region_bytes(persistent.region);
    auto const initialized = std::span<iv::Sample const>{
        reinterpret_cast<iv::Sample const*>(bytes.data()),
        bytes.size() / sizeof(iv::Sample)};
    ASSERT_FALSE(initialized.empty());
    for (auto const sample : initialized) {
        EXPECT_FLOAT_EQ(static_cast<float>(sample), 0.25f);
    }
}

TEST(GraphJitSampleStoragePlan, DetachedMixingAlignsUnequalSourceLatencies)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::SampleOutputChannelId const slow_source{
        .bundle = 1,
        .port = 0,
        .channel = 0,
    };
    iv::SampleOutputChannelId const fast_source{
        .bundle = 2,
        .port = 0,
        .channel = 0,
    };

    ConnectionAnalysisPlan connections;
    connections.schedule.bundle_execution_position.resize(4);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;
    connections.schedule.bundle_execution_position[3] = 2;

    SampleConnectionPlan feedback;
    feedback.source_type = iv::ChannelTypeId::stereo;
    feedback.source_channels = {slow_source, fast_source};
    feedback.source_channel_timings = {
        SampleSourceChannelTimingPlan{
            .source = slow_source,
            .source_layout = mono,
            .source_latency = 7,
            .read_latency = 7,
        },
        SampleSourceChannelTimingPlan{
            .source = fast_source,
            .source_layout = mono,
            .source_latency = 2,
            .read_latency = 2,
        },
    };
    feedback.projection_contributions = {
        SampleProjectionContributionPlan{
            .source_type = iv::ChannelTypeId::stereo,
            .source_channel_indices = {0, 1},
            .target_type = iv::ChannelTypeId::mono,
            .target_channels = {0},
        },
    };
    feedback.target_type = iv::ChannelTypeId::mono;
    feedback.target_layout = mono;
    feedback.target_channels = {
        iv::SampleInputChannelId{.bundle = 3, .port = 0, .channel = 0},
    };
    feedback.target_port = iv::NodeBundlePortId{3, iv::PortKind::sample, 0};
    feedback.source_latency = 7;
    feedback.read_latency = 7;
    feedback.target_history = 3;
    feedback.requires_conversion = true;
    feedback.detach = iv::ConfiguredSampleConnectionDetach{
        .loop_extra_latency = 5,
        .initial_value_override = iv::Sample{0.25f},
    };
    feedback.detach_initial_value = sample_value_bytes(iv::Sample{0.25f});
    connections.sample_connections.push_back(feedback);

    auto append_group = [&](iv::SampleOutputChannelId source,
                            std::size_t begin) {
        SampleProducerGroupPlan group;
        group.source_port = iv::NodeBundlePortId{
            source.bundle, iv::PortKind::sample, source.port};
        group.source_type = iv::ChannelTypeId::mono;
        group.source_channels = {source};
        group.canonical_source_layout = mono;
        group.connection_indices = {0};
        group.has_realtime_connections = true;
        group.storage_plan = iv::SampleConnectionStoragePlan{
            iv::RealtimeBufferStorageKind::transient_stack};
        group.live_interval = ConnectionLiveIntervalPlan{
            .begin = begin,
            .end = 2,
            .crosses_kernel_invocations = true,
        };
        connections.sample_producer_groups.push_back(std::move(group));
    };
    append_group(slow_source, 0);
    append_group(fast_source, 1);

    auto storage = build_sample_storage_plan(connections, 8);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->feedback_timelines.size(), 1u);
    ASSERT_EQ(storage->persistent_allocations.size(), 2u);

    auto const& contribution = storage->feedback_timelines.front()
        .writer.composition_contributions.front();
    ASSERT_NE(
        contribution.feedback_alignment_representation,
        no_sample_representation);
    EXPECT_EQ(contribution.feedback_alignment_write_latency, 2u);
    ASSERT_LT(
        contribution.feedback_alignment_representation,
        storage->representations.size());
    auto const& alignment = storage->representations[
        contribution.feedback_alignment_representation];
    EXPECT_EQ(
        alignment.storage,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(alignment.channel_layout.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(alignment.frame_capacity, 16u);
    ASSERT_LT(
        alignment.persistent_allocation,
        storage->persistent_allocations.size());
    auto const& alignment_allocation = storage->persistent_allocations[
        alignment.persistent_allocation];
    EXPECT_EQ(alignment_allocation.retained_frames, 5u);
    ASSERT_TRUE(alignment_allocation.initialize_value.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(
            *alignment_allocation.initialize_value)),
        0.25f);

    iv::NodeLayoutBuilder builder(8);
    auto declared = declare_sample_storage(builder, *storage);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *storage);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());

    auto const& alignment_region =
        layout.regions[alignment_allocation.region.index];
    ASSERT_NE(alignment_region.raw_initialize_fn, nullptr);
    EXPECT_EQ(
        alignment_region.raw_initialize_data.size(), sizeof(iv::Sample));

    iv::ResourceContext resources;
    auto node_storage = layout.create_storage(resources);
    node_storage.initialize();
    auto const alignment_bytes =
        node_storage.region_bytes(alignment_allocation.region);
    auto const initialized = std::span<iv::Sample const>{
        reinterpret_cast<iv::Sample const*>(alignment_bytes.data()),
        alignment_bytes.size() / sizeof(iv::Sample)};
    ASSERT_FALSE(initialized.empty());
    for (auto const sample : initialized) {
        EXPECT_FLOAT_EQ(static_cast<float>(sample), 0.25f);
    }
}

TEST(GraphJitSampleStoragePlan, ConvertedFeedbackUsesCompactCarryTimeline)
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
    iv::SampleOutputChannelId const source_channel{
        .bundle = 1,
        .port = 0,
        .channel = 0,
    };
    iv::NodeBundlePortId const source_port{1, iv::PortKind::sample, 0};

    ConnectionAnalysisPlan connections;
    connections.schedule.bundle_execution_position.resize(3);
    connections.schedule.bundle_execution_position[1] = 1;
    connections.schedule.bundle_execution_position[2] = 0;

    SampleConnectionPlan feedback;
    feedback.source_type = iv::ChannelTypeId::mono;
    feedback.source_channels = {source_channel};
    feedback.source_channel_timings = {SampleSourceChannelTimingPlan{
        .source = source_channel,
        .source_layout = mono,
    }};
    feedback.canonical_source_port = source_port;
    feedback.canonical_source_layout = mono;
    feedback.target_type = iv::ChannelTypeId::stereo;
    feedback.target_layout = stereo;
    feedback.target_channels = {
        iv::SampleInputChannelId{.bundle = 2, .port = 0, .channel = 0},
        iv::SampleInputChannelId{.bundle = 2, .port = 0, .channel = 1},
    };
    feedback.target_port = iv::NodeBundlePortId{2, iv::PortKind::sample, 0};
    feedback.target_history = 3;
    feedback.requires_conversion = true;
    feedback.detach = iv::ConfiguredSampleConnectionDetach{
        .loop_extra_latency = 6,
        .initial_value_override = iv::Sample{-0.25f},
    };
    feedback.detach_initial_value = sample_value_bytes(iv::Sample{-0.25f});
    connections.sample_connections.push_back(feedback);

    SampleProducerGroupPlan group;
    group.source_port = source_port;
    group.source_type = iv::ChannelTypeId::mono;
    group.source_channels = {source_channel};
    group.canonical_source_layout = mono;
    group.connection_indices = {0};
    group.has_realtime_connections = true;
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::transient_stack};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 2u);
    ASSERT_EQ(storage->persistent_allocations.size(), 1u);
    ASSERT_EQ(storage->feedback_timelines.size(), 1u);
    EXPECT_TRUE(storage->materializations.empty());
    ASSERT_EQ(storage->carry_operations.size(), 1u);

    auto const canonical =
        storage->producer_groups[0]->canonical_representation;
    auto const& timeline = storage->feedback_timelines[0];
    EXPECT_EQ(timeline.writer.kind, SampleFeedbackTimelineWriterKind::copy);
    EXPECT_EQ(
        timeline.storage_plan.kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(
        timeline.storage_plan.candidate_costs.stack_with_persistent_carry
            .copied_bytes,
        (64u + 2u * 9u) * sizeof(iv::Sample));
    EXPECT_EQ(
        timeline.storage_plan.candidate_costs.full_node_storage
            .ring_addressed_bytes,
        64u * sizeof(iv::Sample));
    auto const ring = timeline.timeline_representation;
    ASSERT_NE(canonical, ring);
    EXPECT_EQ(storage->representations[ring].channel_layout, mono);
    EXPECT_EQ(
        storage->representations[ring].storage,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_FALSE(storage->connection_representations[0].has_value());
    ASSERT_TRUE(storage->connection_channel_bindings[0].has_value());
    auto const& converted_channels =
        *storage->connection_channel_bindings[0];
    ASSERT_EQ(converted_channels.size(), 2u);
    for (auto const& channel : converted_channels) {
        EXPECT_EQ(channel.representation, ring);
        EXPECT_EQ(channel.representation_channel, 0u);
        EXPECT_EQ(channel.frame_delay, 0u);
    }

    auto const persistent_index =
        storage->representations[ring].persistent_allocation;
    ASSERT_LT(persistent_index, storage->persistent_allocations.size());
    auto const& persistent = storage->persistent_allocations[persistent_index];
    EXPECT_EQ(persistent.kind, SamplePersistentStorageKind::compact_carry);
    EXPECT_EQ(persistent.channel_layout, mono);
    EXPECT_EQ(persistent.retained_frames, 9u);
    EXPECT_EQ(persistent.frame_capacity, 128u);
    EXPECT_EQ(persistent.size_bytes, 9u * sizeof(iv::Sample));

    auto const& carry = storage->carry_operations.front();
    EXPECT_EQ(carry.representation_index, ring);
    EXPECT_EQ(carry.persistent_allocation, persistent_index);
    EXPECT_EQ(carry.restore_execution_position, 0u);
    EXPECT_EQ(carry.commit_execution_position, 1u);
    EXPECT_EQ(carry.retained_frames, 9u);
    EXPECT_EQ(timeline.writer.after_execution_position, 1u);

}

TEST(GraphJitSampleStoragePlan, AliasableConversionReadsRetainedProducerHistoryDirectly)
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
    converted.source_channel_timings = {SampleSourceChannelTimingPlan{}};
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
    group.storage_requirements = iv::SampleConnectionStorageRequirements{
        .current_block_frames = 64,
        .retained_frames = 7,
        .channel_count = 1,
    };
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 1u);
    EXPECT_TRUE(storage->materializations.empty());
    ASSERT_EQ(storage->carry_operations.size(), 1u);
    auto const canonical =
        storage->producer_groups[0]->canonical_representation;
    EXPECT_EQ(storage->representations[canonical].frame_capacity, 128u);
    EXPECT_FALSE(storage->connection_representations[0].has_value());
    ASSERT_TRUE(storage->connection_channel_bindings[0].has_value());
    auto const& channels = *storage->connection_channel_bindings[0];
    ASSERT_EQ(channels.size(), 2u);
    for (auto const& channel : channels) {
        EXPECT_EQ(channel.representation, canonical);
        EXPECT_EQ(channel.representation_channel, 0u);
        EXPECT_EQ(channel.frame_delay, 0u);
    }
    auto const canonical_allocation = storage->representations[canonical]
        .transient_allocation;
    ASSERT_LT(canonical_allocation, storage->transient_allocations.size());
    EXPECT_EQ(
        storage->transient_allocations[canonical_allocation].size_bytes,
        128u * sizeof(iv::Sample));
}

TEST(GraphJitSampleStoragePlan, AliasableConvertedFanoutSharesRetainedProducerTimeline)
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
    compensated.source_channel_timings = {SampleSourceChannelTimingPlan{}};
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
    group.storage_requirements = iv::SampleConnectionStorageRequirements{
        .current_block_frames = 64,
        .retained_frames = 7,
        .channel_count = 1,
    };
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 2,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 1u);
    EXPECT_TRUE(storage->materializations.empty());
    ASSERT_EQ(storage->connection_representations.size(), 2u);
    EXPECT_FALSE(storage->connection_representations[0].has_value());
    EXPECT_FALSE(storage->connection_representations[1].has_value());
    ASSERT_TRUE(storage->connection_channel_bindings[0].has_value());
    ASSERT_TRUE(storage->connection_channel_bindings[1].has_value());
    auto const canonical =
        storage->producer_groups[0]->canonical_representation;
    EXPECT_EQ(storage->representations[canonical].frame_capacity, 128u);
    for (auto connection_index : {0u, 1u}) {
        auto const& channels =
            *storage->connection_channel_bindings[connection_index];
        ASSERT_EQ(channels.size(), 2u);
        for (auto const& channel : channels) {
            EXPECT_EQ(channel.representation, canonical);
            EXPECT_EQ(channel.representation_channel, 0u);
            EXPECT_EQ(channel.frame_delay, 0u);
        }
    }
}

TEST(GraphJitSampleStoragePlan, GeneratedConversionReadsAffectProducerStorageCost)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const stereo{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    };
    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::NodeBundlePortId const source_port{1, iv::PortKind::sample, 0};
    std::array const source_channels{
        iv::SampleOutputChannelId{.bundle = 1, .port = 0, .channel = 0},
        iv::SampleOutputChannelId{.bundle = 1, .port = 0, .channel = 1},
    };

    ConnectionAnalysisPlan connections;
    connections.schedule.bundle_execution_position.resize(3);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;

    SampleConnectionPlan converted;
    converted.source_type = iv::ChannelTypeId::stereo;
    converted.source_channels.assign(source_channels.begin(), source_channels.end());
    converted.source_channel_timings = {
        SampleSourceChannelTimingPlan{
            .source = source_channels[0],
            .source_layout = stereo,
            .source_history = 1,
        },
        SampleSourceChannelTimingPlan{
            .source = source_channels[1],
            .source_layout = stereo,
            .source_history = 1,
        },
    };
    converted.canonical_source_port = source_port;
    converted.canonical_source_layout = stereo;
    converted.target_type = iv::ChannelTypeId::mono;
    converted.target_layout = mono;
    converted.target_channels = {
        iv::SampleInputChannelId{.bundle = 2, .port = 0, .channel = 0},
    };
    converted.target_port = iv::NodeBundlePortId{2, iv::PortKind::sample, 0};
    converted.requires_conversion = true;
    connections.sample_connections.push_back(converted);

    SampleProducerGroupPlan group;
    group.source_port = source_port;
    group.source_type = iv::ChannelTypeId::stereo;
    group.source_channels.assign(source_channels.begin(), source_channels.end());
    group.canonical_source_layout = stereo;
    group.connection_indices = {0};
    group.has_realtime_connections = true;
    group.storage_requirements = iv::SampleConnectionStorageRequirements{
        .current_block_frames = 64,
        .retained_frames = 1,
        .channel_count = 2,
        .value_size_bytes = sizeof(iv::Sample),
    };
    // Connection analysis has not yet seen the generated conversion reads. The
    // sample-buffer planner must replace this provisional choice using the work
    // it is about to emit.
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::full_node_storage};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(
        connections,
        64,
        {},
        {},
        iv::RealtimeStorageCostModel{
            .copied_byte_weight = 0,
            .ring_addressed_byte_weight = 1,
            .stack_footprint_byte_weight = 0,
            .persistent_footprint_byte_weight = 0,
        });
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_TRUE(storage->producer_groups[0]);
    EXPECT_EQ(
        storage->producer_groups[0]->storage_plan.kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(
        storage->producer_groups[0]
            ->storage_plan.candidate_costs.full_node_storage
            .ring_addressed_bytes,
        128u * sizeof(iv::Sample));
    ASSERT_EQ(storage->materializations.size(), 1u);
}

TEST(GraphJitSampleStoragePlan, RejectsTransientStorageThatCrossesKernelCalls)
{
    using namespace iv::graph_jit::detail;

    ConnectionAnalysisPlan connections;
    SampleProducerGroupPlan group;
    group.canonical_source_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    group.has_realtime_connections = true;
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::transient_stack};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(std::move(group));

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_FALSE(storage.has_value());
    EXPECT_NE(
        storage.error().find("cross-kernel retained storage"),
        std::string::npos);
}

namespace {
constexpr char graph_jit_runtime_fixture_name[] = "graph_jit_runtime_shared_package";

std::string_view graph_jit_runtime_package_source()
{
    // Keep each raw string literal comfortably below the implementation
    // minimum 64 KiB literal-size limit. The shared runtime package keeps
    // growing as GraphJit coverage expands, so assemble it once from
    // stable fragments instead of relying on one oversized literal.
    static constexpr std::string_view part_1 = R"cpp(
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
        std::uint64_t observed_background_extent = 0;
    };

    struct alignas(128) TockState {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        std::uint64_t observed_state_extent = 0;
        std::uint64_t observed_background_extent = 0;
    };

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    bool can_skip_block() const { return true; }

    void tick_block(iv::TickBlockContext<StatefulProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.tick_calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.sample_rate = ctx.sample_rate;
        state.observed_state_extent = ctx.buffer.size();
        state.observed_background_extent = 0;
    }

    void skip_block(iv::SkipBlockContext<StatefulProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.skip_calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.sample_rate = ctx.sample_rate;
        state.observed_state_extent = ctx.buffer.size();
        state.observed_background_extent = 0;
    }
};

struct StateOnlyProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t observed_state_extent = 0;
        std::uint64_t observed_background_extent = 0;
    };

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<StateOnlyProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.observed_state_extent = ctx.buffer.size();
        state.observed_background_extent = 0;
    }
};

struct BackgroundOnlyProbe {
    struct TockState {
        std::uint64_t calls = 0;
        std::uint64_t observed_state_extent = 0;
        std::uint64_t observed_background_extent = 0;
    };

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<BackgroundOnlyProbe> const&) const {}
};

struct BackgroundExecutionProbe {
    struct TockState {
        std::uint64_t calls = 0;
        std::uint64_t sample_rate = 0;
    };

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::coverage)};
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<BackgroundExecutionProbe>& context) const
    {
        iv::IndexCoverage const selected{iv::GlobalIndexRange{
            .begin = iv::GlobalIndex::from_bits(10),
            .extent = 3,
        }};
        context.template output<"out">().publish_region(
            iv::GlobalIndexRange{
                .begin = iv::GlobalIndex::from_bits(10),
                .extent = 3,
            });
        context.template output<"out">().change(selected);
    }

    void tock_coverage(
        iv::TockCoverageContext<BackgroundExecutionProbe>& context) const
    {
        auto& state = context.tock_state();
        ++state.calls;
        state.sample_rate = context.sample_rate;
        auto output = context.template output<"out">().regions();
        for (auto region : output) {
            for (std::uint64_t offset = 0;
                 offset < region.range().extent;
                 ++offset) {
                auto const position = region.range().begin.advanced_by(offset);
                region.at(position) =
                    iv::Sample{static_cast<float>(position.signed_value())};
            }
        }
    }
};

struct StatelessProbe {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
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
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
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
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
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
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    std::size_t max_block_size() const { return 16; }
    bool can_skip_block() const { return true; }

    void tick_block(iv::TickBlockContext<LimitedBlockProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.tick_calls++;
        if (index < state.tick_indices.size()) {
            state.tick_indices[index] = ctx.index.whole().bits();
            state.tick_sizes[index] = ctx.block_size;
        }
    }

    void skip_block(iv::SkipBlockContext<LimitedBlockProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.skip_calls++;
        if (index < state.skip_indices.size()) {
            state.skip_indices[index] = ctx.index.whole().bits();
            state.skip_sizes[index] = ctx.block_size;
        }
    }
};

struct SampleRampSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void tick_block(iv::TickBlockContext<SampleRampSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i})
                    .whole().signed_value())});
        }
    }
};

struct RevisingSampleSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RevisingSampleSource>& ctx) const
    {
        ctx.output<"out">().latency() = 1;
    }

    void tick_block(iv::TickBlockContext<RevisingSampleSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const position = ctx.index.advanced_by({.whole = i}).whole();
            if (position != iv::GlobalIndex{}) {
                output.update(iv::Sample{
                    static_cast<float>(99 + position.signed_value())});
            }
            output.push(iv::Sample{static_cast<float>(position.signed_value())});
        }
    }
};

struct TickCaptureSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample>("samples", iv::OutputProduction::tick, iv::OutputRetention::persisted),
            iv::event_output("events", iv::EventTypeId::trigger, iv::OutputProduction::tick, iv::OutputRetention::persisted, {.max_events_per_index = 0.25}),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<TickCaptureSource>& ctx) const
    {
        ctx.output<"samples">().history() = 2;
        ctx.output<"samples">().latency() = 1;
        ctx.output<"events">().history() = 2;
        ctx.output<"events">().latency() = 1;
    }

    void tick_block(iv::TickBlockContext<TickCaptureSource> const& ctx) const
    {
        auto samples = ctx.template output<"samples">();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            samples[i] = iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i})
                    .whole().signed_value())
                    + 0.5f};
        }
        if (ctx.block_size > 3) {
            ctx.template output<"events">().push(iv::TriggerEvent{}, 3);
        }
    }
};

struct PersistentRevisingSampleSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistentRevisingSampleSource>& ctx) const
    {
        ctx.output<"out">().history() = 5000;
        ctx.output<"out">().latency() = 2;
    }

    void tick_block(iv::TickBlockContext<PersistentRevisingSampleSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const position = ctx.index.advanced_by({.whole = i}).whole();
            if (position != iv::GlobalIndex{}) {
                output.update(iv::Sample{
                    static_cast<float>(99 + position.signed_value())});
            }
            output.push(iv::Sample{static_cast<float>(position.signed_value())});
        }
    }
};

struct TickFallbackSampleSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<TickFallbackSampleSource>& ctx) const
    {
        ctx.output<"out">().latency() = 1;
    }

    std::size_t max_block_size() const { return 2; }

    void tick(iv::TickSampleContext<TickFallbackSampleSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        auto const index = ctx.index.whole();
        if (index != iv::GlobalIndex{}) {
            output.update(iv::Sample{
                static_cast<float>(99 + index.signed_value())});
        }
        output.push(iv::Sample{static_cast<float>(index.signed_value())});
    }
};

struct LimitedSampleRampSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    std::size_t max_block_size() const { return 16; }

    void tick_block(iv::TickBlockContext<LimitedSampleRampSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i})
                    .whole().signed_value())});
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
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<SampleConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const block = ctx.template input<0>().block();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.first = block.empty() ? iv::Sample{0} : block[0];
        state.last = block.empty() ? iv::Sample{0} : block[block.size() - 1];
        state.sum = 0.0f;
        for (auto const sample : block) {
            state.sum += sample;
        }
    }
};

struct SampleFeedbackA {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<float, 24> first_inputs{};
        std::array<float, 24> last_inputs{};
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void tick_block(iv::TickBlockContext<SampleFeedbackA> const& ctx) const
    {
        auto& state = ctx.state();
        auto const input = ctx.template input<0>().block();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.first_inputs[index] = input.empty()
                ? 0.0f
                : static_cast<float>(input[0]);
            state.last_inputs[index] = input.empty()
                ? 0.0f
                : static_cast<float>(input[input.size() - 1]);
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0x5a11ce01u;
        auto output = ctx.template output<0>();
        for (auto const sample : input) {
            output.push(sample + 1.0f);
        }
    }
};

struct SampleFeedbackB {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<float, 24> first_inputs{};
        std::array<float, 24> last_inputs{};
        std::uint64_t marker = 0;
        std::uint64_t distinct_padding = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void tick_block(iv::TickBlockContext<SampleFeedbackB> const& ctx) const
    {
        auto& state = ctx.state();
        auto const input = ctx.template input<0>().block();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.first_inputs[index] = input.empty()
                ? 0.0f
                : static_cast<float>(input[0]);
            state.last_inputs[index] = input.empty()
                ? 0.0f
                : static_cast<float>(input[input.size() - 1]);
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0x5b22ce02ull;
        auto output = ctx.template output<0>();
        for (auto const sample : input) {
            output.push(sample);
        }
    }
};

struct MultiBranchSampleFeedback {
    struct State {
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

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>("fast", iv::InputAccess::sequential),
            iv::input<iv::Sample>("slow", iv::InputAccess::sequential),
            iv::input<iv::Sample>("initialized", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void tick_block(iv::TickBlockContext<MultiBranchSampleFeedback> const& ctx) const
    {
        auto& state = ctx.state();
        auto const fast = ctx.template input<0>().block();
        auto const slow = ctx.template input<1>().block();
        auto const initialized = ctx.template input<2>().block();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            if (!fast.empty()) {
                state.first_fast[index] = static_cast<float>(fast[0]);
                state.last_fast[index] = static_cast<float>(fast[fast.size() - 1]);
                state.first_slow[index] = static_cast<float>(slow[0]);
                state.last_slow[index] = static_cast<float>(slow[slow.size() - 1]);
                state.first_initialized[index] = static_cast<float>(initialized[0]);
                state.last_initialized[index] = static_cast<float>(initialized[initialized.size() - 1]);
            }
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0x6d756c74u;
        auto output = ctx.template output<0>();
        for (auto const sample : fast) {
            output.push(sample + 1.0f);
        }
    }
};

struct TemporalSampleFeedback {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<float, 24> current_inputs{};
        std::array<float, 24> history_3_inputs{};
        std::array<float, 24> first_inputs{};
        std::array<float, 24> last_inputs{};
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<TemporalSampleFeedback>& ctx) const
    {
        ctx.input<"in">().history() = 3;
        ctx.output<"out">().latency() = 2;
    }

    void tick_block(iv::TickBlockContext<TemporalSampleFeedback> const& ctx) const
    {
        auto& state = ctx.state();
        auto const input = ctx.template input<0>().block();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.current_inputs[index] = static_cast<float>(ctx.template input<0>().get());
            state.history_3_inputs[index] =
                static_cast<float>(ctx.template input<0>().get(3));
            state.first_inputs[index] = input.empty()
                ? 0.0f
                : static_cast<float>(input[0]);
            state.last_inputs[index] = input.empty()
                ? 0.0f
                : static_cast<float>(input[input.size() - 1]);
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0x7e4fba11u;
        auto output = ctx.template output<0>();
        for (auto const sample : input) {
            output.push(sample + 1.0f);
        }
    }
};

struct RevisingSampleFeedback {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 8> indices{};
        std::array<float, 8> first_inputs{};
        std::array<float, 8> last_inputs{};
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RevisingSampleFeedback>& ctx) const
    {
        ctx.output<"out">().latency() = 2;
    }

    void tick_block(iv::TickBlockContext<RevisingSampleFeedback> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            auto const block = ctx.template input<0>().block();
            state.first_inputs[index] = block.empty()
                ? 0.0f
                : static_cast<float>(block[0]);
            state.last_inputs[index] = block.empty()
                ? 0.0f
                : static_cast<float>(block[block.size() - 1]);
        }
        ++state.calls;
        state.marker = 0x5a17e001u;

        auto output = ctx.template output<0>();
        if (ctx.index == iv::FractionalGlobalIndex::from_global(
                iv::GlobalIndex::from_bits(4))) {
            // Frame 3 was authored by the preceding root call and remains
            // revisable because the output declares two frames of latency.
            output.update(iv::Sample{103.0f});
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i}).whole().signed_value())});
        }
    }
};

struct ProjectedRevisingSampleFeedback {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 8> indices{};
        std::array<float, 8> first_left{};
        std::array<float, 8> first_right{};
        std::array<float, 8> last_left{};
        std::array<float, 8> last_right{};
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<ProjectedRevisingSampleFeedback>& ctx) const
    {
        ctx.output<"out">().latency() = 2;
    }

    void tick_block(
        iv::TickBlockContext<ProjectedRevisingSampleFeedback> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            if (ctx.block_size != 0) {
                state.first_left[index] =
                    static_cast<float>(ctx.template input<0>().get_value(0, 0));
                state.first_right[index] =
                    static_cast<float>(ctx.template input<0>().get_value(0, 1));
                state.last_left[index] = static_cast<float>(
                    ctx.template input<0>().get_value(ctx.block_size - 1, 0));
                state.last_right[index] = static_cast<float>(
                    ctx.template input<0>().get_value(ctx.block_size - 1, 1));
            }
        }
        ++state.calls;
        state.marker = 0x52e71e55u;

        auto output = ctx.template output<0>();
        if (ctx.index == iv::FractionalGlobalIndex::from_global(
                iv::GlobalIndex::from_bits(4))) {
            output.update(iv::Sample{103.0f});
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i}).whole().signed_value())});
        }
    }
};

struct ConvertedSampleFeedback {
    struct State {
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

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void tick_block(iv::TickBlockContext<ConvertedSampleFeedback> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            if (ctx.block_size != 0) {
                state.first_left[index] =
                    static_cast<float>(ctx.template input<0>().get_value(0, 0));
                state.first_right[index] =
                    static_cast<float>(ctx.template input<0>().get_value(0, 1));
                state.last_left[index] = static_cast<float>(
                    ctx.template input<0>().get_value(ctx.block_size - 1, 0));
                state.last_right[index] = static_cast<float>(
                    ctx.template input<0>().get_value(ctx.block_size - 1, 1));
            }
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xc04e7ed1u;
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const left = ctx.template input<0>().get_value(i, 0);
            auto const right = ctx.template input<0>().get_value(i, 1);
            output.push((left + right) * 0.5f + 1.0f);
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
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<MonoInterleavedConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const block = ctx.template input<0>().block();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
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
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    void tick_block(iv::TickBlockContext<StereoRampSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const position = ctx.index.advanced_by({.whole = i}).whole();
            std::array<iv::Sample, 2> frame{
                iv::Sample{static_cast<float>(position.signed_value())},
                iv::Sample{
                    static_cast<float>(position.signed_value()) + 1000.0f},
            };
            output.value(i, 0) = frame[0];
            output.value(i, 1) = frame[1];
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
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            }})};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<StereoPlanarConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.sum_left = 0.0f;
        state.sum_right = 0.0f;
        if (ctx.block_size != 0) {
            state.first_left = static_cast<float>(ctx.template input<0>().get_value(0, 0));
            state.first_right = static_cast<float>(ctx.template input<0>().get_value(0, 1));
            state.last_left = static_cast<float>(
                ctx.template input<0>().get_value(ctx.block_size - 1, 0));
            state.last_right = static_cast<float>(
                ctx.template input<0>().get_value(ctx.block_size - 1, 1));
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            state.sum_left += static_cast<float>(ctx.template input<0>().get_value(i, 0));
            state.sum_right += static_cast<float>(ctx.template input<0>().get_value(i, 1));
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
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<StereoSampleConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.sum_left = 0.0f;
        state.sum_right = 0.0f;
        if (ctx.block_size != 0) {
            state.first_left = static_cast<float>(ctx.template input<0>().get_value(0, 0));
            state.first_right = static_cast<float>(ctx.template input<0>().get_value(0, 1));
            state.last_left = static_cast<float>(
                ctx.template input<0>().get_value(ctx.block_size - 1, 0));
            state.last_right = static_cast<float>(
                ctx.template input<0>().get_value(ctx.block_size - 1, 1));
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            state.sum_left += static_cast<float>(ctx.template input<0>().get_value(i, 0));
            state.sum_right += static_cast<float>(ctx.template input<0>().get_value(i, 1));
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
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<HistoryRampSource>& ctx) const
    {
        ctx.output<"out">().history() = 3;
        ctx.output<"out">().latency() = 2;
    }

    void tick_block(iv::TickBlockContext<HistoryRampSource> const& ctx) const
    {
        auto& state = ctx.state();
        auto output = ctx.template output<0>();
        ++state.calls;
        state.previous_output = static_cast<float>(output.get());
        state.marker = 0x91a2b3c4u;
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i}).whole().signed_value())});
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
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<HistoryConsumerProbe>& ctx) const
    {
        ctx.input<"in">().history() = 5;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<HistoryConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.current = static_cast<float>(ctx.template input<0>().get(0));
        state.history_1 = static_cast<float>(ctx.template input<0>().get(1));
        state.history_5 = static_cast<float>(ctx.template input<0>().get(5));
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
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<StereoHistoryConsumerProbe>& ctx) const
    {
        ctx.input<"in">().history() = 5;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<StereoHistoryConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.current_left = static_cast<float>(ctx.template input<0>().get(0, 0));
        state.current_right = static_cast<float>(ctx.template input<0>().get(0, 1));
        state.history_5_left = static_cast<float>(ctx.template input<0>().get(5, 0));
        state.history_5_right = static_cast<float>(ctx.template input<0>().get(5, 1));
        state.marker = 0xa1b2c3d4e5f60718ull;
    }
};

struct LargeHistoryRampSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void tick_block(iv::TickBlockContext<LargeHistoryRampSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i}).whole().signed_value())});
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
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<LargeHistoryConsumerProbe>& ctx) const
    {
        ctx.input<"in">().history() = 5000;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<LargeHistoryConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.current = static_cast<float>(ctx.template input<0>().get(0));
        state.history_1 = static_cast<float>(ctx.template input<0>().get(1));
        state.history_5000 = static_cast<float>(ctx.template input<0>().get(5000));
    }
};

struct FiveSampleDelay {
    struct State {
        std::array<iv::Sample, 8> memory{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void constrain_ports(iv::ConstrainPortsContext<FiveSampleDelay>& ctx) const
    {
        ctx.output<"out">().latency() = 2;
    }

    constexpr std::size_t internal_latency() const { return 5; }

    void tick_block(iv::TickBlockContext<FiveSampleDelay> const& ctx) const
    {
        auto const input = ctx.template input<0>().block();
        auto output = ctx.template output<0>();
        auto& memory = ctx.state().memory;
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const index = static_cast<std::size_t>(
                ctx.index.advanced_by({.whole = i}).whole().bits());
            auto const delayed = memory[index & 7u];
            memory[(index + 5u) & 7u] = input[i];
            output.push(delayed);
        }
    }
};

)cpp";
    static constexpr std::string_view part_2 = R"cpp(struct LatencyCompensationProbe {
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
            iv::input<iv::Sample>("fast", iv::InputAccess::sequential),
            iv::input<iv::Sample>("slow", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<LatencyCompensationProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const fast = ctx.template input<0>().block();
        auto const slow = ctx.template input<1>().block();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
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
            iv::input<iv::Sample>("fast", iv::InputAccess::sequential,
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::interleaved,
                }}),
            iv::input<iv::Sample>("slow", iv::InputAccess::sequential,
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::interleaved,
                }}),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<InterleavedLatencyCompensationProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const fast = ctx.template input<0>().block();
        auto const slow = ctx.template input<1>().block();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
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

struct DisconnectedSampleInputProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        float current_left = 0.0f;
        float current_right = 0.0f;
        float history_left = 0.0f;
        float history_right = 0.0f;
        float block_first = 0.0f;
        float block_last = 0.0f;
        float block_sum = 0.0f;
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
            {.channel_layout = {
                 .channel_type = iv::ChannelTypeId::stereo,
                 .sample_layout = iv::SampleStreamLayout::interleaved,
             },
             .default_value = iv::Sample{0.375f}})};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<DisconnectedSampleInputProbe>& ctx) const
    {
        ctx.input<"in">().history() = 5;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<DisconnectedSampleInputProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto input = ctx.template input<0>();
        auto const block = input.block(std::size_t{0});
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.current_left = input.get(0, 0);
        state.current_right = input.get(0, 1);
        state.history_left = input.get(5, 0);
        state.history_right = input.get(5, 1);
        state.block_first = block.empty() ? 0.0f : static_cast<float>(block[0]);
        state.block_last = block.empty()
            ? 0.0f
            : static_cast<float>(block[block.size() - 1]);
        state.block_sum = 0.0f;
        for (auto const sample : block) state.block_sum += sample;
        state.marker = 0xd15c0a11u;
    }
};

struct TriggerEventSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::event_output("trigger", iv::EventTypeId::trigger, iv::OutputProduction::tick),
        };
    }

    void tick_block(iv::TickBlockContext<TriggerEventSource> const& ctx) const
    {
        if (ctx.block_size == 0) return;
        ctx.template output<0>().push(
            iv::TriggerEvent{}, 3);
        ctx.template output<0>().push(
            iv::TriggerEvent{}, ctx.block_size - 1);
    }
};

struct MidiEventSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::event_output("midi", iv::EventTypeId::midi, iv::OutputProduction::tick),
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

        ctx.template output<0>().push(
            note_on_a, 5);
        ctx.template output<0>().push(
            note_off, 9);
        ctx.template output<0>().push(
            note_on_b, 17);
    }
};

struct LimitedTriggerEventSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::event_output("trigger", iv::EventTypeId::trigger, iv::OutputProduction::tick),
        };
    }

    std::size_t max_block_size() const { return 16; }

    void tick_block(iv::TickBlockContext<LimitedTriggerEventSource> const& ctx) const
    {
        if (ctx.block_size == 0) return;
        ctx.template output<0>().push(
            iv::TriggerEvent{}, 3);
        ctx.template output<0>().push(
            iv::TriggerEvent{}, ctx.block_size - 1);
    }
};

struct FanInBurstEventSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.125})};
    }

    void tick_block(iv::TickBlockContext<FanInBurstEventSource> const& ctx) const
    {
        if (ctx.block_size == 0) return;
        for (std::size_t i = 0; i < 8; ++i) {
            ctx.template output<0>().push(
                iv::TriggerEvent{}, 3);
        }
    }
};

struct FanInSparseEventSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.125})};
    }

    void tick_block(iv::TickBlockContext<FanInSparseEventSource> const& ctx) const
    {
        if (ctx.block_size == 0) return;
        for (std::size_t i = 0; i < 4; ++i) {
            ctx.template output<0>().push(
                iv::TriggerEvent{}, 3);
        }
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
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
    }

    void tick_block(iv::TickBlockContext<EventFeedbackA> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        auto const events = ctx.template input<0>().events();
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.input_counts[index] = events.size();
            state.first_input_times[index] =
                events.empty() ? 0 : events[0].time.bits();
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xa11ce001u;
        if (ctx.block_size != 0) {
            auto const offset = std::min<std::size_t>(1, ctx.block_size - 1);
            ctx.template output<0>().push(
                iv::TriggerEvent{}, offset);
        }
    }
};

struct RetainedFanInEventFeedbackA {
    using State = EventFeedbackA::State;

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RetainedFanInEventFeedbackA>& ctx) const
    {
        ctx.input<"in">().history() = 8;
    }

    static constexpr auto outputs()
    {
        return EventFeedbackA::outputs();
    }

    void tick_block(
        iv::TickBlockContext<RetainedFanInEventFeedbackA> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        constexpr std::size_t history = 8;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(), ctx.block_size + history);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.input_counts[index] = events.size();
            state.first_input_times[index] =
                events.empty() ? 0 : events[0].time.bits();
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xa11ce008u;
        if (ctx.block_size != 0) {
            auto const offset = std::min<std::size_t>(1, ctx.block_size - 1);
            ctx.template output<0>().push(
                iv::TriggerEvent{}, offset);
        }
    }
};

struct LatentEventFeedbackA {
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
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<LatentEventFeedbackA>& ctx) const
    {
        ctx.output<"out">().latency() = 16;
    }

    void tick_block(iv::TickBlockContext<LatentEventFeedbackA> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        auto const events = ctx.template input<0>().events();
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.input_counts[index] = events.size();
            state.first_input_times[index] =
                events.empty() ? 0 : events[0].time.bits();
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xa11ce016u;

        // Publish one event beyond the current SCC slice. Across slices and root
        // calls these timestamps remain globally nondecreasing; authored latency
        // is a future-publication horizon, not permission to back-fill.
        auto const offset = ctx.block_size + 1;
        ctx.template output<0>().push(
            iv::TriggerEvent{}, offset);
    }
};

struct PersistentLatentEventFeedbackA {
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
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistentLatentEventFeedbackA>& ctx) const
    {
        ctx.output<"out">().latency() = 320;
    }

    void tick_block(iv::TickBlockContext<PersistentLatentEventFeedbackA> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        auto const events = ctx.template input<0>().events();
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.input_counts[index] = events.size();
            state.first_input_times[index] =
                events.empty() ? 0 : events[0].time.bits();
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xa11ce320u;

        // The large declared future horizon forces canonical persistent-ring
        // storage. Actual publication still advances monotonically by one sample
        // beyond each SCC slice; latency is permission to publish future events,
        // not permission to insert before the prior publication watermark.
        auto const offset = ctx.block_size + 1;
        ctx.template output<0>().push(
            iv::TriggerEvent{}, offset);
    }
};

struct PersistentLatentBoundaryEventFeedbackA {
    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("in", iv::EventTypeId::boundary, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::boundary, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistentLatentBoundaryEventFeedbackA>& ctx) const
    {
        ctx.output<"out">().latency() = 320;
    }

    void tick_block(
        iv::TickBlockContext<PersistentLatentBoundaryEventFeedbackA> const& ctx) const
    {
        (void)ctx.template input<0>().events();
        auto const offset = ctx.block_size + 1;
        ctx.template output<0>().push(
            iv::BoundaryEvent{.is_begin = true},
            offset);
    }
};

struct EventFeedbackBurstA {
    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 16.0})};
    }

    void tick_block(iv::TickBlockContext<EventFeedbackBurstA> const& ctx) const
    {
        if (ctx.block_size != 1) return;
        // Publish the full declared one-sample rate at one timestamp to stress
        // delayed feedback carry across tiny root calls.
        for (std::size_t i = 0; i < 16; ++i) {
            ctx.template output<0>().push(
                iv::TriggerEvent{}, 0);
        }
    }
};

struct EventFeedbackFanoutA {
    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("in_a", iv::EventTypeId::trigger, iv::InputAccess::sequential),
            iv::event_input("in_b", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
    }

    void tick_block(iv::TickBlockContext<EventFeedbackFanoutA> const& ctx) const
    {
        // Force both explicit feed-forward dependencies to remain observable to
        // the package/JIT interfaces even though this probe only needs to emit.
        (void)ctx.template input<0>().events();
        (void)ctx.template input<1>().events();
        if (ctx.block_size == 0) return;
        auto const offset = std::min<std::size_t>(1, ctx.block_size - 1);
        ctx.template output<0>().push(
            iv::TriggerEvent{}, offset);
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
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
    }

    void tick_block(iv::TickBlockContext<EventFeedbackB> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        auto const events = ctx.template input<0>().events();
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.input_counts[index] = events.size();
            state.first_input_times[index] =
                events.empty() ? 0 : events[0].time.bits();
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xb22ce002ull;
        if (ctx.block_size != 0) {
            auto const offset = std::min<std::size_t>(2, ctx.block_size - 1);
            ctx.template output<0>().push(
                iv::TriggerEvent{}, offset);
        }
    }
};

struct RetainedDualEventFeedbackA {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<std::uint64_t, 24> exact_counts{};
        std::array<std::uint64_t, 24> exact_first_times{};
        std::array<std::uint64_t, 24> exact_last_times{};
        std::array<std::uint64_t, 24> converted_counts{};
        std::array<std::uint64_t, 24> converted_first_times{};
        std::array<std::uint64_t, 24> converted_last_times{};
        std::uint64_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("exact", iv::EventTypeId::trigger, iv::InputAccess::sequential),
            iv::event_input("converted", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RetainedDualEventFeedbackA>& ctx) const
    {
        ctx.input<"exact">().history() = 8;
        ctx.input<"converted">().history() = 8;
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
    }

    void tick_block(
        iv::TickBlockContext<RetainedDualEventFeedbackA> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls++);
        constexpr std::size_t history = 8;
        auto const begin = ctx.index.retreated_by({.whole = history}).whole();
        auto const count = ctx.block_size + history;
        auto const exact = ctx.template input<0>().events(begin, count);
        auto const converted = ctx.template input<1>().events(begin, count);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.exact_counts[index] = exact.size();
            state.converted_counts[index] = converted.size();
            if (!exact.empty()) {
                state.exact_first_times[index] = exact[0].time.bits();
                state.exact_last_times[index] =
                    exact[exact.size() - 1].time.bits();
            }
            if (!converted.empty()) {
                state.converted_first_times[index] = converted[0].time.bits();
                state.converted_last_times[index] =
                    converted[converted.size() - 1].time.bits();
            }
        }
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xa11ce808u;
        auto const offset = std::min<std::size_t>(1, ctx.block_size - 1);
        ctx.template output<0>().push(
            iv::TriggerEvent{}, offset);
    }
};

struct DualEventFeedbackB {
    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::event_output("exact", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25}),
            iv::event_output("converted", iv::EventTypeId::boundary, iv::OutputProduction::tick, {.max_events_per_index = 0.25}),
        };
    }

    void tick_block(iv::TickBlockContext<DualEventFeedbackB> const& ctx) const
    {
        (void)ctx.template input<0>().events();
        auto const offset = std::min<std::size_t>(2, ctx.block_size - 1);
        ctx.template output<0>().push(
            iv::TriggerEvent{}, offset);
        ctx.template output<1>().push(
            iv::BoundaryEvent{.is_begin = true},
            offset);
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
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<TriggerEventConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const events = ctx.template input<0>().events();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.event_count = events.size();
        state.trigger_count = 0;
        state.marker = 0xe71e17u;
        if (!events.empty()) {
            state.first_time = events[0].time.bits();
            state.last_time = events[events.size() - 1].time.bits();
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
            iv::event_input("midi", iv::EventTypeId::midi, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RetainedMidiEventConsumer>& ctx) const
    {
        ctx.input<"midi">().history() = 160;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<RetainedMidiEventConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.calls++;
        if (index >= state.indices.size()) return;
        constexpr std::size_t history = 160;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(), ctx.block_size + history);
        state.indices[index] = ctx.index.whole().bits();
        state.event_counts[index] = events.size();
        state.midi_counts[index] = 0;
        if (!events.empty()) {
            state.first_times[index] = events[0].time.bits();
            state.last_times[index] = events[events.size() - 1].time.bits();
        }
        for (auto const& event : events) {
            if (std::holds_alternative<iv::MidiEvent>(event.value)) {
                ++state.midi_counts[index];
            }
        }
    }
};

struct BoundaryEventFeedbackA {
    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("in", iv::EventTypeId::boundary, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::boundary, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
    }

    void tick_block(iv::TickBlockContext<BoundaryEventFeedbackA> const& ctx) const
    {
        (void)ctx.template input<0>().events();
        auto const offset = std::min<std::size_t>(1, ctx.block_size - 1);
        ctx.template output<0>().push(
            iv::BoundaryEvent{.is_begin = true},
            offset);
    }
};

struct BoundaryEventFeedbackB {
    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("in", iv::EventTypeId::boundary, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::boundary, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
    }

    void tick_block(iv::TickBlockContext<BoundaryEventFeedbackB> const& ctx) const
    {
        (void)ctx.template input<0>().events();
        auto const offset = std::min<std::size_t>(2, ctx.block_size - 1);
        ctx.template output<0>().push(
            iv::BoundaryEvent{.is_begin = true},
            offset);
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
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    std::size_t max_block_size() const { return 32; }

    void tick_block(iv::TickBlockContext<LimitedTriggerEventConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.calls++;
        if (index >= state.indices.size()) return;
        auto const events = ctx.template input<0>().events();
        state.indices[index] = ctx.index.whole().bits();
        state.block_sizes[index] = ctx.block_size;
        state.event_counts[index] = events.size();
        state.trigger_counts[index] = 0;
        if (!events.empty()) {
            state.first_times[index] = events[0].time.bits();
            state.last_times[index] = events[events.size() - 1].time.bits();
        }
        for (auto const& event : events) {
            if (std::holds_alternative<iv::TriggerEvent>(event.value)) {
                ++state.trigger_counts[index];
            }
        }
    }
};

struct RetainedTriggerEventSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::event_output("trigger", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.5}),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RetainedTriggerEventSource>& ctx) const
    {
        ctx.output<"trigger">().latency() = 8;
    }

    void tick_block(iv::TickBlockContext<RetainedTriggerEventSource> const& ctx) const
    {
        if (ctx.block_size < 8) return;
        ctx.template output<0>().push(
            iv::TriggerEvent{}, 5);
        ctx.template output<0>().push(
            iv::TriggerEvent{}, ctx.block_size - 3);
        // Authored output latency permits publishing a future event that must
        // remain visible to the next root invocation.
        ctx.template output<0>().push(
            iv::TriggerEvent{}, ctx.block_size + 3);
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
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RetainedTriggerEventConsumer>& ctx) const
    {
        ctx.input<"trigger">().history() = 8;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<RetainedTriggerEventConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.calls++;
        if (index >= state.indices.size()) return;
        constexpr std::size_t history = 8;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(), ctx.block_size + history);
        state.indices[index] = ctx.index.whole().bits();
        state.event_counts[index] = events.size();
        if (!events.empty()) {
            state.first_times[index] = events[0].time.bits();
            state.last_times[index] = events[events.size() - 1].time.bits();
        }
        if (events.size() > 1) {
            state.second_times[index] = events[1].time.bits();
        }
    }
};

struct PersistentEventRingSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::event_output("trigger", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.5}),
        };
    }

    void tick_block(iv::TickBlockContext<PersistentEventRingSource> const& ctx) const
    {
        if (ctx.block_size < 8) return;
        // Concentrating all events at one timestamp also proves the sizing-rate
        // field is not interpreted as a per-sample runtime limiter.
        for (std::size_t i = 0; i < 32; ++i) {
            ctx.template output<0>().push(
                iv::TriggerEvent{}, 5);
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
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistentEventRingConsumer>& ctx) const
    {
        ctx.input<"trigger">().history() = 160;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<PersistentEventRingConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.calls++;
        if (index >= state.indices.size()) return;
        constexpr std::size_t history = 160;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(), ctx.block_size + history);
        state.indices[index] = ctx.index.whole().bits();
        state.event_counts[index] = events.size();
        if (!events.empty()) {
            state.first_times[index] = events[0].time.bits();
            state.last_times[index] = events[events.size() - 1].time.bits();
        }
    }
};

struct PersistentEventFeedbackConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 4> indices{};
        std::array<std::uint64_t, 4> event_counts{};
        std::array<std::uint64_t, 4> first_times{};
        std::array<std::uint64_t, 4> last_times{};
        std::uint64_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistentEventFeedbackConsumer>& ctx) const
    {
        ctx.input<"trigger">().history() = 320;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PersistentEventFeedbackConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.calls++;
        if (index >= state.indices.size()) return;
        constexpr std::size_t history = 320;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(), ctx.block_size + history);
        state.indices[index] = ctx.index.whole().bits();
        state.event_counts[index] = events.size();
        if (!events.empty()) {
            state.first_times[index] = events[0].time.bits();
            state.last_times[index] = events[events.size() - 1].time.bits();
        }
        state.marker = 0xfeed320u;
    }
};

struct PortedProbe {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::TickOutputConfig<iv::Sample>, 1>{};
    }

    void tick(iv::TickSampleContext<PortedProbe> const& ctx) const
    {
        ctx.template output<0>().push(iv::Sample{0.25f});
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

void background_only_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.background_only">();
    graph.outputs();
}

void background_execution_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.background_evaluation_call">();
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

void disconnected_sample_input_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.disconnected_sample_input_probe">();
    graph.outputs();
}

void disconnected_sample_output_module(iv::GraphBuilder& graph)
{
    auto source =
        graph.node<"iv.test.graph_jit.state_context.history_ramp_source">();
    (void)source;
    graph.outputs();
}

void tick_capture_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.tick_capture_source">();
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

void sample_revision_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.revising_sample_source">();
    auto mono_sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    auto stereo_sink = graph.node<"iv.test.graph_jit.state_context.stereo_sample_consumer">();
    mono_sink(source);
    stereo_sink(source);
    graph.outputs();
}

void persistent_sample_revision_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.persistent_revising_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink(source);
    graph.outputs();
}

void composed_sample_revision_module(iv::GraphBuilder& graph)
{
    auto left = graph.node<"iv.test.graph_jit.state_context.revising_sample_source">();
    auto right = graph.node<"iv.test.graph_jit.state_context.revising_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.stereo_sample_consumer">();
    sink(graph.tile<iv::stereo>(left, right));
    graph.outputs();
}

void tick_fallback_sample_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.tick_fallback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink(source);
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

void stereo_history_conversion_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.stereo_ramp_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.history_consumer">();
    sink(source);
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

void large_history_rewire_module(iv::GraphBuilder& graph)
{
    auto first_source =
        graph.node<"iv.test.graph_jit.state_context.large_history_ramp_source">();
    auto second_source =
        graph.node<"iv.test.graph_jit.state_context.large_history_ramp_source">();
    auto first_consumer =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    auto second_consumer =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    first_consumer(first_source);
    second_consumer(second_source);
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
IV_NODE("iv.test.graph_jit.state_context.background_only", BackgroundOnlyProbe);
IV_NODE("iv.test.graph_jit.state_context.background_evaluation_call", BackgroundExecutionProbe);
IV_NODE("iv.test.graph_jit.state_context.stateless", StatelessProbe);
IV_NODE("iv.test.graph_jit.state_context.configured", ConfiguredProbe);
IV_NODE("iv.test.graph_jit.state_context.pointer_configured", PointerConfiguredProbe);
IV_NODE("iv.test.graph_jit.state_context.limited_block", LimitedBlockProbe);
IV_NODE("iv.test.graph_jit.state_context.sample_ramp_source", SampleRampSource);
IV_NODE("iv.test.graph_jit.state_context.revising_sample_source", RevisingSampleSource);
IV_NODE("iv.test.graph_jit.state_context.tick_capture_source", TickCaptureSource);
IV_NODE("iv.test.graph_jit.state_context.persistent_revising_sample_source", PersistentRevisingSampleSource);
IV_NODE("iv.test.graph_jit.state_context.tick_fallback_sample_source", TickFallbackSampleSource);
IV_NODE("iv.test.graph_jit.state_context.limited_sample_ramp_source", LimitedSampleRampSource);
IV_NODE("iv.test.graph_jit.state_context.sample_consumer", SampleConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.sample_feedback_a", SampleFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.sample_feedback_b", SampleFeedbackB);
IV_NODE("iv.test.graph_jit.state_context.multi_branch_sample_feedback", MultiBranchSampleFeedback);
IV_NODE("iv.test.graph_jit.state_context.temporal_sample_feedback", TemporalSampleFeedback);
IV_NODE("iv.test.graph_jit.state_context.revising_sample_feedback", RevisingSampleFeedback);
IV_NODE("iv.test.graph_jit.state_context.projected_revising_sample_feedback", ProjectedRevisingSampleFeedback);
IV_NODE("iv.test.graph_jit.state_context.converted_sample_feedback", ConvertedSampleFeedback);
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
IV_NODE("iv.test.graph_jit.state_context.disconnected_sample_input_probe", DisconnectedSampleInputProbe);
IV_NODE("iv.test.graph_jit.state_context.trigger_event_source", TriggerEventSource);
IV_NODE("iv.test.graph_jit.state_context.midi_event_source", MidiEventSource);
IV_NODE("iv.test.graph_jit.state_context.limited_trigger_event_source", LimitedTriggerEventSource);
IV_NODE("iv.test.graph_jit.state_context.fan_in_burst_event_source", FanInBurstEventSource);
IV_NODE("iv.test.graph_jit.state_context.fan_in_sparse_event_source", FanInSparseEventSource);
IV_NODE("iv.test.graph_jit.state_context.event_feedback_a", EventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.retained_fan_in_event_feedback_a", RetainedFanInEventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.latent_event_feedback_a", LatentEventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.persistent_latent_event_feedback_a", PersistentLatentEventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.persistent_latent_boundary_event_feedback_a", PersistentLatentBoundaryEventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.event_feedback_b", EventFeedbackB);
IV_NODE("iv.test.graph_jit.state_context.boundary_event_feedback_a", BoundaryEventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.boundary_event_feedback_b", BoundaryEventFeedbackB);
IV_NODE("iv.test.graph_jit.state_context.event_feedback_burst_a", EventFeedbackBurstA);
IV_NODE("iv.test.graph_jit.state_context.event_feedback_fanout_a", EventFeedbackFanoutA);
IV_NODE("iv.test.graph_jit.state_context.retained_dual_event_feedback_a", RetainedDualEventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.dual_event_feedback_b", DualEventFeedbackB);
IV_NODE("iv.test.graph_jit.state_context.trigger_event_consumer", TriggerEventConsumer);
IV_NODE("iv.test.graph_jit.state_context.limited_trigger_event_consumer", LimitedTriggerEventConsumer);
IV_NODE("iv.test.graph_jit.state_context.retained_trigger_event_source", RetainedTriggerEventSource);
IV_NODE("iv.test.graph_jit.state_context.retained_trigger_event_consumer", RetainedTriggerEventConsumer);
IV_NODE("iv.test.graph_jit.state_context.persistent_event_ring_source", PersistentEventRingSource);
IV_NODE("iv.test.graph_jit.state_context.persistent_event_ring_consumer", PersistentEventRingConsumer);
IV_NODE("iv.test.graph_jit.state_context.persistent_event_feedback_consumer", PersistentEventFeedbackConsumer);
IV_NODE("iv.test.graph_jit.state_context.retained_midi_event_consumer", RetainedMidiEventConsumer);
IV_NODE("iv.test.graph_jit.state_context.ported", PortedProbe);
IV_MODULE("iv.test.graph_jit.state_context.stateful_module", stateful_module);
IV_MODULE("iv.test.graph_jit.state_context.state_only_module", state_only_module);
IV_MODULE("iv.test.graph_jit.state_context.background_only_module", background_only_module);
IV_MODULE("iv.test.graph_jit.state_context.background_execution_module", background_execution_module);
IV_MODULE("iv.test.graph_jit.state_context.stateless_module", stateless_module);
IV_MODULE("iv.test.graph_jit.state_context.configured_module", configured_module);
IV_MODULE("iv.test.graph_jit.state_context.pointer_configured_module", pointer_configured_module);
IV_MODULE("iv.test.graph_jit.state_context.multiple_module", multiple_module);
IV_MODULE("iv.test.graph_jit.state_context.skippable_pair_module", skippable_pair_module);
IV_MODULE("iv.test.graph_jit.state_context.limited_block_module", limited_block_module);
IV_MODULE("iv.test.graph_jit.state_context.disconnected_sample_input_module", disconnected_sample_input_module);
IV_MODULE("iv.test.graph_jit.state_context.disconnected_sample_output_module", disconnected_sample_output_module);
IV_MODULE("iv.test.graph_jit.state_context.tick_capture_module", tick_capture_module);
IV_MODULE("iv.test.graph_jit.state_context.direct_sample_module", direct_sample_module);
IV_MODULE("iv.test.graph_jit.state_context.transient_sample_module", transient_sample_module);
IV_MODULE("iv.test.graph_jit.state_context.reused_sample_arena_module", reused_sample_arena_module);
IV_MODULE("iv.test.graph_jit.state_context.sample_fanout_conversion_module", sample_fanout_conversion_module);
IV_MODULE("iv.test.graph_jit.state_context.sample_revision_module", sample_revision_module);
IV_MODULE("iv.test.graph_jit.state_context.persistent_sample_revision_module", persistent_sample_revision_module);
IV_MODULE("iv.test.graph_jit.state_context.composed_sample_revision_module", composed_sample_revision_module);
IV_MODULE("iv.test.graph_jit.state_context.tick_fallback_sample_module", tick_fallback_sample_module);
IV_MODULE("iv.test.graph_jit.state_context.stereo_conversion_module", stereo_conversion_module);
IV_MODULE("iv.test.graph_jit.state_context.stereo_history_conversion_module", stereo_history_conversion_module);
IV_MODULE("iv.test.graph_jit.state_context.history_fanout_module", history_fanout_module);
IV_MODULE("iv.test.graph_jit.state_context.persistent_history_module", persistent_history_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.large_history_rewire_module",
    large_history_rewire_module);
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
    static constexpr std::string_view part_3 = R"cpp(
#include <intravenous/dsl.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace {

struct NativeBatchTickProbe {
    std::uint64_t id = 0;

    struct State {
        std::uint64_t tick_batch_calls = 0;
        std::uint64_t skip_batch_calls = 0;
        std::uint64_t last_lane_count = 0;
        std::uint64_t configured_id = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
    };

    constexpr explicit NativeBatchTickProbe(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }
    bool can_skip_block() const { return true; }

    static void tick_block_batch(
        iv::TickBlockBatchContext<NativeBatchTickProbe> const& context)
    {
        for (auto lane : context) {
            auto& state = lane.state();
            ++state.tick_batch_calls;
            state.last_lane_count = context.size();
            state.configured_id = lane.node().id;
            state.last_index = lane.context().index.whole().bits();
            state.last_block_size = lane.context().block_size;
        }
    }

    static void skip_block_batch(
        iv::SkipBlockBatchContext<NativeBatchTickProbe> const& context)
    {
        for (auto lane : context) {
            auto& state = lane.state();
            ++state.skip_batch_calls;
            state.last_lane_count = context.size();
            state.configured_id = lane.node().id;
            state.last_index = lane.context().index.whole().bits();
            state.last_block_size = lane.context().block_size;
        }
    }
};

struct NativeBatchAliasingProbe {
    std::uint64_t id = 0;

    struct State {
        std::uint64_t tick_batch_calls = 0;
        std::uint64_t last_lane_count = 0;
        std::uint64_t configured_id = 0;
    };

    constexpr explicit NativeBatchAliasingProbe(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::tick)};
    }

    static void tick_block_batch(
        iv::TickBlockBatchContext<NativeBatchAliasingProbe> const& context)
    {
        for (auto lane : context) {
            auto& state = lane.state();
            ++state.tick_batch_calls;
            state.last_lane_count = context.size();
            state.configured_id = lane.node().id;
            for (auto& sample : lane.template output<"out">().block()) {
                sample = static_cast<float>(lane.node().id);
            }
        }
    }
};

struct NativeBatchReplaySource {
    std::uint64_t id = 0;
    static constexpr bool intrinsically_replayable = true;

    constexpr explicit NativeBatchReplaySource(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::tick)};
    }

    static void tick_block_batch(
        iv::TickBlockBatchContext<NativeBatchReplaySource> const& context)
    {
        for (auto lane : context) {
            auto output = lane.template output<"out">().block();
            auto const begin = lane.context().index.whole().bits();
            for (std::size_t i = 0; i < output.size(); ++i) {
                output[i] = static_cast<float>(lane.node().id + begin + i);
            }
        }
    }
};

struct NativeBatchBackgroundProbe {
    std::uint64_t id = 0;

    struct TockState {
        std::uint64_t tock_batch_calls = 0;
        std::uint64_t last_lane_count = 0;
        std::uint64_t configured_id = 0;
        std::uint64_t sample_rate = 0;
    };

    constexpr explicit NativeBatchBackgroundProbe(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "in", iv::InputAccess::coverage)};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::coverage)};
    }

    static void propagate_forward_coverage_batch(
        iv::PropagateForwardCoverageBatchContext<
            NativeBatchBackgroundProbe>& context)
    {
        for (auto lane : context) {
            iv::IndexCoverage const selected{iv::GlobalIndexRange{
                .begin = iv::GlobalIndex::from_bits(lane.node().id),
                .extent = context.size(),
            }};
            lane.template output<"out">().publish_region(
                iv::GlobalIndexRange{
                    .begin = iv::GlobalIndex::from_bits(lane.node().id),
                    .extent = context.size(),
                });
            lane.template output<"out">().change(selected);
        }
    }

    static void tock_coverage_batch(
        iv::TockCoverageBatchContext<NativeBatchBackgroundProbe>& context)
    {
        for (auto lane : context) {
            auto& state = lane.tock_state();
            ++state.tock_batch_calls;
            state.last_lane_count = context.size();
            state.configured_id = lane.node().id;
            state.sample_rate = lane.context().sample_rate;
        }
    }

    static void propagate_reverse_coverage_batch(
        iv::PropagateReverseCoverageBatchContext<
            NativeBatchBackgroundProbe>& context)
    {
        for (auto lane : context) {
            lane.template input<"in">().require(iv::GlobalIndexRange{
                .begin = iv::GlobalIndex::from_bits(lane.node().id),
                .extent = context.size(),
            });
        }
    }
};

struct NativeBatchTockProbe {
    std::uint64_t id = 0;

    struct TockState {
        std::uint64_t tock_batch_calls = 0;
        std::uint64_t last_lane_count = 0;
        std::uint64_t configured_id = 0;
        std::uint64_t sample_rate = 0;
    };

    constexpr explicit NativeBatchTockProbe(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::coverage)};
    }

    static void propagate_forward_coverage_batch(
        iv::PropagateForwardCoverageBatchContext<NativeBatchTockProbe>& context)
    {
        for (auto lane : context) {
            iv::IndexCoverage const selected{iv::GlobalIndexRange{
                .begin = iv::GlobalIndex::from_bits(lane.node().id),
                .extent = context.size(),
            }};
            lane.template output<"out">().publish_region(
                iv::GlobalIndexRange{
                    .begin = iv::GlobalIndex::from_bits(lane.node().id),
                    .extent = context.size(),
                });
            lane.template output<"out">().change(selected);
        }
    }

    static void tock_coverage_batch(
        iv::TockCoverageBatchContext<NativeBatchTockProbe>& context)
    {
        for (auto lane : context) {
            auto& state = lane.tock_state();
            ++state.tock_batch_calls;
            state.last_lane_count = context.size();
            state.configured_id = lane.node().id;
            state.sample_rate = lane.context().sample_rate;
        }
    }
};

struct NativeBatchActivityProbe {
    struct State {
        std::uint64_t tick_batch_calls = 0;
        std::uint64_t skip_batch_calls = 0;
        std::uint64_t tick_lane_total = 0;
        std::uint64_t skip_lane_total = 0;
    };

    // No continuous input: a block callback with one uses the shared
    // contiguous-input scratch area, which keeps its ready group scalar.
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::tick)};
    }

    bool can_skip_block() const { return true; }

    static void tick_block_batch(
        iv::TickBlockBatchContext<NativeBatchActivityProbe> const& context)
    {
        for (auto lane : context) {
            auto& state = lane.state();
            ++state.tick_batch_calls;
            state.tick_lane_total += context.size();
            for (auto& sample : lane.template output<"out">().block()) {
                sample = {};
            }
        }
    }

    static void skip_block_batch(
        iv::SkipBlockBatchContext<NativeBatchActivityProbe> const& context)
    {
        for (auto lane : context) {
            auto& state = lane.state();
            ++state.skip_batch_calls;
            state.skip_lane_total += context.size();
            for (auto& sample : lane.template output<"out">().block()) {
                sample = {};
            }
        }
    }
};

struct NativeBatchActivitySink {
    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>("first", iv::InputAccess::sequential),
            iv::input<iv::Sample>("second", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void tick_block(
        iv::TickBlockContext<NativeBatchActivitySink> const&) const
    {}
};

struct ActivityStepSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::tick)};
    }

    void tick_block(
        iv::TickBlockContext<ActivityStepSource> const& ctx) const
    {
        auto output = ctx.output<"out">().block();
        auto const value = ctx.index.whole().bits() < 128 ? 0.0f : 1.0f;
        for (auto& sample : output) sample = value;
    }
};

struct ActivityProbe {
    struct State {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::tick)};
    }

    bool can_skip_block() const { return true; }

    void tick_block(iv::TickBlockContext<ActivityProbe> const& ctx) const
    {
        ++ctx.state().tick_calls;
        auto output = ctx.output<"out">().block();
        for (auto& sample : output) sample = {};
    }

    void skip_block(iv::SkipBlockContext<ActivityProbe> const& ctx) const
    {
        ++ctx.state().skip_calls;
        auto output = ctx.output<"out">().block();
        for (auto& sample : output) sample = {};
    }
};

struct ActivityTypeDefaultProbe {
    struct State {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::tick)};
    }

    std::optional<std::size_t> ttl_samples() const { return 64; }
    bool can_skip_block() const { return true; }

    void tick_block(
        iv::TickBlockContext<ActivityTypeDefaultProbe> const& ctx) const
    {
        ++ctx.state().tick_calls;
        auto output = ctx.output<"out">().block();
        for (auto& sample : output) sample = {};
    }

    void skip_block(
        iv::SkipBlockContext<ActivityTypeDefaultProbe> const& ctx) const
    {
        ++ctx.state().skip_calls;
        auto output = ctx.output<"out">().block();
        for (auto& sample : output) sample = {};
    }
};

struct EventActivityStepSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "out",
            iv::EventTypeId::trigger,
            iv::OutputProduction::tick,
            {.max_events_per_index = 1.0 / 64.0})};
    }

    void tick_block(
        iv::TickBlockContext<EventActivityStepSource> const& ctx) const
    {
        if (ctx.index.whole().bits() >= 128 && ctx.block_size != 0)
            ctx.output<"out">().push(iv::TriggerEvent{}, 0);
    }
};

struct EventActivityProbe {
    struct State {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::event_input(
            "in", iv::EventTypeId::trigger, iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "out",
            iv::EventTypeId::trigger,
            iv::OutputProduction::tick,
            {.max_events_per_index = 1.0 / 64.0})};
    }

    std::optional<std::size_t> ttl_samples() const { return 64; }
    bool can_skip_block() const { return true; }

    void tick_block(
        iv::TickBlockContext<EventActivityProbe> const& ctx) const
    {
        ++ctx.state().tick_calls;
    }

    void skip_block(
        iv::SkipBlockContext<EventActivityProbe> const& ctx) const
    {
        ++ctx.state().skip_calls;
    }
};

void activity_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.activity_step_source">();
    auto first = graph.node<
        "iv.test.graph_jit.state_context.activity_probe">();
    first(source);
    first.ttl(128);
    auto second = graph.node<
        "iv.test.graph_jit.state_context.activity_probe">();
    second(source);
    second.ttl(128);
    graph.outputs();
}

void activity_disabled_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.activity_step_source">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.activity_probe">();
    probe(source);
    graph.outputs();
}

void activity_type_default_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.activity_step_source">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.activity_type_default_probe">();
    probe(source);
    graph.outputs();
}

void event_activity_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.event_activity_step_source">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.event_activity_probe">();
    probe.connect_event_input(0, source.event_port());
    graph.outputs();
}

struct DeclarationLayoutLeaf {
    struct State {
        std::uint64_t marker = 17;
    };

    struct TockState {
        std::uint64_t marker = 23;
    };
};

struct DeclarationLayoutParent {
    struct State {
        std::span<std::uint64_t> local;
        std::span<std::span<std::byte>> nested;
        std::uint64_t calls = 0;
        std::uint64_t local_sum = 0;
        std::uint64_t nested_sum = 0;
    };

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }

    void declare(iv::DeclarationContext<DeclarationLayoutParent> const& ctx) const
    {
        auto const& state = ctx.state();
        ctx.local_array(state.local, 3);
        DeclarationLayoutLeaf first;
        DeclarationLayoutLeaf second;
        do_declare(first, ctx, {});
        do_declare(second, ctx, {});
        ctx.nested_node_states(state.nested);
    }

    void initialize(iv::InitializationContext<DeclarationLayoutParent> const& ctx) const
    {
        auto& state = ctx.state();
        state.local[0] = 2;
        state.local[1] = 3;
        state.local[2] = 5;
    }

    void tick_block(iv::TickBlockContext<DeclarationLayoutParent> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.local_sum = state.local[0] + state.local[1] + state.local[2];
        state.nested_sum = 0;
        for (auto const child : state.nested) {
            state.nested_sum +=
                reinterpret_cast<DeclarationLayoutLeaf::State const*>(
                    child.data())->marker;
        }
    }
};

struct DeclarationLayoutProducer {
    struct State {
        std::span<std::uint64_t> values;
        std::uint64_t calls = 0;
    };

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }

    void declare(iv::DeclarationContext<DeclarationLayoutProducer> const& ctx) const
    {
        auto const& state = ctx.state();
        ctx.local_array(state.values, 3);
        ctx.export_array("declaration_layout_values", state.values);
    }

    void initialize(iv::InitializationContext<DeclarationLayoutProducer> const& ctx) const
    {
        auto& state = ctx.state();
        state.values[0] = 11;
        state.values[1] = 13;
        state.values[2] = 17;
    }

    void tick_block(iv::TickBlockContext<DeclarationLayoutProducer> const& ctx) const
    {
        ++ctx.state().calls;
    }
};

struct DeclarationLayoutConsumer {
    struct State {
        std::span<std::uint64_t> imported;
        std::uint64_t initialize_sum = 0;
        std::uint64_t tick_sum = 0;
        std::uint64_t calls = 0;
    };

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }

    void declare(iv::DeclarationContext<DeclarationLayoutConsumer> const& ctx) const
    {
        ctx.import_array("declaration_layout_values", ctx.state().imported);
    }

    void initialize(iv::InitializationContext<DeclarationLayoutConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        state.initialize_sum =
            state.imported[0] + state.imported[1] + state.imported[2];
    }

    void tick_block(iv::TickBlockContext<DeclarationLayoutConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.tick_sum =
            state.imported[0] + state.imported[1] + state.imported[2];
    }
};

struct FFTValueProducer {
    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>("samples", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::output<iv::FFTBlock>("spectra", iv::OutputProduction::tick),
        };
    }

    void constrain_ports(iv::ConstrainPortsContext<FFTValueProducer>& ctx) const
    {
        ctx.input<"samples">().pace() = 4;
        ctx.input<"samples">().pace().absolute() = 1;
        ctx.input<"samples">().history() = 4;
        ctx.output<"spectra">().pace() = 1;
        ctx.output<"spectra">().size() = 3;
    }

    void tick_block(iv::TickBlockContext<FFTValueProducer> const& ctx) const
    {
        auto const input = ctx.input<"samples">();
        auto const samples = input.block_extended().elements();
        auto output = ctx.output<"spectra">();
        auto spectra = output.block();
        for (std::size_t value = 0; value < spectra.size(); ++value) {
            auto const transform_end = (output.value_index() + value) * 4;
            auto const input_offset = static_cast<std::size_t>(
                transform_end - input.value_index());
            IV_ASSERT(input_offset <= samples.size()
                    && 4 <= samples.size() - input_offset,
                "FFT-value test producer lost its input history window");
            auto spectrum = spectra[value];
            IV_ASSERT(spectrum.size() == 3,
                "FFT-value test producer received the wrong resolved size");
            for (std::size_t bin = 0; bin < spectrum.size(); ++bin) {
                spectrum[bin] = iv::FFTBin{
                    static_cast<float>(samples[input_offset + bin]),
                    static_cast<float>(bin),
                };
            }
        }
    }
};

struct FFTValueConsumer {
    struct alignas(256) State {
        std::uint64_t calls = 0;
        std::uint64_t root_block_size = 0;
        std::uint64_t input_block_size = 0;
        std::uint64_t extended_block_size = 0;
        std::uint64_t value_index = 0;
        float preceding_bin = 0.0f;
        float first_bin = 0.0f;
        float last_bin = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample, iv::FFTBlock>(
                "spectra", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void constrain_ports(iv::ConstrainPortsContext<FFTValueConsumer>& ctx) const
    {
        ctx.input<"spectra">().size() = 3;
        ctx.input<"spectra">().history() = 1;
        ctx.input<"spectra">().pace() = 1;
    }

    void tick_block(iv::TickBlockContext<FFTValueConsumer> const& ctx) const
    {
        auto const selected = ctx.input<"spectra">();
        IV_ASSERT(selected.template is<iv::FFTBlock>(),
            "FFT-value test consumer selected the wrong continuous type");
        auto const input = selected.template as<iv::FFTBlock>();
        auto const values = input.block();
        auto const extended = input.block_extended();
        auto& state = ctx.state();
        ++state.calls;
        state.root_block_size = ctx.block_size;
        state.input_block_size = values.size();
        state.extended_block_size = extended.size();
        state.value_index = input.value_index();
        if (!extended.empty()) {
            state.preceding_bin = extended[0][0].real();
        }
        if (!values.empty()) {
            state.first_bin = values[0][0].real();
            state.last_bin = values[values.size() - 1][0].real();
        }
    }
};

struct FractionalIndexProducer {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::FractionalGlobalIndex>(
            "coordinates", iv::OutputProduction::tick)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<FractionalIndexProducer>& ctx) const
    {
        ctx.output<"coordinates">().pace() = 2;
        ctx.output<"coordinates">().pace().absolute() = 2;
    }

    void tick_block(
        iv::TickBlockContext<FractionalIndexProducer> const& ctx) const
    {
        auto output = ctx.output<"coordinates">();
        auto values = output.block();
        for (std::size_t offset = 0; offset < values.size(); ++offset) {
            auto const ordinal = output.value_index() + offset;
            values[offset] = iv::FractionalGlobalIndex::from_parts(
                iv::GlobalIndex::from_bits(ordinal),
                ordinal ^ 0x8000000000000000ull);
        }
    }
};

struct FractionalIndexConsumer {
    struct alignas(512) State {
        std::uint64_t calls = 0;
        std::uint64_t root_block_size = 0;
        std::uint64_t input_block_size = 0;
        std::uint64_t value_index = 0;
        std::uint64_t first_whole = 0;
        std::uint64_t first_fraction = 0;
        std::uint64_t last_whole = 0;
        std::uint64_t last_fraction = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::FractionalGlobalIndex>(
            "coordinates", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void constrain_ports(
        iv::ConstrainPortsContext<FractionalIndexConsumer>& ctx) const
    {
        ctx.input<"coordinates">().pace() = 1;
    }

    void tick_block(
        iv::TickBlockContext<FractionalIndexConsumer> const& ctx) const
    {
        auto const input = ctx.input<"coordinates">();
        auto const values = input.block();
        auto& state = ctx.state();
        ++state.calls;
        state.root_block_size = ctx.block_size;
        state.input_block_size = values.size();
        state.value_index = input.value_index();
        if (!values.empty()) {
            auto const first = values[0];
            auto const last = values[values.size() - 1];
            state.first_whole = first.whole().bits();
            state.first_fraction = first.fraction();
            state.last_whole = last.whole().bits();
            state.last_fraction = last.fraction();
        }
    }
};

struct PersistedFFTValueProducer {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::FFTBlock>(
            "spectra",
            iv::OutputProduction::tick,
            iv::OutputRetention::persisted)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistedFFTValueProducer>& ctx) const
    {
        ctx.output<"spectra">().size() = 3;
    }

    void tick_block(
        iv::TickBlockContext<PersistedFFTValueProducer> const& ctx) const
    {
        ctx.output<"spectra">().write_void();
    }
};

struct PersistedNonProjectRateSampleProducer {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "samples",
            iv::OutputProduction::tick,
            iv::OutputRetention::persisted)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistedNonProjectRateSampleProducer>& ctx) const
    {
        ctx.output<"samples">().pace() = 2;
    }

    void tick_block(
        iv::TickBlockContext<PersistedNonProjectRateSampleProducer> const& ctx) const
    {
        ctx.output<"samples">().write_void();
    }
};

void declaration_layout_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.declaration_layout_parent">();
    (void)graph.node<"iv.test.graph_jit.state_context.declaration_layout_producer">();
    (void)graph.node<"iv.test.graph_jit.state_context.declaration_layout_consumer">();
    graph.outputs();
}

void fft_value_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto transform = graph.node<"iv.test.graph_jit.state_context.fft_value_producer">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.fft_value_consumer">();
    transform(source);
    sink(transform);
    graph.outputs();
}

void fractional_index_value_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.fractional_index_producer">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.fractional_index_consumer">();
    sink(source);
    graph.outputs();
}

void persisted_fft_value_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.persisted_fft_value_producer">();
    graph.outputs();
}

void persisted_non_project_rate_sample_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.persisted_non_project_rate_sample_producer">();
    graph.outputs();
}

void native_batch_tick_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.native_batch_tick">(
        std::uint64_t{11});
    (void)graph.node<"iv.test.graph_jit.state_context.native_batch_tick">(
        std::uint64_t{22});
    graph.outputs();
}

void native_batch_aliasing_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.native_batch_aliasing">(
        std::uint64_t{11});
    (void)graph.node<"iv.test.graph_jit.state_context.native_batch_aliasing">(
        std::uint64_t{22});
    graph.outputs();
}

void native_batch_replay_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.native_batch_replay">(
        std::uint64_t{11});
    auto consumer = graph.node<
        "iv.test.graph_jit.state_context.native_batch_background">(
        std::uint64_t{22});
    consumer(source);
    graph.outputs();
}

void native_batch_activity_module(iv::GraphBuilder& graph)
{
    auto first = graph.node<
        "iv.test.graph_jit.state_context.native_batch_activity">();
    first.ttl(64);
    auto second = graph.node<
        "iv.test.graph_jit.state_context.native_batch_activity">();
    second.ttl(128);
    // Keeping both outputs live until one consumer prevents transient
    // storage sharing from forcing scalar fallback.
    auto sink = graph.node<
        "iv.test.graph_jit.state_context.native_batch_activity_sink">();
    sink(first, second);
    graph.outputs();
}

void native_batch_background_module(iv::GraphBuilder& graph)
{
    // These probes have no inputs, so this module isolates forward propagation
    // and Tock batching from Random Access input coverage.
    (void)graph.node<
        "iv.test.graph_jit.state_context.native_batch_tock">(
        std::uint64_t{11});
    (void)graph.node<
        "iv.test.graph_jit.state_context.native_batch_tock">(
        std::uint64_t{22});
    graph.outputs();
}

void native_batch_reverse_module(iv::GraphBuilder& graph)
{
    auto first_source = graph.node<
        "iv.test.graph_jit.state_context.background_evaluation_call">();
    auto second_source = graph.node<
        "iv.test.graph_jit.state_context.background_evaluation_call">();
    auto first = graph.node<
        "iv.test.graph_jit.state_context.native_batch_background">(
        std::uint64_t{11});
    auto second = graph.node<
        "iv.test.graph_jit.state_context.native_batch_background">(
        std::uint64_t{22});
    first(first_source);
    second(second_source);
    graph.outputs();
}

} // namespace

IV_NODE("iv.test.graph_jit.state_context.native_batch_tick", NativeBatchTickProbe);
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_aliasing",
    NativeBatchAliasingProbe);
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_replay",
    NativeBatchReplaySource);
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_background",
    NativeBatchBackgroundProbe);
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_tock",
    NativeBatchTockProbe);
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_activity",
    NativeBatchActivityProbe);
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_activity_sink",
    NativeBatchActivitySink);
IV_NODE("iv.test.graph_jit.state_context.activity_step_source", ActivityStepSource);
IV_NODE("iv.test.graph_jit.state_context.activity_probe", ActivityProbe);
IV_NODE("iv.test.graph_jit.state_context.activity_type_default_probe", ActivityTypeDefaultProbe);
IV_NODE("iv.test.graph_jit.state_context.event_activity_step_source", EventActivityStepSource);
IV_NODE("iv.test.graph_jit.state_context.event_activity_probe", EventActivityProbe);
IV_NODE("iv.test.graph_jit.state_context.declaration_layout_parent", DeclarationLayoutParent);
IV_NODE("iv.test.graph_jit.state_context.declaration_layout_producer", DeclarationLayoutProducer);
IV_NODE("iv.test.graph_jit.state_context.declaration_layout_consumer", DeclarationLayoutConsumer);
IV_NODE("iv.test.graph_jit.state_context.fft_value_producer", FFTValueProducer);
IV_NODE("iv.test.graph_jit.state_context.fft_value_consumer", FFTValueConsumer);
IV_NODE("iv.test.graph_jit.state_context.fractional_index_producer", FractionalIndexProducer);
IV_NODE("iv.test.graph_jit.state_context.fractional_index_consumer", FractionalIndexConsumer);
IV_NODE("iv.test.graph_jit.state_context.persisted_fft_value_producer", PersistedFFTValueProducer);
IV_NODE("iv.test.graph_jit.state_context.persisted_non_project_rate_sample_producer", PersistedNonProjectRateSampleProducer);
IV_MODULE("iv.test.graph_jit.state_context.activity_module", activity_module);
IV_MODULE("iv.test.graph_jit.state_context.activity_disabled_module", activity_disabled_module);
IV_MODULE("iv.test.graph_jit.state_context.activity_type_default_module", activity_type_default_module);
IV_MODULE("iv.test.graph_jit.state_context.event_activity_module", event_activity_module);
IV_MODULE("iv.test.graph_jit.state_context.declaration_layout_module", declaration_layout_module);
IV_MODULE("iv.test.graph_jit.state_context.fft_value_module", fft_value_module);
IV_MODULE("iv.test.graph_jit.state_context.fractional_index_value_module", fractional_index_value_module);
IV_MODULE("iv.test.graph_jit.state_context.persisted_fft_value_module", persisted_fft_value_module);
IV_MODULE("iv.test.graph_jit.state_context.persisted_non_project_rate_sample_module", persisted_non_project_rate_sample_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_tick_module",
    native_batch_tick_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_aliasing_module",
    native_batch_aliasing_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_replay_module",
    native_batch_replay_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_background_module",
    native_batch_background_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_activity_module",
    native_batch_activity_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_reverse_module",
    native_batch_reverse_module);
)cpp";
    static constexpr std::string_view part_4 = R"cpp(
namespace {
// Background-evaluated sources whose playback the tests supply directly as a
// TickMaterializationSnapshot. Their own evaluation is covered elsewhere.
struct PlaybackSampleSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::coverage)};
    }

    void tock_coverage(iv::TockCoverageContext<PlaybackSampleSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<PlaybackSampleSource>&) const {}
};

struct PlaybackTriggerSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "out",
            iv::EventTypeId::trigger,
            iv::OutputProduction::coverage,
            {.max_events_per_index = 0.25})};
    }

    void tock_coverage(iv::TockCoverageContext<PlaybackTriggerSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<PlaybackTriggerSource>&) const {}
};

// Records its complete visible history window: window[k] = get(4 - k).
struct PlaybackHistoryConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::array<float, 5> window{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PlaybackHistoryConsumer>& ctx) const
    {
        ctx.input<"in">().history() = 4;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PlaybackHistoryConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        for (std::size_t k = 0; k < state.window.size(); ++k) {
            state.window[k] = static_cast<float>(
                ctx.template input<0>().get(state.window.size() - 1 - k));
        }
    }
};

// A second consumer type, so a test can tell two consumers apart by state
// size.
struct PlaybackHistoryPeer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::array<float, 5> window{};
        std::uint64_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    void constrain_ports(iv::ConstrainPortsContext<PlaybackHistoryPeer>& ctx) const
    {
        ctx.input<"in">().history() = 4;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<PlaybackHistoryPeer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        for (std::size_t k = 0; k < state.window.size(); ++k) {
            state.window[k] = static_cast<float>(
                ctx.template input<0>().get(state.window.size() - 1 - k));
        }
        state.marker = 0x91e3u;
    }
};

// left[k] and right[k] = get(2 - k, channel).
struct PlaybackStereoHistoryConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::array<float, 3> left{};
        std::array<float, 3> right{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PlaybackStereoHistoryConsumer>& ctx) const
    {
        ctx.input<"in">().history() = 2;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PlaybackStereoHistoryConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        for (std::size_t k = 0; k < state.left.size(); ++k) {
            auto const history = state.left.size() - 1 - k;
            state.left[k] = static_cast<float>(
                ctx.template input<0>().get(history, 0));
            state.right[k] = static_cast<float>(
                ctx.template input<0>().get(history, 1));
        }
    }
};

// Records the event times of its window [index - 32, index + block).
struct PlaybackEventHistoryConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t count = 0;
        std::array<std::uint64_t, 8> times{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PlaybackEventHistoryConsumer>& ctx) const
    {
        ctx.input<"trigger">().history() = 32;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PlaybackEventHistoryConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        constexpr std::size_t history = 32;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(),
            ctx.block_size + history);
        state.count = events.size();
        for (std::size_t i = 0; i < state.times.size(); ++i) {
            state.times[i] = i < events.size() ? events[i].time.bits() : 0;
        }
    }
};

struct PlaybackEventHistoryPeer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t count = 0;
        std::array<std::uint64_t, 8> times{};
        std::uint64_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PlaybackEventHistoryPeer>& ctx) const
    {
        ctx.input<"trigger">().history() = 32;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PlaybackEventHistoryPeer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        constexpr std::size_t history = 32;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(),
            ctx.block_size + history);
        state.count = events.size();
        for (std::size_t i = 0; i < state.times.size(); ++i) {
            state.times[i] = i < events.size() ? events[i].time.bits() : 0;
        }
        state.marker = 0x91e4u;
    }
};

void playback_sample_history_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_history_consumer">();
    sink(source);
    graph.outputs();
}

// This replacement has the same node and connection shape as
// playback_sample_history_module, but its module definition gives the
// ConfiguredGraph a different identity. The event replacement below does the
// same for playback_event_history_module. They exercise reuse of the same
// generation-local bundle/port coordinates by different semantic nodes.
void playback_sample_history_replacement_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_history_consumer">();
    sink(source);
    graph.outputs();
}

void playback_sample_window_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink(source);
    graph.outputs();
}

void playback_mixed_sample_module(iv::GraphBuilder& graph)
{
    auto realtime = graph.node<"iv.test.graph_jit.state_context.large_history_ramp_source">();
    auto background = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_stereo_history_consumer">();
    sink(graph.tile<iv::stereo>(realtime, background));
    graph.outputs();
}

void playback_event_history_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_trigger_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_event_history_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void playback_event_history_replacement_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_trigger_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_event_history_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void playback_mixed_event_module(iv::GraphBuilder& graph)
{
    auto realtime = graph.node<"iv.test.graph_jit.state_context.trigger_event_source">();
    auto background = graph.node<"iv.test.graph_jit.state_context.playback_trigger_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_event_history_consumer">();
    std::array ports{realtime.event_port(), background.event_port()};
    sink.connect_event_input(0, graph.make_event_port(ports));
    graph.outputs();
}

// The consumer fed by the realtime source and the consumer fed by playback
// exchange sources in the rewired successor.
void playback_sample_rewire_module(iv::GraphBuilder& graph)
{
    auto realtime = graph.node<"iv.test.graph_jit.state_context.large_history_ramp_source">();
    auto background = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto realtime_fed = graph.node<"iv.test.graph_jit.state_context.playback_history_consumer">();
    auto background_fed = graph.node<"iv.test.graph_jit.state_context.playback_history_peer">();
    realtime_fed(realtime);
    background_fed(background);
    graph.outputs();
}

void playback_event_rewire_module(iv::GraphBuilder& graph)
{
    auto realtime = graph.node<"iv.test.graph_jit.state_context.trigger_event_source">();
    auto background = graph.node<"iv.test.graph_jit.state_context.playback_trigger_source">();
    auto realtime_fed = graph.node<"iv.test.graph_jit.state_context.playback_event_history_consumer">();
    auto background_fed = graph.node<"iv.test.graph_jit.state_context.playback_event_history_peer">();
    realtime_fed.connect_event_input(0, realtime.event_port());
    background_fed.connect_event_input(0, background.event_port());
    graph.outputs();
}
} // namespace

IV_NODE("iv.test.graph_jit.state_context.playback_sample_source", PlaybackSampleSource);
IV_NODE("iv.test.graph_jit.state_context.playback_trigger_source", PlaybackTriggerSource);
IV_NODE("iv.test.graph_jit.state_context.playback_history_consumer", PlaybackHistoryConsumer);
IV_NODE("iv.test.graph_jit.state_context.playback_history_peer", PlaybackHistoryPeer);
IV_NODE("iv.test.graph_jit.state_context.playback_stereo_history_consumer", PlaybackStereoHistoryConsumer);
IV_NODE("iv.test.graph_jit.state_context.playback_event_history_consumer", PlaybackEventHistoryConsumer);
IV_NODE("iv.test.graph_jit.state_context.playback_event_history_peer", PlaybackEventHistoryPeer);
IV_MODULE("iv.test.graph_jit.state_context.playback_sample_history_module", playback_sample_history_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_sample_history_replacement_module", playback_sample_history_replacement_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_sample_window_module", playback_sample_window_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_mixed_sample_module", playback_mixed_sample_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_event_history_module", playback_event_history_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_event_history_replacement_module", playback_event_history_replacement_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_mixed_event_module", playback_mixed_event_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_sample_rewire_module", playback_sample_rewire_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_event_rewire_module", playback_event_rewire_module);
)cpp";
    static_assert(part_1.size() < 60 * 1024);
    static_assert(part_2.size() < 60 * 1024);
    static_assert(part_3.size() < 60 * 1024);
    static_assert(part_4.size() < 60 * 1024);
    static std::string const source = [] {
        std::string result;
        result.reserve(
            part_1.size() + part_2.size() + part_3.size() + part_4.size());
        result.append(part_1);
        result.append(part_2);
        result.append(part_3);
        result.append(part_4);
        return result;
    }();
    return std::string_view{source};
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
    iv::PackageJit package_jit(startup_config.initialize());
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

std::shared_ptr<iv::ConfiguredGraph const> configured_sample_feedback_graph(
    iv::PackageRevision const& revision,
    std::size_t latency = 6,
    iv::Sample initial_value = iv::Sample{-0.625f})
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit sample-feedback builder session");
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
        graph, graph_jit_sample_feedback_a_id, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, graph_jit_sample_feedback_b_id, std::nullopt, {});
    first(second);
    second(static_cast<iv::SamplePortRef>(first).detach(latency, initial_value));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}


std::shared_ptr<iv::ConfiguredGraph const> configured_multi_branch_sample_feedback_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit multi-branch sample-feedback builder session");
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
    auto node = iv::details::configure_package_definition_provider(
        graph, graph_jit_multi_branch_sample_feedback_id, std::nullopt, {});
    auto const output = static_cast<iv::SamplePortRef>(node);
    node.connect_input(0, output.detach(4, iv::Sample{0.0f}));
    node.connect_input(1, output.detach(8, iv::Sample{0.0f}));
    node.connect_input(2, output.detach(6, iv::Sample{-2.0f}));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}


std::shared_ptr<iv::ConfiguredGraph const> configured_temporal_sample_feedback_graph(
    iv::PackageRevision const& revision,
    std::size_t latency = 6,
    iv::Sample initial_value = iv::Sample{-0.625f})
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit temporal sample-feedback builder session");
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
    auto node = iv::details::configure_package_definition_provider(
        graph, graph_jit_temporal_sample_feedback_id, std::nullopt, {});
    node(static_cast<iv::SamplePortRef>(node).detach(latency, initial_value));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}


std::shared_ptr<iv::ConfiguredGraph const> configured_revising_sample_feedback_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit revising sample-feedback builder session");
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
    auto node = iv::details::configure_package_definition_provider(
        graph, graph_jit_revising_sample_feedback_id, std::nullopt, {});
    node(static_cast<iv::SamplePortRef>(node).detach(6, iv::Sample{-1.0f}));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}


std::shared_ptr<iv::ConfiguredGraph const>
configured_projected_revising_sample_feedback_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit projected revising sample-feedback builder session");
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

    iv::GraphBuilder builder(session.get());
    auto node = iv::details::configure_package_definition_provider(
        builder, graph_jit_projected_revising_sample_feedback_id, std::nullopt, {});
    node(static_cast<iv::SamplePortRef>(node).detach(6, iv::Sample{-1.0f}));
    builder.outputs();
    auto base = std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
    auto graph = std::make_shared<iv::ConfiguredGraph>(*base);

    std::vector<iv::ConfiguredSampleConnection> projected;
    bool split_detached = false;
    for (auto const& connection :
         base->connections.configured_sample_connections()) {
        if (!configured_sample_connection_is_detached(connection)) {
            projected.push_back(connection);
            continue;
        }
        if (split_detached
            || connection.source_channels.size() != 1
            || connection.target_channels.size() != 2) {
            throw std::runtime_error(
                "GraphJit projected revising sample-feedback fixture lost its detached connection shape");
        }
        split_detached = true;
        for (std::size_t target_index = 0;
             target_index < connection.target_channels.size(); ++target_index) {
            auto const target = connection.target_channels[target_index];
            if (target_index == 0) {
                projected.push_back(iv::ConfiguredSampleConnection{
                    .source_type = iv::ChannelTypeId::stereo,
                    .source_channels = {
                        connection.source_channels.front(),
                        connection.source_channels.front(),
                    },
                    .target_type = iv::ChannelTypeId::mono,
                    .target_channels = {target},
                });
            } else {
                projected.push_back(iv::ConfiguredSampleConnection{
                    .source_type = iv::ChannelTypeId::mono,
                    .source_channels = connection.source_channels,
                    .target_type = iv::ChannelTypeId::mono,
                    .target_channels = {target},
                });
            }
        }
    }
    if (!split_detached) {
        throw std::runtime_error(
            "GraphJit projected revising sample-feedback fixture lost its detach");
    }
    graph->connections = iv::GraphBuilderConnections::from_configured_connections(
        projected,
        base->connections.configured_event_connections());
    return graph;
}


std::shared_ptr<iv::ConfiguredGraph const> configured_converted_sample_feedback_graph(
    iv::PackageRevision const& revision,
    std::size_t latency = 6,
    iv::Sample initial_value = iv::Sample{-0.25f})
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit converted sample-feedback builder session");
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
    auto node = iv::details::configure_package_definition_provider(
        graph, graph_jit_converted_sample_feedback_id, std::nullopt, {});
    node(static_cast<iv::SamplePortRef>(node).detach(latency, initial_value));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}


std::shared_ptr<iv::ConfiguredGraph const> configured_projected_sample_feedback_graph(
    iv::PackageRevision const& revision,
    std::size_t latency = 6,
    iv::Sample initial_value = iv::Sample{-0.25f})
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit projected sample-feedback builder session");
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

    iv::GraphBuilder builder(session.get());
    auto temporal = iv::details::configure_package_definition_provider(
        builder, graph_jit_temporal_sample_feedback_id, std::nullopt, {});
    auto converted = iv::details::configure_package_definition_provider(
        builder, graph_jit_converted_sample_feedback_id, std::nullopt, {});
    temporal(converted);
    converted(static_cast<iv::SamplePortRef>(temporal).detach(
        latency, initial_value));
    builder.outputs();
    auto base = std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
    auto graph = std::make_shared<iv::ConfiguredGraph>(*base);

    std::vector<iv::ConfiguredSampleConnection> projected;
    bool split_detached = false;
    for (auto const& connection :
         base->connections.configured_sample_connections()) {
        if (!configured_sample_connection_is_detached(connection)) {
            projected.push_back(connection);
            continue;
        }
        if (split_detached
            || connection.source_channels.size() != 1
            || connection.target_channels.size() != 2) {
            throw std::runtime_error(
                "GraphJit projected sample-feedback fixture lost its detached connection shape");
        }
        split_detached = true;
        for (std::size_t target_index = 0;
             target_index < connection.target_channels.size(); ++target_index) {
            auto const target = connection.target_channels[target_index];
            if (target_index == 0) {
                // Force the normalized projection path to preserve a real
                // channel-count conversion boundary. The same mono source is
                // gathered as semantic stereo and stereo->mono conversion
                // therefore produces the same value while exercising mixing.
                projected.push_back(iv::ConfiguredSampleConnection{
                    .source_type = iv::ChannelTypeId::stereo,
                    .source_channels = {
                        connection.source_channels.front(),
                        connection.source_channels.front(),
                    },
                    .target_type = iv::ChannelTypeId::mono,
                    .target_channels = {target},
                });
            } else {
                projected.push_back(iv::ConfiguredSampleConnection{
                    .source_type = iv::ChannelTypeId::mono,
                    .source_channels = connection.source_channels,
                    .target_type = iv::ChannelTypeId::mono,
                    .target_channels = {target},
                });
            }
        }
    }
    if (!split_detached) {
        throw std::runtime_error(
            "GraphJit projected sample-feedback fixture lost its detach");
    }
    graph->connections = iv::GraphBuilderConnections::from_configured_connections(
        projected,
        base->connections.configured_event_connections());
    return graph;
}


std::shared_ptr<iv::ConfiguredGraph const>
configured_unequal_latency_projected_sample_feedback_graph(
    iv::PackageRevision const& revision,
    std::size_t latency = 6,
    iv::Sample initial_value = iv::Sample{-0.25f})
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit unequal-latency projected sample-feedback builder session");
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

    iv::GraphBuilder builder(session.get());
    auto temporal = iv::details::configure_package_definition_provider(
        builder, graph_jit_temporal_sample_feedback_id, std::nullopt, {});
    auto converted = iv::details::configure_package_definition_provider(
        builder, graph_jit_converted_sample_feedback_id, std::nullopt, {});
    temporal(converted);
    converted(static_cast<iv::SamplePortRef>(temporal).detach(
        latency, initial_value));
    builder.outputs();
    auto base = std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
    auto graph = std::make_shared<iv::ConfiguredGraph>(*base);

    std::optional<iv::SampleOutputChannelId> converted_output;
    std::optional<iv::ConfiguredSampleConnection> detached;
    for (auto const& connection :
         base->connections.configured_sample_connections()) {
        if (configured_sample_connection_is_detached(connection)) {
            detached = connection;
        } else if (connection.source_channels.size() == 1) {
            converted_output = connection.source_channels.front().channel;
        }
    }
    if (!converted_output || !detached
        || detached->source_channels.size() != 1
        || detached->target_channels.size() != 2) {
        throw std::runtime_error(
            "GraphJit unequal-latency projected sample-feedback fixture lost its base connection shape");
    }

    std::vector<iv::ConfiguredSampleConnection> projected;
    for (auto const& connection :
         base->connections.configured_sample_connections()) {
        if (!configured_sample_connection_is_detached(connection))
            projected.push_back(connection);
    }
    projected.push_back(iv::ConfiguredSampleConnection{
        .source_type = iv::ChannelTypeId::stereo,
        .source_channels = {
            detached->source_channels.front(),
            iv::ConfiguredSampleConnectionSource{
                .channel = *converted_output,
                .detach = detached->source_channels.front().detach,
            },
        },
        .target_type = iv::ChannelTypeId::mono,
        .target_channels = {detached->target_channels[0]},
    });
    projected.push_back(iv::ConfiguredSampleConnection{
        .source_type = iv::ChannelTypeId::mono,
        .source_channels = detached->source_channels,
        .target_type = iv::ChannelTypeId::mono,
        .target_channels = {detached->target_channels[1]},
    });
    graph->connections = iv::GraphBuilderConnections::from_configured_connections(
        projected,
        base->connections.configured_event_connections());
    return graph;
}


std::shared_ptr<iv::ConfiguredGraph const>
configured_permuted_unequal_latency_projected_sample_feedback_graph(
    iv::PackageRevision const& revision,
    std::size_t latency = 6,
    iv::Sample initial_value = iv::Sample{-0.25f})
{
    auto base = configured_unequal_latency_projected_sample_feedback_graph(
        revision, latency, initial_value);
    auto graph = std::make_shared<iv::ConfiguredGraph>(*base);
    auto const configured = base->connections.configured_sample_connections();
    auto connections = std::vector<iv::ConfiguredSampleConnection>(
        configured.begin(), configured.end());

    std::vector<std::size_t> detached_indices;
    for (std::size_t i = 0; i < connections.size(); ++i) {
        if (!configured_sample_connection_is_detached(connections[i])) continue;
        if (connections[i].target_channels.size() != 1) {
            throw std::runtime_error(
                "GraphJit permuted projected feedback fixture lost its mono target projection");
        }
        detached_indices.push_back(i);
    }
    if (detached_indices.size() != 2) {
        throw std::runtime_error(
            "GraphJit permuted projected feedback fixture lost its detached contributions");
    }

    std::swap(
        connections[detached_indices[0]].target_channels.front(),
        connections[detached_indices[1]].target_channels.front());
    graph->connections = iv::GraphBuilderConnections::from_configured_connections(
        connections,
        base->connections.configured_event_connections());
    return graph;
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_sample_feedback_scc_fanout_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit sample-feedback SCC-fanout builder session");
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
        graph, graph_jit_sample_feedback_a_id, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, graph_jit_sample_feedback_b_id, std::nullopt, {});
    auto identity_observer = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.sample_consumer",
        std::nullopt,
        {});
    auto converted_history_observer =
        iv::details::configure_package_definition_provider(
            graph,
            "iv.test.graph_jit.state_context.stereo_history_consumer",
            std::nullopt,
            {});

    first(second);
    second(static_cast<iv::SamplePortRef>(first).detach(
        6, iv::Sample{-0.625f}));
    identity_observer(first);
    converted_history_observer(first);
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
rewired_large_history_graph(
    std::shared_ptr<iv::ConfiguredGraph const> const& predecessor)
{
    auto successor = std::make_shared<iv::ConfiguredGraph>(*predecessor);
    auto sample_connections = std::vector<iv::ConfiguredSampleConnection>(
        predecessor->connections.configured_sample_connections().begin(),
        predecessor->connections.configured_sample_connections().end());
    if (sample_connections.size() != 2) {
        throw std::runtime_error(
            "unexpected large-history transition connection count");
    }
    auto const event_connections = std::vector<iv::ConfiguredEventConnection>(
        predecessor->connections.configured_event_connections().begin(),
        predecessor->connections.configured_event_connections().end());
    sample_connections[1].source_type = sample_connections[0].source_type;
    sample_connections[1].source_channels =
        sample_connections[0].source_channels;
    successor->connections =
        iv::GraphBuilderConnections::from_configured_connections(
            sample_connections, event_connections);
    return successor;
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_merged_feed_forward_event_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit merged-event builder session");
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
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto second = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.limited_trigger_event_source",
        std::nullopt,
        {});
    auto sink = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        std::nullopt,
        {});
    auto sink_b = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        std::nullopt,
        {});

    auto const first_port = first.event_port();
    auto const second_port = second.event_port();
    if (first_port.type != second_port.type
        || first_port.sources().size() != 1
        || second_port.sources().size() != 1) {
        throw std::runtime_error(
            "GraphJit merged-event fixture lost its source shape");
    }
    // Put the limited producer first in the semantic source order. Its raw
    // EventOutputPortId sequence is written across several callback slices
    // before fan-in merges both source sequences.
    std::array merged_sources{
        second_port.sources().front(),
        first_port.sources().front(),
    };
    auto merged = iv::EventPortRef(graph, first_port.type, merged_sources);
    sink.connect_event_input(0, merged);
    sink_b.connect_event_input(0, merged);
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_bounded_merged_feed_forward_event_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit bounded merged-event builder session");
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
    auto burst = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.fan_in_burst_event_source",
        std::nullopt,
        {});
    auto sparse = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.fan_in_sparse_event_source",
        std::nullopt,
        {});
    auto sink = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        std::nullopt,
        {});

    auto const burst_port = burst.event_port();
    auto const sparse_port = sparse.event_port();
    if (burst_port.type != sparse_port.type
        || burst_port.sources().size() != 1
        || sparse_port.sources().size() != 1) {
        throw std::runtime_error(
            "GraphJit bounded merged-event fixture lost its source shape");
    }
    std::array merged_sources{
        burst_port.sources().front(),
        sparse_port.sources().front(),
    };
    auto merged = iv::EventPortRef(graph, burst_port.type, merged_sources);
    sink.connect_event_input(0, merged);
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_overlapping_event_fan_in_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit overlapping-event builder session");
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
    auto shared = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto first = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto second = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto first_sink = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        std::nullopt,
        {});
    auto second_sink = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        std::nullopt,
        {});

    std::array<iv::EventPortRef, 2> first_sources{
        shared.event_port(), first.event_port()};
    std::array<iv::EventPortRef, 2> second_sources{
        shared.event_port(), second.event_port()};
    first_sink.connect_event_input(
        0,
        iv::EventPortRef{
            graph, std::span<iv::EventPortRef const>{first_sources}});
    second_sink.connect_event_input(
        0,
        iv::EventPortRef{
            graph, std::span<iv::EventPortRef const>{second_sources}});
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_structural_event_source_conversion_fan_in_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit structural-event builder session");
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
    auto converted = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto source = iv::details::configure_package_definition_provider(
            graph,
            "iv.test.graph_jit.state_context.midi_event_source",
            std::nullopt,
            {});
        auto raw = source.event_port();
        boundary.event_outputs(iv::EventPortRef{
            graph, iv::EventTypeId::trigger, raw.sources()});
    }, "midi-to-trigger-boundary");
    auto direct = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto source = iv::details::configure_package_definition_provider(
            graph,
            "iv.test.graph_jit.state_context.trigger_event_source",
            std::nullopt,
            {});
        boundary.event_outputs(source.event_port());
    }, "trigger-boundary");
    auto sink = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        std::nullopt,
        {});
    std::array<iv::EventPortRef, 2> sources{
        converted.event_port(), direct.event_port()};
    sink.connect_event_input(
        0,
        iv::EventPortRef{graph, std::span<iv::EventPortRef const>{sources}});
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const> configured_merged_converted_event_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) throw std::runtime_error("could not create merged converted-event session");
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
        graph, "iv.test.graph_jit.state_context.midi_event_source", std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, "iv.test.graph_jit.state_context.midi_event_source", std::nullopt, {});
    auto sink_a = iv::details::configure_package_definition_provider(
        graph, "iv.test.graph_jit.state_context.trigger_event_consumer", std::nullopt, {});
    auto sink_b = iv::details::configure_package_definition_provider(
        graph, "iv.test.graph_jit.state_context.trigger_event_consumer", std::nullopt, {});
    auto const first_port = first.event_port();
    auto const second_port = second.event_port();
    std::array sources{first_port.sources().front(), second_port.sources().front()};
    auto merged = iv::EventPortRef(graph, first_port.type, sources);
    sink_a.connect_event_input(0, merged);
    sink_b.connect_event_input(0, merged);
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_merged_target_history_event_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create merged target-history event session");
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
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto second = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto sink = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.retained_trigger_event_consumer",
        std::nullopt,
        {});
    auto const first_port = first.event_port();
    auto const second_port = second.event_port();
    std::array sources{
        first_port.sources().front(), second_port.sources().front()};
    sink.connect_event_input(
        0, iv::EventPortRef(graph, first_port.type, sources));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const> configured_merged_retained_event_graph(
    iv::PackageRevision const& revision,
    bool persistent_ring)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) throw std::runtime_error("could not create merged retained-event session");
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
    auto const source_id = persistent_ring
        ? "iv.test.graph_jit.state_context.persistent_event_ring_source"
        : "iv.test.graph_jit.state_context.retained_trigger_event_source";
    auto const sink_id = persistent_ring
        ? "iv.test.graph_jit.state_context.persistent_event_ring_consumer"
        : "iv.test.graph_jit.state_context.retained_trigger_event_consumer";
    auto first = iv::details::configure_package_definition_provider(
        graph, source_id, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, source_id, std::nullopt, {});
    auto sink = iv::details::configure_package_definition_provider(
        graph, sink_id, std::nullopt, {});
    auto const first_port = first.event_port();
    auto const second_port = second.event_port();
    std::array sources{first_port.sources().front(), second_port.sources().front()};
    sink.connect_event_input(
        0, iv::EventPortRef(graph, first_port.type, sources));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_disconnected_retained_event_output_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create disconnected retained-event session");
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
    (void)iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.retained_trigger_event_source",
        std::nullopt,
        {});
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_event_feedback_scc_external_fanout_graph(
    iv::PackageRevision const& revision,
    std::string_view observer_definition =
        "iv.test.graph_jit.state_context.trigger_event_consumer",
    std::string_view first_definition = graph_jit_event_feedback_a_id,
    std::string_view second_definition = graph_jit_event_feedback_b_id)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create GraphJit event-feedback SCC-fanout builder session");
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
        graph, second_definition, std::nullopt, {});
    auto observer = iv::details::configure_package_definition_provider(
        graph, observer_definition, std::nullopt, {});
    first.connect_event_input(0, second.event_port());
    second.connect_event_input(0, first.event_port().detach(10));
    observer.connect_event_input(0, first.event_port());
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
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

std::shared_ptr<iv::ConfiguredGraph const>
configured_mixed_event_feedback_graph(iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create mixed event-feedback builder session");
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
    auto ingress = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto first = iv::details::configure_package_definition_provider(
        graph, graph_jit_event_feedback_a_id, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, graph_jit_event_feedback_b_id, std::nullopt, {});
    first.connect_event_input(0, second.event_port());
    std::array sources{
        first.event_port().detach(10),
        ingress.event_port(),
    };
    second.connect_event_input(0, iv::EventPortRef(graph, sources));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_staged_event_fan_in_graph(iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create staged event fan-in builder session");
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
    auto ingress = iv::details::configure_package_definition_provider(
        graph,
        "iv.test.graph_jit.state_context.trigger_event_source",
        std::nullopt,
        {});
    auto first = iv::details::configure_package_definition_provider(
        graph,
        graph_jit_retained_fan_in_event_feedback_a_id,
        std::nullopt,
        {});
    auto second = iv::details::configure_package_definition_provider(
        graph, graph_jit_event_feedback_b_id, std::nullopt, {});

    auto const ingress_port = ingress.event_port();
    auto const feedback_port = second.event_port();
    std::array sources{
        feedback_port.sources().front(),
        ingress_port.sources().front(),
    };
    first.connect_event_input(
        0, iv::EventPortRef(graph, feedback_port.type, sources));
    second.connect_event_input(0, first.event_port().detach(10));
    graph.outputs();
    return std::make_shared<iv::ConfiguredGraph const>(
        iv::details::take_built_graph(session.get()));
}

std::shared_ptr<iv::ConfiguredGraph const>
configured_retained_converted_event_feedback_graph(
    iv::PackageRevision const& revision)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(
        iv::details::iv_builder_session_create(),
        iv::details::iv_builder_session_destroy);
    if (!session) {
        throw std::runtime_error(
            "could not create retained converted event-feedback builder session");
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
        graph, graph_jit_retained_dual_event_feedback_a_id, std::nullopt, {});
    auto second = iv::details::configure_package_definition_provider(
        graph, graph_jit_dual_event_feedback_b_id, std::nullopt, {});
    first.connect_event_input(0, second.event_port(0));
    first.connect_event_input(1, second.event_port(1));
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
        "iv.test.graph_jit.state_context.background_evaluation_call"));
    EXPECT_TRUE(has_module_definition(graph_jit_background_execution_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_native_batch_tick_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_native_batch_aliasing_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_native_batch_replay_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_native_batch_background_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_native_batch_activity_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_native_batch_reverse_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_declaration_layout_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_activity_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_activity_disabled_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_activity_type_default_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_event_activity_module_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.stereo_ramp_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.stereo_planar_consumer"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.stereo_sample_consumer"));
    EXPECT_TRUE(has_module_definition(graph_jit_sample_fanout_conversion_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_sample_revision_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_persistent_sample_revision_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_composed_sample_revision_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_stereo_conversion_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_stereo_history_conversion_module_id));
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
    EXPECT_TRUE(has_module_definition(graph_jit_large_history_rewire_module_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.five_sample_delay"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.latency_compensation_probe"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.interleaved_latency_compensation_probe"));
    EXPECT_TRUE(has_leaf_definition(graph_jit_sample_feedback_a_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_sample_feedback_b_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_projected_revising_sample_feedback_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_multi_branch_sample_feedback_id));
    EXPECT_TRUE(has_module_definition(graph_jit_latency_compensation_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_latency_conversion_fanout_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_composed_latency_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_composed_history_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_projected_composition_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_fft_value_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_fractional_index_value_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_persisted_fft_value_module_id));
    EXPECT_TRUE(has_module_definition(
        graph_jit_persisted_non_project_rate_sample_module_id));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.trigger_event_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.limited_trigger_event_source"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.trigger_event_consumer"));
    EXPECT_TRUE(has_leaf_definition(graph_jit_latent_event_feedback_a_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_persistent_latent_event_feedback_a_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_persistent_latent_boundary_event_feedback_a_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_boundary_event_feedback_a_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_boundary_event_feedback_b_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_retained_dual_event_feedback_a_id));
    EXPECT_TRUE(has_leaf_definition(graph_jit_dual_event_feedback_b_id));
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
        "iv.test.graph_jit.state_context.persistent_event_feedback_consumer"));
    EXPECT_TRUE(has_leaf_definition(
        "iv.test.graph_jit.state_context.retained_midi_event_consumer"));
    EXPECT_TRUE(has_module_definition(graph_jit_persistent_event_ring_module_id));
    EXPECT_TRUE(has_module_definition(graph_jit_retained_converted_event_fanout_module_id));
}

TEST_F(GraphJitRuntimeFixture, StateAndBackgroundStateContexts)
{
    auto stateful = compile(graph_jit_stateful_module_id, 100);
    ASSERT_TRUE(stateful.succeeded())
        << (stateful.diagnostics.empty() ? "" : stateful.diagnostics.front().message);
    ASSERT_TRUE(stateful.compiled_graph->root_operations.valid());
    expect_single_node_canonical_regions(
        stateful.compiled_graph->node_layout,
        stateful.compiled_graph->tock_state_layout,
        sizeof(StatefulProbeStateMirror),
        sizeof(StatefulProbeBackgroundStateMirror));
    ASSERT_EQ(stateful.compiled_graph->node_layout.nodes.size(), 1u);

    auto stateful_storage = stateful.compiled_graph->node_layout.create_storage(resources);
    stateful_storage.initialize();
    auto stateful_tock_states =
        stateful.compiled_graph->tock_state_layout.create_storage(resources);
    stateful_tock_states.initialize();
    auto* state = static_cast<StatefulProbeStateMirror*>(stateful_storage.state_ptr(0));
    auto* background = static_cast<StatefulProbeBackgroundStateMirror*>(
        stateful_tock_states.state_ptr(0));
    ASSERT_NE(state, nullptr);
    ASSERT_NE(background, nullptr);
    auto const& stateful_node = stateful.compiled_graph->node_layout.nodes.front();
    auto const& stateful_tock_node =
        stateful.compiled_graph->tock_state_layout.nodes.front();
    EXPECT_EQ(stateful_node.state_alignment, alignof(StatefulProbeStateMirror));
    EXPECT_EQ(
        stateful_tock_node.state_alignment,
        alignof(StatefulProbeBackgroundStateMirror));
    EXPECT_EQ(
        reinterpret_cast<std::uintptr_t>(state) % alignof(StatefulProbeStateMirror),
        0u);
    EXPECT_EQ(
        reinterpret_cast<std::uintptr_t>(background)
            % alignof(StatefulProbeBackgroundStateMirror),
        0u);
    EXPECT_EQ(
        static_cast<std::byte*>(static_cast<void*>(state)),
        stateful_storage.buffer().data()
            + stateful_node.state_offset);

    stateful.compiled_graph->root_operations.tick_block(
        stateful_storage.buffer().data(), &empty_tick_invocation, index(17), 32);
    EXPECT_EQ(state->tick_calls, 1u);
    EXPECT_EQ(state->skip_calls, 0u);
    EXPECT_EQ(state->last_index, 17u);
    EXPECT_EQ(state->last_block_size, 32u);
    EXPECT_EQ(state->sample_rate, 88200u);
    EXPECT_EQ(state->observed_state_extent, sizeof(StatefulProbeStateMirror));
    EXPECT_EQ(state->observed_background_extent, 0u);
    EXPECT_EQ(background->tick_calls, 0u);
    EXPECT_EQ(background->skip_calls, 0u);
    EXPECT_EQ(background->last_index, 0u);
    EXPECT_EQ(background->last_block_size, 0u);
    EXPECT_EQ(background->observed_state_extent, 0u);
    EXPECT_EQ(background->observed_background_extent, 0u);

    stateful.compiled_graph->root_operations.tick_block(
        stateful_storage.buffer().data(), &empty_tick_invocation, index(73), 64);
    EXPECT_EQ(state->tick_calls, 2u);
    EXPECT_EQ(state->skip_calls, 0u);
    EXPECT_EQ(state->last_index, 73u);
    EXPECT_EQ(state->last_block_size, 64u);
    EXPECT_EQ(background->tick_calls, 0u);
    EXPECT_EQ(background->skip_calls, 0u);
    EXPECT_EQ(background->last_index, 0u);
    EXPECT_EQ(background->last_block_size, 0u);

    auto state_only = compile(graph_jit_state_only_module_id, 101);
    ASSERT_TRUE(state_only.succeeded())
        << (state_only.diagnostics.empty() ? "" : state_only.diagnostics.front().message);
    expect_single_node_canonical_regions(
        state_only.compiled_graph->node_layout,
        state_only.compiled_graph->tock_state_layout,
        sizeof(SingleSpanProbeMirror),
        0);
    ASSERT_EQ(state_only.compiled_graph->node_layout.nodes.size(), 1u);
    auto state_only_storage = state_only.compiled_graph->node_layout.create_storage(resources);
    state_only_storage.initialize();
    auto* state_only_value = static_cast<SingleSpanProbeMirror*>(
        state_only_storage.state_ptr(0));
    state_only.compiled_graph->root_operations.tick_block(
        state_only_storage.buffer().data(), &empty_tick_invocation, index(3), 8);
    ASSERT_NE(state_only_value, nullptr);
    EXPECT_EQ(state_only_value->calls, 1u);
    EXPECT_EQ(state_only_value->observed_state_extent, sizeof(SingleSpanProbeMirror));
    EXPECT_EQ(state_only_value->observed_background_extent, 0u);

    auto background_only = compile(graph_jit_background_only_module_id, 102);
    ASSERT_TRUE(background_only.succeeded())
        << (background_only.diagnostics.empty() ? "" : background_only.diagnostics.front().message);
    expect_single_node_canonical_regions(
        background_only.compiled_graph->node_layout,
        background_only.compiled_graph->tock_state_layout,
        0,
        sizeof(SingleSpanProbeMirror));
    ASSERT_EQ(background_only.compiled_graph->node_layout.nodes.size(), 1u);
    auto background_only_storage =
        background_only.compiled_graph->node_layout.create_storage(resources);
    background_only_storage.initialize();
    auto background_only_tock_states =
        background_only.compiled_graph->tock_state_layout.create_storage(resources);
    background_only_tock_states.initialize();
    auto* background_only_value = static_cast<SingleSpanProbeMirror*>(
        background_only_tock_states.state_ptr(0));
    background_only.compiled_graph->root_operations.tick_block(
        background_only_storage.buffer().data(), &empty_tick_invocation, index(5), 16);
    ASSERT_NE(background_only_value, nullptr);
    EXPECT_EQ(background_only_value->calls, 0u);
    EXPECT_EQ(background_only_value->observed_state_extent, 0u);
    EXPECT_EQ(background_only_value->observed_background_extent, 0u);

    auto stateless = compile(graph_jit_stateless_module_id, 103);
    ASSERT_TRUE(stateless.succeeded())
        << (stateless.diagnostics.empty() ? "" : stateless.diagnostics.front().message);
    expect_single_node_canonical_regions(
        stateless.compiled_graph->node_layout,
        stateless.compiled_graph->tock_state_layout,
        0,
        0);
    EXPECT_EQ(stateless.compiled_graph->node_layout.storage_size, 0u);
    EXPECT_NO_THROW(stateless.compiled_graph->root_operations.tick_block(nullptr, &empty_tick_invocation, index(9), 32));

}

TEST_F(
    GraphJitRuntimeFixture,
    DeclarationOwnedStorageAndNestedNodesPreservePrimitiveStateMapping)
{
    auto compiled = compile(graph_jit_declaration_layout_module_id, 105);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->root_operations.valid());

    auto const& layout = compiled.compiled_graph->node_layout;
    auto const& tock_layout = compiled.compiled_graph->tock_state_layout;
    ASSERT_EQ(layout.nodes.size(), 5u);
    ASSERT_EQ(tock_layout.nodes.size(), 5u);

    auto storage = layout.create_storage(resources);
    storage.initialize();
    auto tock_states = tock_layout.create_storage(resources);
    tock_states.initialize();

    auto* parent = static_cast<DeclarationLayoutParentStateMirror*>(
        storage.state_ptr(0));
    auto* producer = static_cast<DeclarationLayoutProducerStateMirror*>(
        storage.state_ptr(3));
    auto* consumer = static_cast<DeclarationLayoutConsumerStateMirror*>(
        storage.state_ptr(4));
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(producer, nullptr);
    ASSERT_NE(consumer, nullptr);
    ASSERT_EQ(parent->local.size(), 3u);
    ASSERT_EQ(parent->nested.size(), 2u);
    ASSERT_EQ(producer->values.size(), 3u);
    ASSERT_EQ(consumer->imported.size(), 3u);
    EXPECT_EQ(consumer->imported.data(), producer->values.data());
    EXPECT_EQ(consumer->initialize_sum, 41u);

    auto const* first_leaf =
        reinterpret_cast<DeclarationLayoutLeafStateMirror const*>(
            parent->nested[0].data());
    auto const* second_leaf =
        reinterpret_cast<DeclarationLayoutLeafStateMirror const*>(
            parent->nested[1].data());
    ASSERT_NE(first_leaf, nullptr);
    ASSERT_NE(second_leaf, nullptr);
    EXPECT_EQ(first_leaf->marker, 17u);
    EXPECT_EQ(second_leaf->marker, 17u);

    auto const* first_tock_leaf =
        static_cast<DeclarationLayoutLeafTockStateMirror const*>(
            tock_states.state_ptr(1));
    auto const* second_tock_leaf =
        static_cast<DeclarationLayoutLeafTockStateMirror const*>(
            tock_states.state_ptr(2));
    ASSERT_NE(first_tock_leaf, nullptr);
    ASSERT_NE(second_tock_leaf, nullptr);
    EXPECT_EQ(first_tock_leaf->marker, 23u);
    EXPECT_EQ(second_tock_leaf->marker, 23u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(7), 16);
    EXPECT_EQ(parent->calls, 1u);
    EXPECT_EQ(parent->local_sum, 10u);
    EXPECT_EQ(parent->nested_sum, 34u);
    EXPECT_EQ(producer->calls, 1u);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_EQ(consumer->tick_sum, 41u);
}

TEST_F(GraphJitRuntimeFixture, GeneratedBackgroundRootsUseSuppliedBatchBindings)
{
    auto compiled = compile(graph_jit_background_execution_module_id, 104);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->background_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->background_evaluation_plan.nodes.size(), 1u);

    auto tock_states =
        compiled.compiled_graph->tock_state_layout.create_storage(resources);
    tock_states.initialize();
    auto* state = static_cast<BackgroundExecutionProbeStateMirror*>(
        tock_states.state_ptr(0));
    ASSERT_NE(state, nullptr);
    BackgroundOperationHookCapture operation_hooks{.state = state};

    BackgroundExecutionCapture capture;
    iv::IndexCoverage const empty;
    std::array output_changes{iv::OutputCoverageChange{
        .data = &capture,
        .previous_coverage_value = &empty,
        .publish_regions_value = &capture_background_regions,
        .publish_changed_value = &capture_background_change,
    }};
    std::array node_frames{iv::graph_jit::BackgroundNodeCall{
        .activity = iv::graph_jit::BackgroundNodeActivity::forward,
        .operation_frame = &operation_hooks,
        .prepare_operations = &prepare_background_operations,
        .finalize_operations = &finalize_background_operations,
        .forward = iv::ReflectedNodeForwardCoverageContext{
            .outputs = output_changes,
            .local_state_changed = true,
            .sample_rate = 88200,
        },
    }};
    iv::graph_jit::BackgroundEvaluationCall batch{.nodes = node_frames};

    compiled.compiled_graph->background_operations.propagate_forward(
        &batch);
    EXPECT_EQ(capture.coverage, index_coverage({{10, 13}}));
    EXPECT_EQ(capture.changed, capture.coverage);
    EXPECT_EQ(operation_hooks.prepare_calls, 0u);
    EXPECT_EQ(operation_hooks.finalize_calls, 0u);

    auto const requested = index_coverage({{10, 13}});
    auto const sample_geometry = *iv::continuous_value_storage_geometry(
        iv::continuous_value_type_id_v<iv::Sample>, std::nullopt);
    std::array<iv::Sample, 3> output_values{};
    std::array output_regions{iv::ContinuousRegionStorageView{
        .storage = reinterpret_cast<std::byte*>(output_values.data()),
        .range = iv::GlobalIndexRange{
            .begin = iv::GlobalIndex::from_bits(10),
            .extent = output_values.size(),
        },
        .geometry = sample_geometry,
        .channel_layout = iv::mono_planar_channel_layout,
    }};
    std::array outputs{iv::TockContinuousOutputPort{
        .regions = output_regions.data(),
        .region_count = output_regions.size(),
        .requested_coverage_value = &requested,
        .geometry = sample_geometry,
    }};
    node_frames[0].activity = iv::graph_jit::BackgroundNodeActivity::evaluate;
    node_frames[0].tock = iv::ReflectedNodeTockCoverageContext{
        .outputs = outputs,
        .background_state_storage = {
            static_cast<std::byte*>(tock_states.state_ptr(0)),
            sizeof(BackgroundExecutionProbeStateMirror),
        },
        .sample_rate = 88200,
    };
    compiled.compiled_graph->background_operations.evaluate(
        &batch);

    EXPECT_EQ(operation_hooks.prepare_calls, 1u);
    EXPECT_EQ(operation_hooks.finalize_calls, 1u);
    EXPECT_EQ(operation_hooks.calls_seen_by_prepare, 0u);
    EXPECT_EQ(operation_hooks.calls_seen_by_finalize, 1u);
    EXPECT_EQ(state->calls, 1u);
    EXPECT_EQ(state->sample_rate, 88200u);
    EXPECT_FLOAT_EQ(output_values[0].value, 10.0f);
    EXPECT_FLOAT_EQ(output_values[1].value, 11.0f);
    EXPECT_FLOAT_EQ(output_values[2].value, 12.0f);

    auto const& plan = compiled.compiled_graph->background_evaluation_plan;
    iv::BackgroundCoverageState coverage(
        plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace workspace(
        plan, compiled.compiled_graph->specialization.sample_rate);
    auto prepared = workspace.prepare(
        compiled.compiled_graph->background_operations,
        coverage,
        iv::CoveragePropagationRequest{
            .locally_changed_nodes = {0},
            .output_demands = {
                iv::OutputCoverageRequest{
                    .port = 0,
                    .required = index_coverage({{10, 13}}),
                },
            },
        });
    auto const& propagated = prepared.result();
    ASSERT_EQ(propagated.output_changes.size(), 1u);
    EXPECT_EQ(propagated.output_changes[0].port, 0u);
    EXPECT_EQ(propagated.output_changes[0].coverage, requested);
    EXPECT_EQ(propagated.output_changes[0].changed, requested);
    ASSERT_EQ(propagated.output_requirements.size(), 1u);
    EXPECT_EQ(propagated.output_requirements[0].port, 0u);
    EXPECT_EQ(propagated.output_requirements[0].required, requested);
    workspace.discard(std::move(prepared));
}

TEST_F(GraphJitRuntimeFixture, NativeBackgroundCallbacksReceiveReadyFrontierBatch)
{
    auto compiled = compile(graph_jit_native_batch_background_module_id, 105);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->background_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->background_evaluation_plan.nodes.size(), 2u);
    ASSERT_EQ(compiled.compiled_graph->tock_state_layout.nodes.size(), 2u);
    ASSERT_EQ(
        compiled.compiled_graph->background_evaluation_plan
            .forward_ready_groups.size(),
        1u);
    EXPECT_EQ(
        compiled.compiled_graph->background_evaluation_plan
            .forward_ready_groups.front().size(),
        2u);
    ASSERT_EQ(
        compiled.compiled_graph->background_evaluation_plan
            .evaluation_ready_groups.size(),
        1u);
    EXPECT_EQ(
        compiled.compiled_graph->background_evaluation_plan
            .evaluation_ready_groups.front().size(),
        2u);

    auto tock_states =
        compiled.compiled_graph->tock_state_layout.create_storage(resources);
    tock_states.initialize();
    auto* first_state = static_cast<NativeBatchBackgroundProbeStateMirror*>(
        tock_states.state_ptr(0));
    auto* second_state = static_cast<NativeBatchBackgroundProbeStateMirror*>(
        tock_states.state_ptr(1));
    ASSERT_NE(first_state, nullptr);
    ASSERT_NE(second_state, nullptr);

    BackgroundExecutionCapture first_capture;
    BackgroundExecutionCapture second_capture;
    iv::IndexCoverage const empty;
    std::array first_changes{iv::OutputCoverageChange{
        .data = &first_capture,
        .previous_coverage_value = &empty,
        .publish_regions_value = &capture_background_regions,
        .publish_changed_value = &capture_background_change,
    }};
    std::array second_changes{iv::OutputCoverageChange{
        .data = &second_capture,
        .previous_coverage_value = &empty,
        .publish_regions_value = &capture_background_regions,
        .publish_changed_value = &capture_background_change,
    }};
    std::array node_frames{
        iv::graph_jit::BackgroundNodeCall{
            .activity = iv::graph_jit::BackgroundNodeActivity::forward,
            .forward = iv::ReflectedNodeForwardCoverageContext{
                .outputs = first_changes,
                .sample_rate = 88200,
            },
        },
        iv::graph_jit::BackgroundNodeCall{
            .activity = iv::graph_jit::BackgroundNodeActivity::forward,
            .forward = iv::ReflectedNodeForwardCoverageContext{
                .outputs = second_changes,
                .sample_rate = 88200,
            },
        },
    };
    iv::graph_jit::BackgroundEvaluationCall batch{.nodes = node_frames};

    compiled.compiled_graph->background_operations.propagate_forward(&batch);
    EXPECT_EQ(first_capture.coverage, index_coverage({{11, 13}}));
    EXPECT_EQ(first_capture.changed, first_capture.coverage);
    EXPECT_EQ(second_capture.coverage, index_coverage({{22, 24}}));
    EXPECT_EQ(second_capture.changed, second_capture.coverage);

    auto const no_op = +[](void*) {};
    node_frames[0].activity = iv::graph_jit::BackgroundNodeActivity::evaluate;
    node_frames[0].operation_frame = nullptr;
    node_frames[0].prepare_operations = no_op;
    node_frames[0].finalize_operations = no_op;
    node_frames[0].tock = iv::ReflectedNodeTockCoverageContext{
        .background_state_storage = {
            static_cast<std::byte*>(tock_states.state_ptr(0)),
            sizeof(NativeBatchBackgroundProbeStateMirror),
        },
        .sample_rate = 88200,
    };
    node_frames[1].activity = iv::graph_jit::BackgroundNodeActivity::evaluate;
    node_frames[1].operation_frame = nullptr;
    node_frames[1].prepare_operations = no_op;
    node_frames[1].finalize_operations = no_op;
    node_frames[1].tock = iv::ReflectedNodeTockCoverageContext{
        .background_state_storage = {
            static_cast<std::byte*>(tock_states.state_ptr(1)),
            sizeof(NativeBatchBackgroundProbeStateMirror),
        },
        .sample_rate = 88200,
    };

    compiled.compiled_graph->background_operations.evaluate(&batch);
    EXPECT_EQ(first_state->tock_batch_calls, 1u);
    EXPECT_EQ(first_state->last_lane_count, 2u);
    EXPECT_EQ(first_state->configured_id, 11u);
    EXPECT_EQ(first_state->sample_rate, 88200u);
    EXPECT_EQ(second_state->tock_batch_calls, 1u);
    EXPECT_EQ(second_state->last_lane_count, 2u);
    EXPECT_EQ(second_state->configured_id, 22u);
    EXPECT_EQ(second_state->sample_rate, 88200u);
}

// Reverse propagation needs a Random Access input. This connected module also
// exercises the background-only sample delivery used by that input.
TEST_F(GraphJitRuntimeFixture, NativeReversePropagationReceivesReadyFrontierBatch)
{
    auto compiled = compile(graph_jit_native_batch_reverse_module_id, 106);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->background_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->background_evaluation_plan.nodes.size(), 4u);
    ASSERT_EQ(
        compiled.compiled_graph->background_evaluation_plan
            .reverse_ready_groups.size(),
        2u);
    EXPECT_EQ(
        compiled.compiled_graph->background_evaluation_plan
            .reverse_ready_groups.front().size(),
        2u);

    iv::IndexCoverage const empty;
    iv::IndexCoverage first_requirement;
    iv::IndexCoverage second_requirement;
    auto const capture_requirement = +[](
        void* data,
        iv::IndexCoverage const& required) {
        *static_cast<iv::IndexCoverage*>(data) = required;
    };
    std::array first_inputs{iv::InputCoverageRequirement{
        .data = &first_requirement,
        .coverage_value = &empty,
        .publish_required_value = capture_requirement,
    }};
    std::array second_inputs{iv::InputCoverageRequirement{
        .data = &second_requirement,
        .coverage_value = &empty,
        .publish_required_value = capture_requirement,
    }};
    std::array node_frames{
        iv::graph_jit::BackgroundNodeCall{},
        iv::graph_jit::BackgroundNodeCall{},
        iv::graph_jit::BackgroundNodeCall{
            .activity = iv::graph_jit::BackgroundNodeActivity::reverse,
            .reverse = iv::ReflectedNodeReverseCoverageContext{
                .inputs = first_inputs,
                .sample_rate = 88200,
            },
        },
        iv::graph_jit::BackgroundNodeCall{
            .activity = iv::graph_jit::BackgroundNodeActivity::reverse,
            .reverse = iv::ReflectedNodeReverseCoverageContext{
                .inputs = second_inputs,
                .sample_rate = 88200,
            },
        },
    };
    iv::graph_jit::BackgroundEvaluationCall batch{.nodes = node_frames};

    compiled.compiled_graph->background_operations.propagate_reverse(&batch);
    EXPECT_EQ(first_requirement, index_coverage({{11, 13}}));
    EXPECT_EQ(second_requirement, index_coverage({{22, 24}}));
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
        configured.compiled_graph->tock_state_layout,
        sizeof(ConfiguredProbeStateMirror),
        0);
    auto configured_storage = configured.compiled_graph->node_layout.create_storage(resources);
    configured_storage.initialize();
    auto* configured_state = static_cast<ConfiguredProbeStateMirror*>(
        configured_storage.state_ptr(0));
    ASSERT_NE(configured_state, nullptr);
    configured.compiled_graph->root_operations.tick_block(
        configured_storage.buffer().data(), &empty_tick_invocation, index(11), 32);
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
        pointer_configured.compiled_graph->tock_state_layout,
        sizeof(PointerConfiguredProbeStateMirror),
        0);
    auto pointer_storage =
        pointer_configured.compiled_graph->node_layout.create_storage(resources);
    pointer_storage.initialize();
    auto* pointer_state = static_cast<PointerConfiguredProbeStateMirror*>(
        pointer_storage.state_ptr(0));
    ASSERT_NE(pointer_state, nullptr);
    pointer_configured.compiled_graph->root_operations.tick_block(
        pointer_storage.buffer().data(), &empty_tick_invocation, index(13), 16);
    EXPECT_EQ(pointer_state->calls, 1u);
    EXPECT_EQ(pointer_state->first_value, 0x2222222222222222ull);
    EXPECT_EQ(pointer_state->second_value, 0x4444444444444444ull);
    EXPECT_EQ(pointer_state->null_seen, 1u);
    EXPECT_EQ(pointer_state->marker, 0x89abcdefu);
    EXPECT_EQ(pointer_state->tag, 0x4567u);

}

TEST_F(GraphJitRuntimeFixture, MultipleNodesAndBlockSlicing)
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
    ASSERT_EQ(multiple.compiled_graph->node_layout.nodes.size(), 2u);
    EXPECT_EQ(
        multiple.compiled_graph->node_layout.nodes[0].state_size,
        sizeof(StatefulProbeStateMirror));
    EXPECT_EQ(
        multiple.compiled_graph->tock_state_layout.nodes[0].state_size,
        sizeof(StatefulProbeBackgroundStateMirror));
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
        multiple_storage.buffer().data(), &empty_tick_invocation, index(19), 32);
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
        skippable_pair_storage.buffer().data(), &empty_tick_invocation, index(23), 32);
    EXPECT_EQ(pair_state_0->tick_calls, 1u);
    EXPECT_EQ(pair_state_1->tick_calls, 1u);
    auto limited = compile(graph_jit_limited_block_module_id, 108);
    ASSERT_TRUE(limited.succeeded())
        << (limited.diagnostics.empty() ? "" : limited.diagnostics.front().message);
    expect_single_node_canonical_regions(
        limited.compiled_graph->node_layout,
        limited.compiled_graph->tock_state_layout,
        sizeof(LimitedBlockProbeStateMirror),
        0);
    auto limited_storage = limited.compiled_graph->node_layout.create_storage(resources);
    limited_storage.initialize();
    auto* limited_state = static_cast<LimitedBlockProbeStateMirror*>(
        limited_storage.state_ptr(0));
    ASSERT_NE(limited_state, nullptr);

    limited.compiled_graph->root_operations.tick_block(
        limited_storage.buffer().data(), &empty_tick_invocation, index(200), 64);
    EXPECT_EQ(limited_state->tick_calls, 4u);
    EXPECT_EQ(
        limited_state->tick_indices,
        (std::array<std::uint64_t, 8>{200, 216, 232, 248, 0, 0, 0, 0}));
    EXPECT_EQ(
        limited_state->tick_sizes,
        (std::array<std::uint64_t, 8>{16, 16, 16, 16, 0, 0, 0, 0}));

    limited.compiled_graph->root_operations.tick_block(
        limited_storage.buffer().data(), &empty_tick_invocation, index(300), 8);
    EXPECT_EQ(limited_state->tick_calls, 5u);
    EXPECT_EQ(limited_state->tick_indices[4], 300u);
    EXPECT_EQ(limited_state->tick_sizes[4], 8u);

}

TEST_F(GraphJitRuntimeFixture, NativeTickCallbacksReceiveReadyFrontierBatch)
{
    auto compiled = compile(graph_jit_native_batch_tick_module_id, 110);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->root_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* first = static_cast<NativeBatchTickProbeStateMirror*>(
        storage.state_ptr(0));
    auto* second = static_cast<NativeBatchTickProbeStateMirror*>(
        storage.state_ptr(1));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(41), 32);
    EXPECT_EQ(first->tick_batch_calls, 1u);
    EXPECT_EQ(first->skip_batch_calls, 0u);
    EXPECT_EQ(first->last_lane_count, 2u);
    EXPECT_EQ(first->configured_id, 11u);
    EXPECT_EQ(first->last_index, 41u);
    EXPECT_EQ(first->last_block_size, 32u);
    EXPECT_EQ(second->tick_batch_calls, 1u);
    EXPECT_EQ(second->skip_batch_calls, 0u);
    EXPECT_EQ(second->last_lane_count, 2u);
    EXPECT_EQ(second->configured_id, 22u);

}

TEST_F(GraphJitRuntimeFixture, NativeTickBatchScalarizesAliasedCallbackStorage)
{
    auto compiled = compile(graph_jit_native_batch_aliasing_module_id, 111);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->root_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* first = static_cast<NativeBatchAliasingProbeStateMirror*>(
        storage.state_ptr(0));
    auto* second = static_cast<NativeBatchAliasingProbeStateMirror*>(
        storage.state_ptr(1));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(41), 32);
    EXPECT_EQ(first->tick_batch_calls, 1u);
    EXPECT_EQ(first->last_lane_count, 1u);
    EXPECT_EQ(first->configured_id, 11u);
    EXPECT_EQ(second->tick_batch_calls, 1u);
    EXPECT_EQ(second->last_lane_count, 1u);
    EXPECT_EQ(second->configured_id, 22u);
}

TEST_F(GraphJitRuntimeFixture, ActivityPartitionsReadyFrontierIntoTickAndSkipBatches)
{
    auto compiled = compile(graph_jit_native_batch_activity_module_id, 113);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->root_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 3u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* short_ttl = static_cast<NativeBatchActivityProbeStateMirror*>(
        storage.state_ptr(0));
    auto* long_ttl = static_cast<NativeBatchActivityProbeStateMirror*>(
        storage.state_ptr(1));
    ASSERT_NE(short_ttl, nullptr);
    ASSERT_NE(long_ttl, nullptr);

    // Without inputs, each lane becomes dormant once its cleared outputs have
    // been quiet for its TTL. Block 64 splits one ready group into a one-lane
    // Tick batch and a one-lane skip batch. Blocks 128 and 192 skip both lanes
    // in one batch.
    for (auto const begin : {0u, 64u, 128u, 192u}) {
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(),
            &empty_tick_invocation,
            index(begin),
            64);
    }
    EXPECT_EQ(short_ttl->tick_batch_calls, 1u);
    EXPECT_EQ(short_ttl->skip_batch_calls, 3u);
    EXPECT_EQ(short_ttl->tick_lane_total, 2u);
    EXPECT_EQ(short_ttl->skip_lane_total, 5u);
    EXPECT_EQ(long_ttl->tick_batch_calls, 2u);
    EXPECT_EQ(long_ttl->skip_batch_calls, 2u);
    EXPECT_EQ(long_ttl->tick_lane_total, 3u);
    EXPECT_EQ(long_ttl->skip_lane_total, 4u);
}

TEST_F(GraphJitRuntimeFixture, BatchOnlyTickCallbackExecutesThroughReplayScalarAdapter)
{
    auto compiled = compile(graph_jit_native_batch_replay_module_id, 112);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_TRUE(compiled.compiled_graph->background_operations.valid());

    auto const& plan = compiled.compiled_graph->background_evaluation_plan;
    ASSERT_EQ(plan.runtime.replay_invocations.size(), 1u);
    auto const replay_node = plan.runtime.replay_invocations.front().node;
    ASSERT_LT(replay_node, plan.nodes.size());

    std::array<iv::Sample, 8> samples{};
    std::array output_bindings{iv::ReflectedContinuousOutputPortBinding{
        .storage = sample_storage_binding(samples),
    }};
    std::array replay_regions{iv::GlobalIndexRange{
        .begin = index(40),
        .extent = 3,
    }};
    auto const no_op = +[](void*) {};
    std::vector<iv::graph_jit::BackgroundNodeCall> node_frames(
        plan.nodes.size());
    node_frames[replay_node] = iv::graph_jit::BackgroundNodeCall{
        .activity = iv::graph_jit::BackgroundNodeActivity::evaluate,
        .prepare_operations = no_op,
        .finalize_operations = no_op,
        .replay = iv::ReflectedNodeTickContext{
            .continuous_output_bindings = output_bindings,
            .sample_rate = 88200,
        },
        .replay_regions = replay_regions,
    };
    iv::graph_jit::BackgroundEvaluationCall batch{.nodes = node_frames};

    compiled.compiled_graph->background_operations.evaluate(&batch);

    EXPECT_EQ(samples, (std::array<iv::Sample, 8>{
        51.0f, 52.0f, 53.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}));
}

TEST_F(GraphJitRuntimeFixture, ActivityTtlIsOptInAndWakesOnInputChange)
{
    auto enabled = compile(graph_jit_activity_module_id, 120);
    ASSERT_TRUE(enabled.succeeded())
        << (enabled.diagnostics.empty()
                ? ""
                : enabled.diagnostics.front().message);
    ASSERT_EQ(enabled.compiled_graph->node_layout.nodes.size(), 3u);
    std::vector<std::size_t> activity_region_sizes;
    for (auto const& region : enabled.compiled_graph->node_layout.regions) {
        if (region.kind == iv::NodeLayout::Region::Kind::raw
            && region.migration_identity.starts_with(
                "graphjit.activity:")) {
            activity_region_sizes.push_back(region.size);
        }
    }
    ASSERT_EQ(activity_region_sizes.size(), 2u);
    EXPECT_EQ(
        std::ranges::count(
            activity_region_sizes,
            sizeof(iv::graph_jit::detail::PrimitiveActivityState)),
        1u);
    EXPECT_EQ(
        std::ranges::count_if(
            activity_region_sizes,
            [](std::size_t size) {
                return size
                    > sizeof(iv::graph_jit::detail::PrimitiveActivityState);
            }),
        1u);
    auto enabled_storage =
        enabled.compiled_graph->node_layout.create_storage(resources);
    enabled_storage.initialize();
    auto* first_enabled_state = static_cast<ActivityProbeStateMirror*>(
        enabled_storage.state_ptr(1));
    auto* second_enabled_state = static_cast<ActivityProbeStateMirror*>(
        enabled_storage.state_ptr(2));
    ASSERT_NE(first_enabled_state, nullptr);
    ASSERT_NE(second_enabled_state, nullptr);

    for (auto const begin : {0u, 64u, 128u, 192u, 256u}) {
        enabled.compiled_graph->root_operations.tick_block(
            enabled_storage.buffer().data(),
            &empty_tick_invocation,
            index(begin),
            64);
    }
    EXPECT_EQ(first_enabled_state->tick_calls, 4u);
    EXPECT_EQ(first_enabled_state->skip_calls, 1u);
    EXPECT_EQ(second_enabled_state->tick_calls, 4u);
    EXPECT_EQ(second_enabled_state->skip_calls, 1u);

    auto disabled = compile(graph_jit_activity_disabled_module_id, 121);
    ASSERT_TRUE(disabled.succeeded())
        << (disabled.diagnostics.empty()
                ? ""
                : disabled.diagnostics.front().message);
    ASSERT_EQ(disabled.compiled_graph->node_layout.nodes.size(), 2u);
    auto disabled_storage =
        disabled.compiled_graph->node_layout.create_storage(resources);
    disabled_storage.initialize();
    auto* disabled_state = static_cast<ActivityProbeStateMirror*>(
        disabled_storage.state_ptr(1));
    ASSERT_NE(disabled_state, nullptr);

    disabled.compiled_graph->root_operations.tick_block(
        disabled_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        64);
    disabled.compiled_graph->root_operations.tick_block(
        disabled_storage.buffer().data(),
        &empty_tick_invocation,
        index(64),
        64);
    EXPECT_EQ(disabled_state->tick_calls, 2u);
    EXPECT_EQ(disabled_state->skip_calls, 0u);

    auto type_default = compile(graph_jit_activity_type_default_module_id, 122);
    ASSERT_TRUE(type_default.succeeded())
        << (type_default.diagnostics.empty()
                ? ""
                : type_default.diagnostics.front().message);
    auto type_default_storage =
        type_default.compiled_graph->node_layout.create_storage(resources);
    type_default_storage.initialize();
    auto* type_default_state = static_cast<ActivityProbeStateMirror*>(
        type_default_storage.state_ptr(1));
    ASSERT_NE(type_default_state, nullptr);
    type_default.compiled_graph->root_operations.tick_block(
        type_default_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        64);
    type_default.compiled_graph->root_operations.tick_block(
        type_default_storage.buffer().data(),
        &empty_tick_invocation,
        index(64),
        64);
    EXPECT_EQ(type_default_state->tick_calls, 1u);
    EXPECT_EQ(type_default_state->skip_calls, 1u);
}

TEST_F(GraphJitRuntimeFixture,
       CompilerOwnedActivityStateMigratesAcrossCompatibleGenerations)
{
    auto predecessor = compile(
        graph_jit_activity_type_default_module_id, 256);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);
    auto predecessor_storage = predecessor.compiled_graph
        ->node_layout.create_storage(resources);
    predecessor_storage.initialize();
    auto* predecessor_state = static_cast<ActivityProbeStateMirror*>(
        predecessor_storage.state_ptr(1));
    ASSERT_NE(predecessor_state, nullptr);

    predecessor.compiled_graph->root_operations.tick_block(
        predecessor_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        64);
    EXPECT_EQ(predecessor_state->tick_calls, 1u);
    EXPECT_EQ(predecessor_state->skip_calls, 0u);

    auto successor = compile(
        graph_jit_activity_type_default_module_id, 257);
    ASSERT_TRUE(successor.succeeded())
        << (successor.diagnostics.empty()
                ? ""
                : successor.diagnostics.front().message);
    auto successor_storage = successor.compiled_graph
        ->node_layout.create_storage(resources);
    successor_storage.initialize(&predecessor_storage);
    auto* successor_state = static_cast<ActivityProbeStateMirror*>(
        successor_storage.state_ptr(1));
    ASSERT_NE(successor_state, nullptr);

    successor.compiled_graph->root_operations.tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(64),
        64);
    // Authored State starts fresh in this fixture. The compiler-owned activity
    // region migrates independently, so the successor immediately skips.
    EXPECT_EQ(successor_state->tick_calls, 0u);
    EXPECT_EQ(successor_state->skip_calls, 1u);
}

TEST_F(GraphJitRuntimeFixture, EventActivityWakesOnANonemptyInputWindow)
{
    auto compiled = compile(graph_jit_event_activity_module_id, 258);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<ActivityProbeStateMirror*>(
        storage.state_ptr(1));
    ASSERT_NE(state, nullptr);

    for (auto const begin : {0u, 64u, 128u}) {
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(),
            &empty_tick_invocation,
            index(begin),
            64);
    }
    EXPECT_EQ(state->tick_calls, 2u);
    EXPECT_EQ(state->skip_calls, 1u);
}

TEST_F(GraphJitRuntimeFixture, DisconnectedSampleInputUsesDeclaredDefault)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_disconnected_sample_input_module_id);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    EXPECT_TRUE(analysis->sample_connections.empty());
    EXPECT_TRUE(analysis->sample_producer_groups.empty());

    auto compiled = compile_graph(graph, 121);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    EXPECT_EQ(count_raw_regions(compiled.compiled_graph->node_layout), 1u);

    auto const& requirements =
        compiled.compiled_graph->realtime_port_state_requirements.states;
    auto const& realizations =
        compiled.compiled_graph->realtime_port_state_realizations.sample_states;
    ASSERT_EQ(requirements.size(), 2u);
    ASSERT_EQ(realizations.size(), 2u);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        auto const found = std::ranges::find_if(
            realizations,
            [&](iv::graph_jit::SampleRealtimePortStateRealization const& view) {
                return view.requirement_index < requirements.size()
                    && requirements[view.requirement_index].channel == channel;
            });
        ASSERT_NE(found, realizations.end());
        EXPECT_EQ(
            found->retained_access,
            iv::graph_jit::SampleRealtimePortStateAccess::immutable_constant);
        EXPECT_EQ(
            found->authored_storage.storage,
            iv::graph_jit::SampleRealtimePortStateStorage::immutable_constant);
        EXPECT_EQ(found->authored_storage.representation_channel, channel);
        EXPECT_EQ(
            found->authored_storage.channel_layout.channel_type,
            iv::ChannelTypeId::stereo);
        EXPECT_EQ(
            found->authored_storage.channel_layout.sample_layout,
            iv::SampleStreamLayout::interleaved);
        EXPECT_EQ(found->authored_storage.timeline_offset_frames, 0);
        ASSERT_TRUE(found->authored_storage.constant_value);
        EXPECT_FLOAT_EQ(
            static_cast<float>(sample_value_from_bytes(
                *found->authored_storage.constant_value)),
            0.375f);
        EXPECT_FALSE(found->authored_storage.node_storage_offset);
        EXPECT_FALSE(found->authored_storage.callback_arena_offset);
        EXPECT_TRUE(found->retained_sources.empty());
        EXPECT_FALSE(found->materialization);
    }

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<DisconnectedSampleInputProbeStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(100), 64);
    EXPECT_EQ(state->calls, 1u);
    EXPECT_EQ(state->last_index, 100u);
    EXPECT_EQ(state->last_block_size, 64u);
    EXPECT_FLOAT_EQ(state->current_left, 0.375f);
    EXPECT_FLOAT_EQ(state->current_right, 0.375f);
    EXPECT_FLOAT_EQ(state->history_left, 0.375f);
    EXPECT_FLOAT_EQ(state->history_right, 0.375f);
    EXPECT_FLOAT_EQ(state->block_first, 0.375f);
    EXPECT_FLOAT_EQ(state->block_last, 0.375f);
    EXPECT_FLOAT_EQ(state->block_sum, 24.0f);
    EXPECT_EQ(state->marker, 0xd15c0a11u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(164), 16);
    EXPECT_EQ(state->calls, 2u);
    EXPECT_EQ(state->last_index, 164u);
    EXPECT_EQ(state->last_block_size, 16u);
    EXPECT_FLOAT_EQ(state->history_left, 0.375f);
    EXPECT_FLOAT_EQ(state->history_right, 0.375f);
    EXPECT_FLOAT_EQ(state->block_sum, 6.0f);
}

TEST_F(GraphJitRuntimeFixture, DisconnectedSampleOutputKeepsDeclaredHistory)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_disconnected_sample_output_module_id);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    EXPECT_TRUE(analysis->sample_connections.empty());
    EXPECT_TRUE(analysis->sample_producer_groups.empty());

    auto compiled = compile_graph(graph, 122);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(count_raw_regions(compiled.compiled_graph->node_layout), 2u);
    auto const raw = std::ranges::find_if(
        compiled.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw;
        });
    ASSERT_NE(raw, compiled.compiled_graph->node_layout.regions.end());
    EXPECT_EQ(raw->size, 5u * sizeof(iv::Sample));

    auto const& requirements =
        compiled.compiled_graph->realtime_port_state_requirements.states;
    auto const& realizations =
        compiled.compiled_graph->realtime_port_state_realizations.sample_states;
    ASSERT_EQ(requirements.size(), 2u);
    ASSERT_EQ(realizations.size(), 2u);
    for (auto const& view : realizations) {
        ASSERT_LT(view.requirement_index, requirements.size());
        auto const& requirement = requirements[view.requirement_index];
        EXPECT_EQ(requirement.direction, iv::graph_jit::PortDirection::output);
        EXPECT_TRUE(
            requirement.role
                == iv::graph_jit::RealtimePortStateRole::tick_output_history
            || requirement.role
                == iv::graph_jit::RealtimePortStateRole::tick_output_latency);
        EXPECT_EQ(
            view.authored_storage.storage,
            iv::graph_jit::SampleRealtimePortStateStorage::callback_transient);
        EXPECT_EQ(
            view.retained_access,
            iv::graph_jit::SampleRealtimePortStateAccess::direct);
        ASSERT_EQ(view.retained_sources.size(), 1u);
        auto const& retained = view.retained_sources.front();
        EXPECT_EQ(
            retained.storage,
            iv::graph_jit::SampleRealtimePortStateStorage::compact_carry);
        EXPECT_EQ(view.authored_storage.representation_channel, 0u);
        EXPECT_EQ(view.authored_storage.timeline_offset_frames, 2);
        ASSERT_TRUE(view.authored_storage.callback_arena_offset);
        EXPECT_FALSE(view.authored_storage.node_storage_offset);
        EXPECT_EQ(retained.representation_channel, 0u);
        EXPECT_EQ(retained.timeline_offset_frames, 0);
        EXPECT_EQ(retained.storage_frame_count, 5u);
        EXPECT_EQ(retained.carry_future_frames, 2u);
        EXPECT_EQ(retained.storage_size_bytes, 5u * sizeof(iv::Sample));
        ASSERT_TRUE(retained.node_storage_offset);
        EXPECT_EQ(*retained.node_storage_offset, raw->storage_offset);
        EXPECT_FALSE(retained.callback_arena_offset);
        EXPECT_FALSE(retained.constant_value);
        EXPECT_FALSE(view.materialization);
    }

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<HistoryRampSourceStateMirror*>(storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(state->calls, 1u);
    EXPECT_FLOAT_EQ(state->previous_output, 0.0f);
    EXPECT_EQ(state->marker, 0x91a2b3c4u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(state->calls, 2u);
    EXPECT_FLOAT_EQ(state->previous_output, 63.0f);
}

TEST_F(GraphJitRuntimeFixture, TransitionRootPreservesSampleOutputWindow)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_disconnected_sample_output_module_id);
    ASSERT_TRUE(graph);
    auto predecessor = compile_graph(graph, 242);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);
    EXPECT_EQ(
        std::ranges::count_if(
            predecessor.compiled_graph->realtime_port_state_requirements.states,
            [](auto const& requirement) {
                return requirement.direction
                        == iv::graph_jit::PortDirection::output
                    && requirement.stable_identity.has_value();
            }),
        2);

    iv::GraphJitCompileRequest successor_request{
        .project_generation = 243,
        .graph = graph,
        .definitions = definitions,
    };
    auto steady_successor = jit->compile(successor_request);
    ASSERT_TRUE(steady_successor.succeeded())
        << (steady_successor.diagnostics.empty()
                ? ""
                : steady_successor.diagnostics.front().message);
    auto specialized = jit->compile_transition(
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
    ASSERT_TRUE(specialized.compiled_graph->cutover);
    ASSERT_NE(
        specialized.compiled_graph->root_operations.transition_tick_block,
        nullptr);
    ASSERT_EQ(
        specialized.compiled_graph->cutover->state_transition
            .sample_expiry_cursors.size(),
        1u);
    EXPECT_EQ(
        specialized.compiled_graph->cutover->state_transition
            .sample_expiry_cursors.front().value_count,
        3u);
    ASSERT_EQ(
        specialized.compiled_graph->cutover->state_transition
            .sample_transfers.size(),
        2u);
    for (auto const& transfer : specialized.compiled_graph->cutover
             ->state_transition.sample_transfers) {
        ASSERT_EQ(transfer.source_storage.size(), 1u);
        EXPECT_EQ(
            transfer.source_storage.front().timeline_offset_frames,
            0);
        EXPECT_EQ(transfer.target_storage.timeline_offset_frames, 0);
    }

    auto predecessor_storage = predecessor.compiled_graph
        ->node_layout.create_storage(resources);
    predecessor_storage.initialize();
    predecessor.compiled_graph->root_operations.tick_block(
        predecessor_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        64);
    predecessor.compiled_graph->root_operations.tick_block(
        predecessor_storage.buffer().data(),
        &empty_tick_invocation,
        index(64),
        64);

    auto successor_storage = specialized.compiled_graph
        ->node_layout.create_storage(resources);
    successor_storage.initialize(&predecessor_storage);
    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        specialized.compiled_graph->cutover->state_transition,
        predecessor_storage.buffer(),
        successor_storage.buffer());
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared,
        predecessor_storage.buffer(),
        successor_storage.buffer(),
        index(128));

    auto* state = static_cast<HistoryRampSourceStateMirror*>(
        successor_storage.state_ptr(0));
    ASSERT_NE(state, nullptr);
    specialized.compiled_graph->root_operations.transition_tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(128),
        64);
    EXPECT_FLOAT_EQ(state->previous_output, 127.0f);
    specialized.compiled_graph->root_operations.tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(192),
        64);
    EXPECT_FLOAT_EQ(state->previous_output, 191.0f);
}

TEST_F(GraphJitRuntimeFixture, GeneratedRootInvokesPersistedTickCaptures)
{
    auto compiled = compile(graph_jit_tick_capture_module_id, 220);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& background =
        compiled.compiled_graph->background_evaluation_plan;
    ASSERT_EQ(background.tick_runtime.sample_captures.size(), 1u);
    ASSERT_EQ(background.tick_runtime.event_captures.size(), 1u);
    ASSERT_EQ(background.tick_runtime.nodes.size(), 1u);
    EXPECT_EQ(background.tick_runtime.nodes[0].sample_capture_begin, 0u);
    EXPECT_EQ(background.tick_runtime.nodes[0].sample_capture_count, 1u);
    EXPECT_EQ(background.tick_runtime.nodes[0].event_capture_begin, 0u);
    EXPECT_EQ(background.tick_runtime.nodes[0].event_capture_count, 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    GeneratedTickCaptureProbe probe{};
    std::array sample_captures{
        iv::graph_jit::TickSampleCaptureOperation{
            .context = &probe,
            .capture = &capture_generated_tick_samples,
        },
    };
    std::array event_captures{
        iv::graph_jit::TickEventCaptureOperation{
            .context = &probe,
            .capture = &capture_generated_tick_events,
        },
    };
    iv::graph_jit::TickInvocationCall invocation{
        .sample_captures = sample_captures,
        .event_captures = event_captures,
    };

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &invocation, index(64), 64);

    EXPECT_EQ(probe.sample_calls, 1u);
    EXPECT_EQ(probe.event_calls, 1u);
    EXPECT_EQ(probe.sample_index, index(64));
    EXPECT_EQ(probe.block_size, 64u);
    EXPECT_EQ(probe.sample_history, 2u);
    EXPECT_EQ(probe.sample_latency, 1u);
    EXPECT_FLOAT_EQ(probe.first_sample, 64.5f);
    EXPECT_FLOAT_EQ(probe.last_sample, 127.5f);
    EXPECT_EQ(probe.event_history, 2u);
    EXPECT_EQ(probe.event_latency, 1u);
    EXPECT_EQ(probe.event_count, 1u);
    EXPECT_EQ(probe.first_event_time, index(67));

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &invocation, index(1000), 4);
    EXPECT_EQ(probe.sample_calls, 2u);
    EXPECT_EQ(probe.event_calls, 2u);
    EXPECT_EQ(probe.sample_index, index(1000));
    EXPECT_EQ(probe.block_size, 4u);
    EXPECT_FLOAT_EQ(probe.first_sample, 1000.5f);
    EXPECT_FLOAT_EQ(probe.last_sample, 1003.5f);
    EXPECT_EQ(probe.event_count, 1u);
    EXPECT_EQ(probe.first_event_time, index(1003));
}

TEST_F(GraphJitRuntimeFixture, TypeOwnedFFTBlockStorageUsesProjectedPortExtents)
{
    auto compiled = compile(graph_jit_fft_value_module_id, 301);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    FFTValueConsumerStateMirror* state = nullptr;
    for (std::size_t node = 0;
         node < compiled.compiled_graph->node_layout.nodes.size(); ++node) {
        if (compiled.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(FFTValueConsumerStateMirror)) {
            ASSERT_EQ(state, nullptr);
            state = static_cast<FFTValueConsumerStateMirror*>(
                storage.state_ptr(node));
        }
    }
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(state->calls, 1u);
    EXPECT_EQ(state->root_block_size, 64u);
    EXPECT_EQ(state->input_block_size, 16u);
    EXPECT_EQ(state->extended_block_size, 17u);
    EXPECT_EQ(state->value_index, 0u);
    EXPECT_FLOAT_EQ(state->first_bin, 0.0f);
    EXPECT_FLOAT_EQ(state->last_bin, 56.0f);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 32);
    EXPECT_EQ(state->calls, 2u);
    EXPECT_EQ(state->root_block_size, 32u);
    EXPECT_EQ(state->input_block_size, 8u);
    EXPECT_EQ(state->extended_block_size, 9u);
    EXPECT_EQ(state->value_index, 16u);
    EXPECT_FLOAT_EQ(state->preceding_bin, 56.0f);
    EXPECT_FLOAT_EQ(state->first_bin, 60.0f);
    EXPECT_FLOAT_EQ(state->last_bin, 88.0f);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(97), 1);
    EXPECT_EQ(state->calls, 3u);
    EXPECT_EQ(state->root_block_size, 1u);
    EXPECT_EQ(state->input_block_size, 0u);
    EXPECT_EQ(state->extended_block_size, 1u);
    EXPECT_EQ(state->value_index, 25u);
    EXPECT_FLOAT_EQ(state->preceding_bin, 88.0f);

    // A transport seam changes the node-visible global lattice immediately,
    // while both the producer's sample history and the consumer's slow FFT
    // history remain contiguous in invocation order. This test producer anchors
    // each spectrum at the preceding four-sample Sequential window.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(1000), 4);
    EXPECT_EQ(state->calls, 4u);
    EXPECT_EQ(state->root_block_size, 4u);
    EXPECT_EQ(state->input_block_size, 1u);
    EXPECT_EQ(state->extended_block_size, 2u);
    EXPECT_EQ(state->value_index, 250u);
    EXPECT_FLOAT_EQ(state->preceding_bin, 88.0f);
    EXPECT_FLOAT_EQ(state->first_bin, 93.0f);
    EXPECT_FLOAT_EQ(state->last_bin, 93.0f);
}

TEST_F(GraphJitRuntimeFixture, TypeOwnedFractionalIndexStorageSupportsFastPace)
{
    auto compiled = compile(graph_jit_fractional_index_value_module_id, 302);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    FractionalIndexConsumerStateMirror* state = nullptr;
    for (std::size_t node = 0;
         node < compiled.compiled_graph->node_layout.nodes.size(); ++node) {
        if (compiled.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(FractionalIndexConsumerStateMirror)) {
            ASSERT_EQ(state, nullptr);
            state = static_cast<FractionalIndexConsumerStateMirror*>(
                storage.state_ptr(node));
        }
    }
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 3);
    EXPECT_EQ(state->calls, 1u);
    EXPECT_EQ(state->root_block_size, 3u);
    EXPECT_EQ(state->input_block_size, 6u);
    EXPECT_EQ(state->value_index, 0u);
    EXPECT_EQ(state->first_whole, 0u);
    EXPECT_EQ(state->first_fraction, 0x8000000000000000ull);
    EXPECT_EQ(state->last_whole, 5u);
    EXPECT_EQ(state->last_fraction, 0x8000000000000005ull);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(3), 2);
    EXPECT_EQ(state->calls, 2u);
    EXPECT_EQ(state->root_block_size, 2u);
    EXPECT_EQ(state->input_block_size, 4u);
    EXPECT_EQ(state->value_index, 6u);
    EXPECT_EQ(state->first_whole, 6u);
    EXPECT_EQ(state->first_fraction, 0x8000000000000006ull);
    EXPECT_EQ(state->last_whole, 9u);
    EXPECT_EQ(state->last_fraction, 0x8000000000000009ull);
}

TEST_F(GraphJitRuntimeFixture, NonSamplePersistedTickCaptureIsRejectedAtItsBoundary)
{
    auto compiled = compile(graph_jit_persisted_fft_value_module_id, 303);
    expect_lowering_failure(
        compiled,
        "persisted Tick capture does not yet support continuous type 'FFTBlock'");
}

TEST_F(GraphJitRuntimeFixture, NonProjectRatePersistedTickCaptureIsRejectedAtItsBoundary)
{
    auto compiled = compile(
        graph_jit_persisted_non_project_rate_sample_module_id, 304);
    expect_lowering_failure(
        compiled,
        "persisted Tick capture does not yet support a non-project-rate continuous output");
}

TEST_F(GraphJitRuntimeFixture, DirectSampleStorage)
{
    auto direct_graph = configured_module_graph(
        *revision, graph_jit_direct_sample_module_id);
    ASSERT_TRUE(direct_graph);
    auto direct_analysis = build_connection_analysis_plan(
        *direct_graph, 64);
    ASSERT_TRUE(direct_analysis.has_value())
        << (direct_analysis ? std::string{} : direct_analysis.error());
    ASSERT_EQ(direct_analysis->sample_producer_groups.size(), 1u);
    ASSERT_TRUE(direct_analysis->sample_producer_groups[0].storage_plan.has_value());
    EXPECT_EQ(
        direct_analysis->sample_producer_groups[0].storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
    auto direct_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *direct_analysis, 64);
    ASSERT_TRUE(direct_plan.has_value())
        << (direct_plan ? std::string{} : direct_plan.error());
    ASSERT_EQ(direct_plan->producer_groups.size(), 1u);
    ASSERT_TRUE(direct_plan->producer_groups[0].has_value());
    ASSERT_EQ(direct_plan->connection_representations.size(), 1u);
    ASSERT_TRUE(direct_plan->connection_representations[0].has_value());
    EXPECT_EQ(
        *direct_plan->connection_representations[0],
        direct_plan->producer_groups[0]->canonical_representation);

    auto direct = compile_graph(direct_graph, 109);
    ASSERT_TRUE(direct.succeeded())
        << (direct.diagnostics.empty() ? "" : direct.diagnostics.front().message);
    ASSERT_EQ(direct.compiled_graph->node_layout.nodes.size(), 2u);
    ASSERT_EQ(count_raw_regions(direct.compiled_graph->node_layout), 1u);

    auto direct_storage = direct.compiled_graph->node_layout.create_storage(resources);
    direct_storage.initialize();
    auto* direct_state = find_consumer_state(*direct.compiled_graph, direct_storage);
    ASSERT_NE(direct_state, nullptr);
    direct.compiled_graph->root_operations.tick_block(
        direct_storage.buffer().data(), &empty_tick_invocation, index(100), 64);
    EXPECT_EQ(direct_state->calls, 1u);
    EXPECT_EQ(direct_state->last_index, 100u);
    EXPECT_EQ(direct_state->last_block_size, 64u);
    EXPECT_FLOAT_EQ(direct_state->first, 100.0f);
    EXPECT_FLOAT_EQ(direct_state->last, 163.0f);
    EXPECT_FLOAT_EQ(direct_state->sum, 8416.0f);
    direct.compiled_graph->root_operations.tick_block(
        direct_storage.buffer().data(), &empty_tick_invocation, index(200), 16);
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
    auto transient_analysis = build_connection_analysis_plan(
        *transient_graph, 64);
    ASSERT_TRUE(transient_analysis.has_value())
        << (transient_analysis ? std::string{} : transient_analysis.error());
    ASSERT_EQ(transient_analysis->sample_producer_groups.size(), 1u);
    ASSERT_TRUE(
        transient_analysis->sample_producer_groups[0].storage_plan.has_value());
    EXPECT_EQ(
        transient_analysis->sample_producer_groups[0].storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
    auto transient_plan =
        iv::graph_jit::detail::build_sample_storage_plan(
            *transient_analysis, 64);
    ASSERT_TRUE(transient_plan.has_value())
        << (transient_plan
                ? std::string{}
                : transient_plan.error());
    ASSERT_EQ(transient_plan->producer_groups.size(), 1u);
    ASSERT_TRUE(transient_plan->producer_groups[0].has_value());
    ASSERT_EQ(transient_plan->connection_representations.size(), 1u);
    ASSERT_TRUE(transient_plan->connection_representations[0].has_value());
    EXPECT_EQ(
        *transient_plan->connection_representations[0],
        transient_plan->producer_groups[0]->canonical_representation);

    auto transient = compile_graph(transient_graph, 110);
    ASSERT_TRUE(transient.succeeded())
        << (transient.diagnostics.empty()
                ? ""
                : transient.diagnostics.front().message);
    ASSERT_EQ(transient.compiled_graph->node_layout.nodes.size(), 2u);
    ASSERT_EQ(count_raw_regions(transient.compiled_graph->node_layout), 1u);
    auto transient_storage =
        transient.compiled_graph->node_layout.create_storage(resources);
    transient_storage.initialize();
    auto* transient_state = find_consumer_state(
        *transient.compiled_graph, transient_storage);
    ASSERT_NE(transient_state, nullptr);

    // The limited producer executes as four 16-frame slices. Each imported
    // invocation reconstructs an ephemeral façade at its absolute sample index,
    // while all slices target the same planned current-block storage.
    // The 64-frame consumer therefore observes one contiguous logical block.
    transient.compiled_graph->root_operations.tick_block(
        transient_storage.buffer().data(), &empty_tick_invocation, index(300), 64);
    EXPECT_EQ(transient_state->calls, 1u);
    EXPECT_EQ(transient_state->last_index, 300u);
    EXPECT_EQ(transient_state->last_block_size, 64u);
    EXPECT_FLOAT_EQ(transient_state->first, 300.0f);
    EXPECT_FLOAT_EQ(transient_state->last, 363.0f);
    EXPECT_FLOAT_EQ(transient_state->sum, 21216.0f);
    transient.compiled_graph->root_operations.tick_block(
        transient_storage.buffer().data(), &empty_tick_invocation, index(500), 32);
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
        build_connection_analysis_plan(
            *reused_arena_graph, 64);
    ASSERT_TRUE(reused_arena_analysis.has_value())
        << (reused_arena_analysis
                ? std::string{}
                : reused_arena_analysis.error());
    auto reused_arena_storage =
        iv::graph_jit::detail::build_sample_storage_plan(
            *reused_arena_analysis, 64);
    ASSERT_TRUE(reused_arena_storage.has_value())
        << (reused_arena_storage
                ? std::string{}
                : reused_arena_storage.error());
    ASSERT_EQ(reused_arena_storage->representations.size(), 2u);
    ASSERT_EQ(reused_arena_storage->transient_allocations.size(), 2u);
    auto const first_arena_allocation =
        reused_arena_storage->representations[0].transient_allocation;
    auto const second_arena_allocation =
        reused_arena_storage->representations[1].transient_allocation;
    ASSERT_LT(
        first_arena_allocation,
        reused_arena_storage->transient_allocations.size());
    ASSERT_LT(
        second_arena_allocation,
        reused_arena_storage->transient_allocations.size());
    EXPECT_EQ(
        reused_arena_storage->transient_allocations[first_arena_allocation]
            .region_relative_offset,
        reused_arena_storage->transient_allocations[second_arena_allocation]
            .region_relative_offset);
    EXPECT_EQ(
        reused_arena_storage->transient_arena_size,
        64u * sizeof(iv::Sample));

    auto reused_arena = compile_graph(reused_arena_graph, 111);
    ASSERT_TRUE(reused_arena.succeeded())
        << (reused_arena.diagnostics.empty()
                ? ""
                : reused_arena.diagnostics.front().message);
    ASSERT_EQ(reused_arena.compiled_graph->node_layout.nodes.size(), 4u);
    ASSERT_EQ(count_raw_regions(reused_arena.compiled_graph->node_layout), 1u);

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
        reused_storage.buffer().data(), &empty_tick_invocation, index(700), 64);
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

TEST_F(GraphJitRuntimeFixture, StackBudgetUsesPackedSampleLifetimes)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_reused_sample_arena_module_id);
    ASSERT_TRUE(graph);

    iv::GraphJit budgeted_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .realtime_storage_cost_model = iv::RealtimeStorageCostModel{
            .stack_budget_bytes = 2u * 64u * sizeof(iv::Sample),
        },
    });
    auto compiled = budgeted_jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 211,
        .graph = graph,
        .definitions = definitions,
    });
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    // The two producer buffers are each 64 samples, but their lifetimes do not
    // overlap, so they pack into one 64-sample range. The block consumers share
    // a second 64-sample contiguous-input scratch range. Both ranges fit the
    // budget, so neither producer is forced into NodeStorage.
    EXPECT_EQ(count_raw_regions(compiled.compiled_graph->node_layout), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(700), 64);

    std::size_t observed = 0;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            != sizeof(SampleConsumerProbeStateMirror)) {
            continue;
        }
        auto const* state = static_cast<SampleConsumerProbeStateMirror const*>(
            storage.state_ptr(i));
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->calls, 1u);
        EXPECT_FLOAT_EQ(state->first, 700.0f);
        EXPECT_FLOAT_EQ(state->last, 763.0f);
        ++observed;
    }
    EXPECT_EQ(observed, 2u);
}

TEST_F(GraphJitRuntimeFixture, StackBudgetMovesOverlappingSampleBuffersToNodeStorage)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_composed_sample_revision_module_id);
    ASSERT_TRUE(graph);

    // Each one-sample-latency producer uses a 128-frame stack ring at the
    // default policy. The two producer lifetimes overlap until the stereo
    // consumer executes, so the packed stack is larger than one ring. A
    // 512-byte limit therefore requires at least one full NodeStorage buffer.
    iv::GraphJit budgeted_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .realtime_storage_cost_model = iv::RealtimeStorageCostModel{
            .stack_budget_bytes = 128u * sizeof(iv::Sample),
        },
    });
    auto compiled = budgeted_jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 212,
        .graph = graph,
        .definitions = definitions,
    });
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    std::size_t full_sample_buffers = 0;
    for (auto const& region : compiled.compiled_graph->node_layout.regions) {
        if (region.kind != iv::NodeLayout::Region::Kind::raw) continue;
        if (region.migration_identity.starts_with("graphjit.sample:")
            && region.size >= 128u * sizeof(iv::Sample)) {
            ++full_sample_buffers;
        }
    }
    EXPECT_GE(full_sample_buffers, 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    StereoSampleConsumerProbeStateMirror* consumer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(StereoSampleConsumerProbeStateMirror)) {
            consumer = static_cast<StereoSampleConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(consumer, nullptr);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_FLOAT_EQ(consumer->first_left, 0.0f);
    EXPECT_FLOAT_EQ(consumer->first_right, 0.0f);
    EXPECT_FLOAT_EQ(consumer->last_left, 102.0f);
    EXPECT_FLOAT_EQ(consumer->last_right, 102.0f);
}

TEST_F(GraphJitRuntimeFixture, StackBudgetRejectsMandatorySampleConversionBuffer)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_stereo_conversion_module_id);
    ASSERT_TRUE(graph);

    iv::GraphJit budgeted_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .realtime_storage_cost_model = iv::RealtimeStorageCostModel{
            .stack_budget_bytes = 32u * sizeof(iv::Sample),
        },
    });
    auto compiled = budgeted_jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 213,
        .graph = graph,
        .definitions = definitions,
    });
    ASSERT_FALSE(compiled.succeeded());
    ASSERT_FALSE(compiled.diagnostics.empty());
    EXPECT_NE(
        compiled.diagnostics.front().message.find(
            "packed realtime stack requires"),
        std::string::npos);
}

TEST_F(GraphJitRuntimeFixture, SampleFanoutConversion)
{
    auto fanout_graph = configured_module_graph(
        *revision, graph_jit_sample_fanout_conversion_module_id);
    ASSERT_TRUE(fanout_graph);
    auto fanout_analysis = build_connection_analysis_plan(
        *fanout_graph, 64);
    ASSERT_TRUE(fanout_analysis.has_value())
        << (fanout_analysis ? std::string{} : fanout_analysis.error());
    ASSERT_EQ(fanout_analysis->sample_producer_groups.size(), 1u);
    ASSERT_EQ(fanout_analysis->sample_producer_groups[0].connection_indices.size(), 4u);
    ASSERT_TRUE(fanout_analysis->sample_producer_groups[0].storage_plan.has_value());
    EXPECT_EQ(
        fanout_analysis->sample_producer_groups[0].storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);

    auto fanout_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *fanout_analysis, 64);
    ASSERT_TRUE(fanout_plan.has_value())
        << (fanout_plan ? std::string{} : fanout_plan.error());
    ASSERT_EQ(fanout_plan->producer_groups.size(), 1u);
    ASSERT_TRUE(fanout_plan->producer_groups[0].has_value());
    ASSERT_EQ(fanout_plan->representations.size(), 1u);
    EXPECT_TRUE(fanout_plan->materializations.empty());
    ASSERT_EQ(fanout_plan->connection_representations.size(), 4u);
    ASSERT_EQ(fanout_plan->connection_channel_bindings.size(), 4u);
    auto const fanout_canonical =
        fanout_plan->producer_groups[0]->canonical_representation;
    EXPECT_EQ(
        fanout_plan->producer_groups[0]
            ->storage_plan.candidate_costs.full_node_storage.copied_bytes,
        0u);
    EXPECT_EQ(
        fanout_plan->producer_groups[0]
            ->storage_plan.candidate_costs.full_node_storage
            .ring_addressed_bytes,
        0u);
    std::size_t direct_whole_port = 0;
    std::size_t channel_granular = 0;
    for (std::size_t connection_index = 0;
         connection_index < fanout_plan->connection_representations.size();
         ++connection_index) {
        if (fanout_plan->connection_representations[connection_index]) {
            EXPECT_EQ(
                *fanout_plan->connection_representations[connection_index],
                fanout_canonical);
            ++direct_whole_port;
            continue;
        }
        ASSERT_TRUE(
            fanout_plan->connection_channel_bindings[connection_index]
                .has_value());
        auto const& channels =
            *fanout_plan->connection_channel_bindings[connection_index];
        ASSERT_FALSE(channels.empty());
        for (auto const& channel : channels) {
            EXPECT_EQ(channel.representation, fanout_canonical);
            EXPECT_EQ(channel.representation_channel, 0u);
            EXPECT_EQ(channel.frame_delay, 0u);
        }
        ++channel_granular;
    }
    EXPECT_EQ(direct_whole_port, 1u);
    EXPECT_EQ(channel_granular, 3u);
    EXPECT_EQ(
        fanout_plan->transient_arena_size,
        64u * sizeof(iv::Sample));

    auto fanout = compile_graph(fanout_graph, 112);
    ASSERT_TRUE(fanout.succeeded())
        << (fanout.diagnostics.empty() ? "" : fanout.diagnostics.front().message);
    ASSERT_EQ(fanout.compiled_graph->node_layout.nodes.size(), 5u);
    ASSERT_EQ(count_raw_regions(fanout.compiled_graph->node_layout), 1u);

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

    // 37..100 crosses the 64-frame storage ring boundary. Converted consumers
    // now read the canonical source channel directly, so absolute-index alias
    // addressing must remain correct across the wrap.
    fanout.compiled_graph->root_operations.tick_block(
        fanout_storage.buffer().data(), &empty_tick_invocation, index(37), 64);
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
        fanout_storage.buffer().data(), &empty_tick_invocation, index(205), 16);
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
        build_connection_analysis_plan(
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
            .storage_plan.has_value());
    EXPECT_EQ(
        stereo_conversion_analysis->sample_producer_groups[0].storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
    auto stereo_conversion_plan =
        iv::graph_jit::detail::build_sample_storage_plan(
            *stereo_conversion_analysis, 64);
    ASSERT_TRUE(stereo_conversion_plan.has_value())
        << (stereo_conversion_plan
                ? std::string{}
                : stereo_conversion_plan.error());
    EXPECT_EQ(stereo_conversion_plan->representations.size(), 2u);
    EXPECT_EQ(stereo_conversion_plan->materializations.size(), 1u);
    ASSERT_TRUE(stereo_conversion_plan->producer_groups[0]);
    auto const& stereo_source_costs =
        stereo_conversion_plan->producer_groups[0]
            ->storage_plan.candidate_costs;
    EXPECT_EQ(
        stereo_source_costs.transient_stack.copied_bytes,
        64u * sizeof(iv::Sample));
    EXPECT_EQ(
        stereo_source_costs.full_node_storage.copied_bytes,
        64u * sizeof(iv::Sample));
    EXPECT_EQ(
        stereo_source_costs.full_node_storage.ring_addressed_bytes,
        128u * sizeof(iv::Sample));
    ASSERT_EQ(
        std::ranges::count_if(
            stereo_conversion_plan->connection_channel_bindings,
            [](auto const& binding) { return binding.has_value(); }),
        1);

    auto stereo_conversion = compile_graph(stereo_conversion_graph, 113);
    ASSERT_TRUE(stereo_conversion.succeeded())
        << (stereo_conversion.diagnostics.empty()
                ? ""
                : stereo_conversion.diagnostics.front().message);
    ASSERT_EQ(stereo_conversion.compiled_graph->node_layout.nodes.size(), 3u);
    ASSERT_EQ(
        count_raw_regions(stereo_conversion.compiled_graph->node_layout), 1u);

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
        stereo_conversion_storage.buffer().data(), &empty_tick_invocation, index(51), 32);
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

TEST_F(GraphJitRuntimeFixture, RetainedStereoToMonoHistoryRecordsItsSources)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_stereo_history_conversion_module_id);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);
    ASSERT_TRUE(analysis->sample_producer_groups.front().storage_plan);
    EXPECT_EQ(
        analysis->sample_producer_groups.front().storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);

    auto compiled = compile_graph(graph, 229);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& requirements = compiled.compiled_graph
        ->realtime_port_state_requirements.states;
    auto const& realizations = compiled.compiled_graph
        ->realtime_port_state_realizations.sample_states;
    ASSERT_EQ(requirements.size(), 1u);
    ASSERT_EQ(realizations.size(), 1u);
    auto const& realization = realizations.front();
    ASSERT_LT(realization.requirement_index, requirements.size());
    EXPECT_EQ(
        requirements[realization.requirement_index].role,
        iv::graph_jit::RealtimePortStateRole::sequential_input_history);
    EXPECT_EQ(
        requirements[realization.requirement_index].extent_samples,
        5u);
    EXPECT_EQ(
        realization.authored_storage.storage,
        iv::graph_jit::SampleRealtimePortStateStorage::callback_transient);
    EXPECT_EQ(
        realization.authored_storage.channel_layout.channel_type,
        iv::ChannelTypeId::mono);
    EXPECT_EQ(
        realization.retained_access,
        iv::graph_jit::SampleRealtimePortStateAccess::materialized);
    ASSERT_TRUE(realization.materialization);
    EXPECT_EQ(
        realization.materialization->source_layout.channel_type,
        iv::ChannelTypeId::stereo);
    EXPECT_EQ(
        realization.materialization->target_layout.channel_type,
        iv::ChannelTypeId::mono);
    EXPECT_EQ(realization.materialization->retained_before, 5u);
    EXPECT_EQ(realization.materialization->latest_read_latency, 0u);
    ASSERT_EQ(realization.retained_sources.size(), 2u);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        auto const& source = realization.retained_sources[channel];
        EXPECT_EQ(
            source.storage,
            iv::graph_jit::SampleRealtimePortStateStorage::compact_carry);
        EXPECT_EQ(source.channel_layout.channel_type, iv::ChannelTypeId::stereo);
        EXPECT_EQ(source.representation_channel, channel);
        EXPECT_EQ(source.timeline_offset_frames, 0);
        ASSERT_TRUE(source.node_storage_offset);
        EXPECT_FALSE(source.callback_arena_offset);
    }
}

TEST_F(GraphJitRuntimeFixture, TransitionRootReconstructsMaterializedSampleHistory)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_stereo_history_conversion_module_id);
    ASSERT_TRUE(graph);
    auto predecessor = compile_graph(graph, 244);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);

    iv::GraphJitCompileRequest successor_request{
        .project_generation = 245,
        .graph = graph,
        .definitions = definitions,
    };
    auto steady_successor = jit->compile(successor_request);
    ASSERT_TRUE(steady_successor.succeeded())
        << (steady_successor.diagnostics.empty()
                ? ""
                : steady_successor.diagnostics.front().message);
    auto specialized = jit->compile_transition(
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
    ASSERT_TRUE(specialized.compiled_graph->cutover);
    ASSERT_NE(
        specialized.compiled_graph->root_operations.transition_tick_block,
        nullptr);
    ASSERT_EQ(
        specialized.compiled_graph->cutover->state_transition
            .sample_expiry_cursors.size(),
        1u);
    EXPECT_EQ(
        specialized.compiled_graph->cutover->state_transition
            .sample_expiry_cursors.front().value_count,
        5u);

    auto predecessor_storage = predecessor.compiled_graph
        ->node_layout.create_storage(resources);
    predecessor_storage.initialize();
    predecessor.compiled_graph->root_operations.tick_block(
        predecessor_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        64);
    predecessor.compiled_graph->root_operations.tick_block(
        predecessor_storage.buffer().data(),
        &empty_tick_invocation,
        index(64),
        64);

    auto transition_storage = specialized.compiled_graph
        ->node_layout.create_storage(resources);
    transition_storage.initialize(&predecessor_storage);
    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        specialized.compiled_graph->cutover->state_transition,
        predecessor_storage.buffer(),
        transition_storage.buffer());
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared,
        predecessor_storage.buffer(),
        transition_storage.buffer(),
        index(128));
    specialized.compiled_graph->root_operations.transition_tick_block(
        transition_storage.buffer().data(),
        &empty_tick_invocation,
        index(128),
        64);

    auto reference_storage = steady_successor.compiled_graph
        ->node_layout.create_storage(resources);
    reference_storage.initialize(&predecessor_storage);
    steady_successor.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(),
        &empty_tick_invocation,
        index(128),
        64);

    HistoryConsumerStateMirror* transition_state = nullptr;
    for (std::size_t node = 0;
         node < specialized.compiled_graph->node_layout.nodes.size(); ++node) {
        if (specialized.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(HistoryConsumerStateMirror)) {
            transition_state = static_cast<HistoryConsumerStateMirror*>(
                transition_storage.state_ptr(node));
        }
    }
    HistoryConsumerStateMirror* reference_state = nullptr;
    for (std::size_t node = 0;
         node < steady_successor.compiled_graph->node_layout.nodes.size(); ++node) {
        if (steady_successor.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(HistoryConsumerStateMirror)) {
            reference_state = static_cast<HistoryConsumerStateMirror*>(
                reference_storage.state_ptr(node));
        }
    }
    ASSERT_NE(transition_state, nullptr);
    ASSERT_NE(reference_state, nullptr);
    EXPECT_FLOAT_EQ(transition_state->current, reference_state->current);
    EXPECT_FLOAT_EQ(transition_state->history_1, reference_state->history_1);
    EXPECT_FLOAT_EQ(transition_state->history_5, reference_state->history_5);
}

TEST_F(GraphJitRuntimeFixture, SampleOutputUpdateRevisesUnpublishedFrames)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_sample_revision_module_id);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);
    auto const& producer = analysis->sample_producer_groups.front();
    EXPECT_EQ(producer.storage_requirements.retained_frames, 1u);
    ASSERT_TRUE(producer.storage_plan.has_value());
    EXPECT_EQ(
        producer.storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->persistent_allocations.size(), 1u);
    EXPECT_EQ(
        sample_plan->persistent_allocations.front().kind,
        iv::graph_jit::detail::SamplePersistentStorageKind::compact_carry);
    EXPECT_TRUE(sample_plan->materializations.empty());

    auto compiled = compile_graph(graph, 114);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 3u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    SampleConsumerProbeStateMirror* mono = nullptr;
    StereoSampleConsumerProbeStateMirror* stereo = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            compiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(SampleConsumerProbeStateMirror)) {
            mono = static_cast<SampleConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        } else if (state_size == sizeof(StereoSampleConsumerProbeStateMirror)) {
            stereo = static_cast<StereoSampleConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(mono, nullptr);
    ASSERT_NE(stereo, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    EXPECT_EQ(mono->calls, 1u);
    EXPECT_FLOAT_EQ(mono->first, 0.0f);
    EXPECT_FLOAT_EQ(mono->last, 102.0f);
    EXPECT_FLOAT_EQ(mono->sum, 303.0f);
    EXPECT_EQ(stereo->calls, 1u);
    EXPECT_FLOAT_EQ(stereo->first_left, 0.0f);
    EXPECT_FLOAT_EQ(stereo->first_right, 0.0f);
    EXPECT_FLOAT_EQ(stereo->last_left, 102.0f);
    EXPECT_FLOAT_EQ(stereo->last_right, 102.0f);
    EXPECT_FLOAT_EQ(stereo->sum_left, 303.0f);
    EXPECT_FLOAT_EQ(stereo->sum_right, 303.0f);

    // Frame 3 was published in the previous root call but remained hidden by
    // the source's one-sample authored latency. The first update in this call
    // must revise that retained frame before either direct or converted fanout
    // observes it.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    EXPECT_EQ(mono->calls, 2u);
    EXPECT_FLOAT_EQ(mono->first, 103.0f);
    EXPECT_FLOAT_EQ(mono->last, 106.0f);
    EXPECT_FLOAT_EQ(mono->sum, 418.0f);
    EXPECT_EQ(stereo->calls, 2u);
    EXPECT_FLOAT_EQ(stereo->first_left, 103.0f);
    EXPECT_FLOAT_EQ(stereo->first_right, 103.0f);
    EXPECT_FLOAT_EQ(stereo->last_left, 106.0f);
    EXPECT_FLOAT_EQ(stereo->last_right, 106.0f);
    EXPECT_FLOAT_EQ(stereo->sum_left, 418.0f);
    EXPECT_FLOAT_EQ(stereo->sum_right, 418.0f);
}

TEST_F(GraphJitRuntimeFixture, SampleOutputUpdateSurvivesPersistentRingStorage)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_persistent_sample_revision_module_id);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);
    auto const& producer = analysis->sample_producer_groups.front();
    EXPECT_EQ(producer.storage_requirements.retained_frames, 5002u);
    ASSERT_TRUE(producer.storage_plan.has_value());
    EXPECT_EQ(
        producer.storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->persistent_allocations.size(), 1u);
    EXPECT_EQ(
        sample_plan->persistent_allocations.front().kind,
        iv::graph_jit::detail::SamplePersistentStorageKind::ring);

    auto compiled = compile_graph(graph, 153);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    SampleConsumerProbeStateMirror* consumer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(SampleConsumerProbeStateMirror)) {
            consumer = static_cast<SampleConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(consumer, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_FLOAT_EQ(consumer->first, 0.0f);
    EXPECT_FLOAT_EQ(consumer->last, 101.0f);
    EXPECT_FLOAT_EQ(consumer->sum, 201.0f);

    // The first update of this invocation revises frame 3 from the preceding
    // root call while the canonical producer storage is a persistent
    // ring rather than compact carry.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    EXPECT_EQ(consumer->calls, 2u);
    EXPECT_FLOAT_EQ(consumer->first, 102.0f);
    EXPECT_FLOAT_EQ(consumer->last, 105.0f);
    EXPECT_FLOAT_EQ(consumer->sum, 414.0f);
}

TEST_F(GraphJitRuntimeFixture, SampleOutputUpdateFeedsComposedFanout)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_composed_sample_revision_module_id);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_producer_groups.size(), 2u);
    for (auto const& producer : analysis->sample_producer_groups) {
        EXPECT_EQ(producer.storage_requirements.retained_frames, 1u);
        ASSERT_TRUE(producer.storage_plan.has_value());
        EXPECT_EQ(
            producer.storage_plan->kind,
            iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    }

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    EXPECT_TRUE(sample_plan->compositions.empty());
    EXPECT_EQ(
        std::ranges::count_if(
            sample_plan->connection_channel_bindings,
            [](auto const& binding) { return binding.has_value(); }),
        1u);
    EXPECT_TRUE(sample_plan->feedback_timelines.empty());

    auto compiled = compile_graph(graph, 154);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 3u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    StereoSampleConsumerProbeStateMirror* consumer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(StereoSampleConsumerProbeStateMirror)) {
            consumer = static_cast<StereoSampleConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(consumer, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_FLOAT_EQ(consumer->first_left, 0.0f);
    EXPECT_FLOAT_EQ(consumer->first_right, 0.0f);
    EXPECT_FLOAT_EQ(consumer->last_left, 102.0f);
    EXPECT_FLOAT_EQ(consumer->last_right, 102.0f);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    EXPECT_EQ(consumer->calls, 2u);
    EXPECT_FLOAT_EQ(consumer->first_left, 103.0f);
    EXPECT_FLOAT_EQ(consumer->first_right, 103.0f);
    EXPECT_FLOAT_EQ(consumer->last_left, 106.0f);
    EXPECT_FLOAT_EQ(consumer->last_right, 106.0f);
}

TEST_F(GraphJitRuntimeFixture, TickOnlySampleNodePreservesContextAcrossPrimitiveSlices)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_tick_fallback_sample_module_id);
    ASSERT_TRUE(graph);
    auto compiled = compile_graph(graph, 130);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    SampleConsumerProbeStateMirror* consumer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(SampleConsumerProbeStateMirror)) {
            consumer = static_cast<SampleConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(consumer, nullptr);

    // The source callback is sliced as [0,2), [2,4). Because it only
    // implements tick(), each slice is further executed one sample at a time.
    // update() at index 2 therefore revises a frame authored by the
    // preceding primitive callback invocation, while the one-sample source
    // latency keeps those frames unpublished until the consumer runs.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_EQ(consumer->last_index, 0u);
    EXPECT_EQ(consumer->last_block_size, 4u);
    EXPECT_FLOAT_EQ(consumer->first, 0.0f);
    EXPECT_FLOAT_EQ(consumer->last, 102.0f);
    EXPECT_FLOAT_EQ(consumer->sum, 303.0f);

    // The next root call begins by revising frame 3 from the previous root.
    // Later updates cross the [4,6) -> [6,8) primitive-slice boundary too.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    EXPECT_EQ(consumer->calls, 2u);
    EXPECT_EQ(consumer->last_index, 4u);
    EXPECT_EQ(consumer->last_block_size, 4u);
    EXPECT_FLOAT_EQ(consumer->first, 103.0f);
    EXPECT_FLOAT_EQ(consumer->last, 106.0f);
    EXPECT_FLOAT_EQ(consumer->sum, 418.0f);
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
        latency_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
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
        latency_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
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
        build_connection_analysis_plan(
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
    EXPECT_EQ(latency_source_group->storage_requirements.retained_frames, 7u);
    ASSERT_TRUE(latency_source_group->storage_plan.has_value());
    EXPECT_EQ(
        latency_source_group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);

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

    auto latency_conversion_plan =
        iv::graph_jit::detail::build_sample_storage_plan(
            *latency_conversion_analysis, 64);
    ASSERT_TRUE(latency_conversion_plan.has_value())
        << (latency_conversion_plan
                ? std::string{}
                : latency_conversion_plan.error());
    auto const latency_source_group_index = static_cast<std::size_t>(
        latency_source_group
        - latency_conversion_analysis->sample_producer_groups.begin());
    ASSERT_LT(
        latency_source_group_index,
        latency_conversion_plan->producer_groups.size());
    ASSERT_TRUE(
        latency_conversion_plan->producer_groups[latency_source_group_index]
            .has_value());
    auto const latency_source_representation =
        latency_conversion_plan->producer_groups[latency_source_group_index]
            ->canonical_representation;
    EXPECT_TRUE(latency_conversion_plan->materializations.empty());
    std::size_t aliased_converted_connections = 0;
    for (auto const connection_index : latency_source_group->connection_indices) {
        auto const& connection =
            latency_conversion_analysis->sample_connections[connection_index];
        if (!connection.requires_conversion) continue;
        ASSERT_LT(
            connection_index,
            latency_conversion_plan->connection_channel_bindings.size());
        auto const& bindings =
            latency_conversion_plan->connection_channel_bindings[connection_index];
        ASSERT_TRUE(bindings.has_value());
        ASSERT_EQ(bindings->size(), 1u);
        EXPECT_EQ(bindings->front().representation, latency_source_representation);
        EXPECT_EQ(bindings->front().representation_channel, 0u);
        EXPECT_EQ(bindings->front().frame_delay, 0u);
        ++aliased_converted_connections;
    }
    EXPECT_EQ(aliased_converted_connections, 2u);

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
        latency_conversion_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(interleaved_latency_probe->calls, 1u);
    EXPECT_EQ(interleaved_latency_probe->mismatches, 0u);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->fast_first, 0.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->slow_first, 0.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->fast_last, 56.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->slow_last, 56.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->max_abs_difference, 0.0f);
    EXPECT_EQ(interleaved_latency_probe->marker, 0x1a7e2e0u);

    // This observer shares the same converted interleaved storage as the
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
        latency_conversion_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
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
        build_connection_analysis_plan(
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
    ASSERT_EQ(composed_connection->projection_contributions.size(), 1u);
    EXPECT_EQ(
        composed_connection->projection_contributions[0].source_type,
        iv::ChannelTypeId::stereo);
    EXPECT_EQ(
        composed_connection->projection_contributions[0].target_type,
        iv::ChannelTypeId::stereo);
    EXPECT_EQ(
        composed_connection->projection_contributions[0].source_channel_indices,
        (std::vector<std::size_t>{0u, 1u}));
    EXPECT_EQ(
        composed_connection->projection_contributions[0].target_channels,
        (std::vector<std::size_t>{0u, 1u}));
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
        composed_latency_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(composed_probe->calls, 1u);
    EXPECT_EQ(composed_probe->last_index, 0u);
    EXPECT_EQ(composed_probe->last_block_size, 64u);
    EXPECT_FLOAT_EQ(composed_probe->first_left, 0.0f);
    EXPECT_FLOAT_EQ(composed_probe->first_right, 0.0f);
    EXPECT_FLOAT_EQ(composed_probe->last_left, 56.0f);
    EXPECT_FLOAT_EQ(composed_probe->last_right, 56.0f);
    EXPECT_FLOAT_EQ(composed_probe->sum_left, 1596.0f);
    EXPECT_FLOAT_EQ(composed_probe->sum_right, 1596.0f);

    // The second call proves the channel-granular input reads each producer's
    // independently restored history (7 frames from the direct source, 2 from
    // the delayed source) without a gathered stereo storage.
    composed_latency.compiled_graph->root_operations.tick_block(
        composed_latency_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(composed_probe->calls, 2u);
    EXPECT_EQ(composed_probe->last_index, 64u);
    EXPECT_EQ(composed_probe->last_block_size, 64u);
    EXPECT_FLOAT_EQ(composed_probe->first_left, 57.0f);
    EXPECT_FLOAT_EQ(composed_probe->first_right, 57.0f);
    EXPECT_FLOAT_EQ(composed_probe->last_left, 120.0f);
    EXPECT_FLOAT_EQ(composed_probe->last_right, 120.0f);
    EXPECT_FLOAT_EQ(composed_probe->sum_left, 5664.0f);
    EXPECT_FLOAT_EQ(composed_probe->sum_right, 5664.0f);

    // A composed input with history must extend each aliased producer's
    // retention by the target history. The direct channel therefore needs
    // 5 + 7 frames while the delayed channel needs 5 + 2; the logical stereo
    // input remains timestamp-aligned at port latency 0.
}

TEST_F(GraphJitRuntimeFixture, ComposedSampleHistory)
{
    auto composed_history_graph = configured_module_graph(
        *revision, graph_jit_composed_history_module_id);
    ASSERT_TRUE(composed_history_graph);
    auto composed_history_analysis =
        build_connection_analysis_plan(
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
    EXPECT_EQ(direct_history_group->storage_requirements.retained_frames, 12u);
    EXPECT_EQ(delayed_history_group->storage_requirements.retained_frames, 7u);

    auto composed_history_plan =
        iv::graph_jit::detail::build_sample_storage_plan(
            *composed_history_analysis, 64);
    ASSERT_TRUE(composed_history_plan.has_value())
        << (composed_history_plan
                ? std::string{}
                : composed_history_plan.error());
    EXPECT_TRUE(composed_history_plan->compositions.empty());
    auto const composed_history_connection_index = static_cast<std::size_t>(
        std::distance(
            composed_history_analysis->sample_connections.begin(),
            composed_history_connection));
    ASSERT_LT(
        composed_history_connection_index,
        composed_history_plan->connection_channel_bindings.size());
    ASSERT_TRUE(
        composed_history_plan
            ->connection_channel_bindings[composed_history_connection_index]
            .has_value());
    auto const& composed_history_aliases =
        *composed_history_plan
            ->connection_channel_bindings[composed_history_connection_index];
    ASSERT_EQ(composed_history_aliases.size(), 2u);
    EXPECT_EQ(composed_history_aliases[0].frame_delay, 7u);
    EXPECT_EQ(composed_history_aliases[1].frame_delay, 2u);
    EXPECT_NE(
        composed_history_aliases[0].representation,
        composed_history_aliases[1].representation);

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
        composed_history_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(composed_history_probe->calls, 1u);
    EXPECT_EQ(composed_history_probe->last_index, 0u);
    EXPECT_FLOAT_EQ(composed_history_probe->current_left, 0.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->current_right, 0.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_left, 0.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_right, 0.0f);

    // At absolute index 64 both channels represent source frame 57 after path
    // equalization, and get(5) must reconstruct source frame 52 on each side.
    composed_history.compiled_graph->root_operations.tick_block(
        composed_history_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
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
        composed_history_storage.buffer().data(), &empty_tick_invocation, index(128), 4);
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
            // The left target is deliberately expressed as stereo->mono using
            // semantic channels from two distinct producer ports. The
            // conversion must read those sources directly without gathering a
            // contiguous stereo input. The right target remains a direct alias,
            // so one logical input mixes computed and aliased channel bindings.
            projected_connections.push_back(iv::ConfiguredSampleConnection{
                .source_type = iv::ChannelTypeId::stereo,
                .source_channels = {
                    connection.source_channels[0],
                    connection.source_channels[1],
                },
                .target_type = iv::ChannelTypeId::mono,
                .target_channels = {connection.target_channels[0]},
            });
            projected_connections.push_back(iv::ConfiguredSampleConnection{
                .source_type = iv::ChannelTypeId::mono,
                .source_channels = {connection.source_channels[1]},
                .target_type = iv::ChannelTypeId::mono,
                .target_channels = {connection.target_channels[1]},
            });
        } else {
            projected_connections.push_back(connection);
        }
    }
    ASSERT_EQ(projected_connections.size(), 3u);
    projected_graph->connections = iv::GraphBuilderConnections::from_configured_connections(
        projected_connections,
        projected_base_graph->connections.configured_event_connections());

    auto projected_analysis = build_connection_analysis_plan(
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
    ASSERT_GE(projected_connection->source_channel_timings.size(), 2u);
    ASSERT_EQ(projected_connection->target_channels.size(), 2u);
    ASSERT_EQ(projected_connection->projection_contributions.size(), 2u);
    auto const mixed = std::ranges::find_if(
        projected_connection->projection_contributions,
        [](auto const& contribution) {
            return contribution.source_type == iv::ChannelTypeId::stereo
                && contribution.target_type == iv::ChannelTypeId::mono;
        });
    ASSERT_NE(mixed, projected_connection->projection_contributions.end());
    ASSERT_EQ(mixed->source_channel_indices.size(), 2u);
    ASSERT_EQ(mixed->target_channels, (std::vector<std::size_t>{0u}));
    auto const aliased = std::ranges::find_if(
        projected_connection->projection_contributions,
        [](auto const& contribution) {
            return contribution.source_type == iv::ChannelTypeId::mono
                && contribution.target_type == iv::ChannelTypeId::mono;
        });
    ASSERT_NE(aliased, projected_connection->projection_contributions.end());
    ASSERT_EQ(aliased->target_channels, (std::vector<std::size_t>{1u}));
    EXPECT_EQ(projected_connection->target_channels[0].channel, 0u);
    EXPECT_EQ(projected_connection->target_channels[1].channel, 1u);

    auto projected_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *projected_analysis, 64);
    ASSERT_TRUE(projected_plan.has_value())
        << (projected_plan ? std::string{} : projected_plan.error());
    ASSERT_EQ(projected_plan->compositions.size(), 1u);
    auto const projected_connection_index = static_cast<std::size_t>(
        std::distance(
            projected_analysis->sample_connections.begin(),
            projected_connection));
    ASSERT_LT(
        projected_connection_index,
        projected_plan->connection_channel_bindings.size());
    ASSERT_TRUE(
        projected_plan
            ->connection_channel_bindings[projected_connection_index]
            .has_value());
    auto const& projected_bindings =
        *projected_plan
            ->connection_channel_bindings[projected_connection_index];
    ASSERT_EQ(projected_bindings.size(), 2u);

    auto const& computed = projected_plan->compositions.front();
    ASSERT_EQ(computed.connection_index, projected_connection_index);
    ASSERT_EQ(computed.contributions.size(), 1u);
    EXPECT_EQ(
        computed.contributions.front().source_layout.channel_type,
        iv::ChannelTypeId::stereo);
    EXPECT_EQ(
        computed.contributions.front().converted_layout.channel_type,
        iv::ChannelTypeId::mono);
    ASSERT_EQ(computed.contributions.front().sources.size(), 2u);
    EXPECT_NE(
        computed.contributions.front().sources[0].source_representation,
        computed.contributions.front().sources[1].source_representation);
    EXPECT_EQ(projected_bindings[0].representation, computed.target_representation);
    EXPECT_EQ(projected_bindings[0].representation_channel, 0u);
    EXPECT_EQ(projected_bindings[0].frame_delay, 0u);

    EXPECT_NE(projected_bindings[1].representation, computed.target_representation);
    EXPECT_EQ(projected_bindings[1].representation_channel, 0u);
    EXPECT_EQ(projected_bindings[1].frame_delay, 2u);

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
        projected_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(projected_probe->calls, 1u);
    EXPECT_FLOAT_EQ(projected_probe->first_left, 0.0f);
    EXPECT_FLOAT_EQ(projected_probe->first_right, 0.0f);
    EXPECT_FLOAT_EQ(projected_probe->last_left, 556.0f);
    EXPECT_FLOAT_EQ(projected_probe->last_right, 56.0f);
    EXPECT_FLOAT_EQ(projected_probe->sum_left, 30096.0f);
    EXPECT_FLOAT_EQ(projected_probe->sum_right, 1596.0f);

    projected.compiled_graph->root_operations.tick_block(
        projected_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(projected_probe->calls, 2u);
    EXPECT_FLOAT_EQ(projected_probe->first_left, 557.0f);
    EXPECT_FLOAT_EQ(projected_probe->first_right, 57.0f);
    EXPECT_FLOAT_EQ(projected_probe->last_left, 620.0f);
    EXPECT_FLOAT_EQ(projected_probe->last_right, 120.0f);
    EXPECT_FLOAT_EQ(projected_probe->sum_left, 37664.0f);
    EXPECT_FLOAT_EQ(projected_probe->sum_right, 5664.0f);

}

TEST_F(GraphJitRuntimeFixture, DirectEventFlow)
{
    auto direct_event_graph = configured_module_graph(
        *revision, graph_jit_direct_event_module_id);
    ASSERT_TRUE(direct_event_graph);
    auto direct_event_analysis =
        build_connection_analysis_plan(
            *direct_event_graph, 64);
    ASSERT_TRUE(direct_event_analysis.has_value())
        << (direct_event_analysis
                ? std::string{}
                : direct_event_analysis.error());
    ASSERT_EQ(direct_event_analysis->event_connections.size(), 1u);
    ASSERT_EQ(direct_event_analysis->event_producer_groups.size(), 1u);
    auto const& direct_event_group =
        direct_event_analysis->event_producer_groups.front();
    ASSERT_TRUE(direct_event_group.storage_plan.has_value());
    EXPECT_EQ(
        direct_event_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
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
        direct_event_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
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
        direct_event_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(direct_event_probe->calls, 2u);
    EXPECT_EQ(direct_event_probe->last_index, 64u);
    EXPECT_EQ(direct_event_probe->event_count, 2u);
    EXPECT_EQ(direct_event_probe->trigger_count, 2u);
    EXPECT_EQ(direct_event_probe->first_time, 67u);
    EXPECT_EQ(direct_event_probe->last_time, 127u);

}

TEST_F(GraphJitRuntimeFixture, ExactSampleDetachFeedback)
{
    auto feedback_graph = configured_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const detached = std::ranges::find_if(
        analysis->sample_connections,
        [](iv::graph_jit::detail::SampleConnectionPlan const& connection) {
            return connection.detach.has_value();
        });
    ASSERT_NE(detached, analysis->sample_connections.end());
    ASSERT_TRUE(detached->detach.has_value());
    EXPECT_EQ(detached->detach->loop_extra_latency, 6u);
    ASSERT_TRUE(detached->detach_initial_value.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(
            *detached->detach_initial_value)),
        -0.625f);
    ASSERT_TRUE(detached->detach_region.has_value());
    ASSERT_LT(*detached->detach_region, analysis->schedule.regions.size());
    auto const& region = analysis->schedule.regions[*detached->detach_region];
    ASSERT_TRUE(region.cyclic);
    EXPECT_EQ(region.maximum_block_size, 4u);
    EXPECT_EQ(region.scc_feedback_latency, 4u);

    auto compiled = compile_graph(feedback_graph, 122);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    SampleFeedbackAStateMirror* state_a = nullptr;
    SampleFeedbackBStateMirror* state_b = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            compiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(SampleFeedbackAStateMirror)) {
            ASSERT_EQ(state_a, nullptr);
            state_a = static_cast<SampleFeedbackAStateMirror*>(
                storage.state_ptr(i));
        } else if (state_size == sizeof(SampleFeedbackBStateMirror)) {
            ASSERT_EQ(state_b, nullptr);
            state_b = static_cast<SampleFeedbackBStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(state_a, nullptr);
    ASSERT_NE(state_b, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 13);

    ASSERT_EQ(state_a->calls, 4u);
    ASSERT_EQ(state_b->calls, 4u);
    EXPECT_EQ(state_a->scc_feedback_latency, 4u);
    EXPECT_EQ(state_b->scc_feedback_latency, 4u);
    EXPECT_EQ(state_a->marker, 0x5a11ce01u);
    EXPECT_EQ(state_b->marker, 0x5b22ce02ull);

    std::array<std::uint64_t, 4> const expected_indices{0, 4, 8, 12};
    std::array<std::uint64_t, 4> const expected_sizes{4, 4, 4, 1};
    std::array<float, 4> const expected_first{-0.625f, -0.625f, 0.375f, 1.375f};
    std::array<float, 4> const expected_last{-0.625f, 0.375f, 0.375f, 1.375f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state_a->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state_b->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state_a->block_sizes[slice], expected_sizes[slice]);
        EXPECT_EQ(state_b->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state_a->first_inputs[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state_b->first_inputs[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state_a->last_inputs[slice], expected_last[slice]);
        EXPECT_FLOAT_EQ(state_b->last_inputs[slice], expected_last[slice]);
    }

    // The detached branch stores A's produced value at its absolute sample
    // index and reads it six samples later. Recompile at an awkward boundary:
    // the next root call must observe A[7..12], proving the feedback timeline was
    // migrated rather than reinitialized to the authored -0.625 value.
    auto recompiled = compile_graph(feedback_graph, 123);
    ASSERT_TRUE(recompiled.succeeded())
        << (recompiled.diagnostics.empty()
                ? ""
                : recompiled.diagnostics.front().message);
    auto migrated_storage =
        recompiled.compiled_graph->node_layout.create_storage(resources);
    migrated_storage.initialize(&storage);
    SampleFeedbackAStateMirror* migrated_a = nullptr;
    SampleFeedbackBStateMirror* migrated_b = nullptr;
    for (std::size_t i = 0;
         i < recompiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            recompiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(SampleFeedbackAStateMirror)) {
            migrated_a = static_cast<SampleFeedbackAStateMirror*>(
                migrated_storage.state_ptr(i));
        } else if (state_size == sizeof(SampleFeedbackBStateMirror)) {
            migrated_b = static_cast<SampleFeedbackBStateMirror*>(
                migrated_storage.state_ptr(i));
        }
    }
    ASSERT_NE(migrated_a, nullptr);
    ASSERT_NE(migrated_b, nullptr);

    recompiled.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(13), 6);
    ASSERT_EQ(migrated_a->calls, 2u);
    ASSERT_EQ(migrated_b->calls, 2u);
    EXPECT_EQ(migrated_a->indices[0], 13u);
    EXPECT_EQ(migrated_b->indices[0], 13u);
    EXPECT_EQ(migrated_a->block_sizes[0], 4u);
    EXPECT_EQ(migrated_b->block_sizes[0], 4u);
    EXPECT_FLOAT_EQ(migrated_a->first_inputs[0], 1.375f);
    EXPECT_FLOAT_EQ(migrated_b->first_inputs[0], 1.375f);
    EXPECT_FLOAT_EQ(migrated_a->last_inputs[0], 1.375f);
    EXPECT_FLOAT_EQ(migrated_b->last_inputs[0], 1.375f);
    EXPECT_EQ(migrated_a->indices[1], 17u);
    EXPECT_EQ(migrated_b->indices[1], 17u);
    EXPECT_EQ(migrated_a->block_sizes[1], 2u);
    EXPECT_EQ(migrated_b->block_sizes[1], 2u);
    EXPECT_FLOAT_EQ(migrated_a->first_inputs[1], 1.375f);
    EXPECT_FLOAT_EQ(migrated_b->first_inputs[1], 1.375f);
    EXPECT_FLOAT_EQ(migrated_a->last_inputs[1], 2.375f);
    EXPECT_FLOAT_EQ(migrated_b->last_inputs[1], 2.375f);
}

TEST_F(GraphJitRuntimeFixture, SampleFeedbackSccFansOutToAcyclicIdentityAndConvertedHistoryConsumers)
{
    auto feedback_graph = configured_sample_feedback_scc_fanout_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());

    std::size_t fanout_leaving_cycle = 0;
    for (auto const& connection : analysis->sample_connections) {
        if (connection.detach || connection.source_channel_timings.empty()) continue;
        auto const source_bundle =
            connection.source_channel_timings.front().source.bundle;
        auto const target_bundle = connection.target_port.node_bundle_handle;
        if (source_bundle >= analysis->schedule.bundle_to_region.size()
            || target_bundle >= analysis->schedule.bundle_to_region.size()
            || !analysis->schedule.bundle_to_region[source_bundle]
            || !analysis->schedule.bundle_to_region[target_bundle]) {
            continue;
        }
        auto const source_region =
            *analysis->schedule.bundle_to_region[source_bundle];
        auto const target_region =
            *analysis->schedule.bundle_to_region[target_bundle];
        if (source_region >= analysis->schedule.regions.size()
            || target_region >= analysis->schedule.regions.size()) {
            continue;
        }
        if (analysis->schedule.regions[source_region].cyclic
            && !analysis->schedule.regions[target_region].cyclic) {
            ++fanout_leaving_cycle;
        }
    }
    EXPECT_EQ(fanout_leaving_cycle, 2u);

    auto compiled = compile_graph(feedback_graph, 140);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 8);

    // Both probe states are 40 bytes, so state size alone cannot distinguish
    // them. Identify the stereo/history probe by its post-execution marker and
    // treat the other 40-byte state as the identity consumer.
    SampleConsumerProbeStateMirror* identity = nullptr;
    StereoHistoryConsumerStateMirror* converted_history = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            != sizeof(SampleConsumerProbeStateMirror)) {
            continue;
        }
        auto* candidate = storage.state_ptr(i);
        auto* history_candidate =
            static_cast<StereoHistoryConsumerStateMirror*>(candidate);
        if (history_candidate->marker == 0xa1b2c3d4e5f60718ull) {
            ASSERT_EQ(converted_history, nullptr);
            converted_history = history_candidate;
        } else {
            ASSERT_EQ(identity, nullptr);
            identity = static_cast<SampleConsumerProbeStateMirror*>(candidate);
        }
    }
    ASSERT_NE(identity, nullptr);
    ASSERT_NE(converted_history, nullptr);

    // A's output is 0.375 + floor(sample / 6). The acyclic identity consumer
    // executes once per root call, after the feedback SCC has completed all of
    // its internal 4-sample slices, and must still see the whole root window.
    ASSERT_EQ(identity->calls, 2u);
    EXPECT_EQ(identity->last_index, 64u);
    EXPECT_EQ(identity->last_block_size, 8u);
    EXPECT_FLOAT_EQ(identity->first, 10.375f);
    EXPECT_FLOAT_EQ(identity->last, 11.375f);
    EXPECT_FLOAT_EQ(identity->sum, 89.0f);

    // The sibling branch simultaneously exercises mono->stereo conversion and
    // five samples of target history across the SCC boundary. At sample 64,
    // history 5 is sample 59, whose value is 9.375.
    ASSERT_EQ(converted_history->calls, 2u);
    EXPECT_EQ(converted_history->last_index, 64u);
    EXPECT_FLOAT_EQ(converted_history->current_left, 10.375f);
    EXPECT_FLOAT_EQ(converted_history->current_right, 10.375f);
    EXPECT_FLOAT_EQ(converted_history->history_5_left, 9.375f);
    EXPECT_FLOAT_EQ(converted_history->history_5_right, 9.375f);
    EXPECT_EQ(converted_history->marker, 0xa1b2c3d4e5f60718ull);
}

TEST_F(GraphJitRuntimeFixture, MultipleSampleDetachBranchesShareProducerHomeAndFallbackCopy)
{
    auto feedback_graph = configured_multi_branch_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 3u);
    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 3u);
    ASSERT_EQ(sample_plan->persistent_allocations.size(), 2u);

    auto const canonical =
        sample_plan->producer_groups.front()->canonical_representation;
    ASSERT_LT(canonical, sample_plan->representations.size());
    EXPECT_EQ(
        sample_plan->representations[canonical].storage,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);

    auto const timeline_for_latency = [&](std::size_t latency)
        -> iv::graph_jit::detail::SampleFeedbackTimelinePlan const* {
        auto const it = std::ranges::find_if(
            sample_plan->feedback_timelines,
            [&](auto const& timeline) {
                return timeline.loop_extra_latency == latency;
            });
        return it == sample_plan->feedback_timelines.end()
            ? nullptr
            : std::addressof(*it);
    };
    auto const* fast = timeline_for_latency(4);
    auto const* initialized = timeline_for_latency(6);
    auto const* slow = timeline_for_latency(8);
    ASSERT_NE(fast, nullptr);
    ASSERT_NE(initialized, nullptr);
    ASSERT_NE(slow, nullptr);

    EXPECT_EQ(
        fast->writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::producer_home);
    EXPECT_EQ(fast->timeline_representation, canonical);
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(fast->initial_value)),
        0.0f);
    EXPECT_EQ(
        slow->writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::producer_home);
    EXPECT_EQ(slow->timeline_representation, canonical);
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(slow->initial_value)),
        0.0f);

    EXPECT_EQ(
        initialized->writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::copy);
    EXPECT_NE(initialized->timeline_representation, canonical);
    EXPECT_EQ(initialized->writer.source_representation, canonical);
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(
            initialized->initial_value)),
        -2.0f);

    auto compiled = compile_graph(feedback_graph, 135);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<MultiBranchSampleFeedbackStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 13);

    ASSERT_EQ(state->calls, 4u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);
    EXPECT_EQ(state->marker, 0x6d756c74u);

    std::array<std::uint64_t, 4> const expected_indices{0, 4, 8, 12};
    std::array<std::uint64_t, 4> const expected_sizes{4, 4, 4, 1};
    std::array<float, 4> const expected_first_fast{0.0f, 1.0f, 2.0f, 3.0f};
    std::array<float, 4> const expected_last_fast{0.0f, 1.0f, 2.0f, 3.0f};
    std::array<float, 4> const expected_first_slow{0.0f, 0.0f, 1.0f, 2.0f};
    std::array<float, 4> const expected_last_slow{0.0f, 0.0f, 1.0f, 2.0f};
    std::array<float, 4> const expected_first_initialized{-2.0f, -2.0f, 1.0f, 2.0f};
    std::array<float, 4> const expected_last_initialized{-2.0f, 1.0f, 2.0f, 2.0f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->first_fast[slice], expected_first_fast[slice]);
        EXPECT_FLOAT_EQ(state->last_fast[slice], expected_last_fast[slice]);
        EXPECT_FLOAT_EQ(state->first_slow[slice], expected_first_slow[slice]);
        EXPECT_FLOAT_EQ(state->last_slow[slice], expected_last_slow[slice]);
        EXPECT_FLOAT_EQ(
            state->first_initialized[slice], expected_first_initialized[slice]);
        EXPECT_FLOAT_EQ(
            state->last_initialized[slice], expected_last_initialized[slice]);
    }
}

TEST_F(GraphJitRuntimeFixture, MultipleSampleDetachBranchesMigrateSharedHomeAndFallbackCopy)
{
    auto feedback_graph = configured_multi_branch_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto current = compile_graph(feedback_graph, 136);
    ASSERT_TRUE(current.succeeded())
        << (current.diagnostics.empty()
                ? ""
                : current.diagnostics.front().message);
    auto storage = current.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    current.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 13);

    // The two zero-initialized detach branches share one producer-home ring,
    // while the non-zero branch owns a second branch-local ring. Snapshot both
    // retained timelines before migration so we prove they migrate
    // independently rather than merely checking the post-migration waveform.
    struct RetainedRegionSnapshot {
        std::string identity;
        std::vector<std::byte> bytes;
    };
    auto retained_regions = [](iv::NodeStorage const& node_storage) {
        std::vector<RetainedRegionSnapshot> result;
        if (!node_storage.layout) return result;
        for (std::size_t i = 0; i < node_storage.layout->regions.size(); ++i) {
            auto const& region = node_storage.layout->regions[i];
            if (region.kind != iv::NodeLayout::Region::Kind::raw
                || !(region.migration_identity.starts_with("graphjit.sample:")
                    || region.migration_identity.starts_with(
                        "graphjit.sample.feedback:"))) {
                continue;
            }
            iv::NodeLayout::RegionHandle handle{.index = i};
            auto const bytes = node_storage.region_bytes(handle);
            result.push_back(RetainedRegionSnapshot{
                .identity = region.migration_identity,
                .bytes = std::vector<std::byte>(bytes.begin(), bytes.end()),
            });
        }
        std::ranges::sort(
            result, {}, &RetainedRegionSnapshot::identity);
        return result;
    };

    auto const before = retained_regions(storage);
    ASSERT_EQ(before.size(), 2u);
    EXPECT_EQ(
        std::ranges::count_if(before, [](auto const& region) {
            return region.identity.starts_with("graphjit.sample:");
        }),
        1u);
    EXPECT_EQ(
        std::ranges::count_if(before, [](auto const& region) {
            return region.identity.starts_with("graphjit.sample.feedback:");
        }),
        1u);

    auto migrated = compile_graph(feedback_graph, 137);
    ASSERT_TRUE(migrated.succeeded())
        << (migrated.diagnostics.empty()
                ? ""
                : migrated.diagnostics.front().message);
    auto migrated_storage =
        migrated.compiled_graph->node_layout.create_storage(resources);
    migrated_storage.initialize(&storage);

    auto const after = retained_regions(migrated_storage);
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(after[i].identity, before[i].identity);
        EXPECT_EQ(after[i].bytes, before[i].bytes);
    }

    auto* state = static_cast<MultiBranchSampleFeedbackStateMirror*>(
        migrated_storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    // Resume at an awkward absolute index. The shared producer-home timeline
    // must continue both zero-initialized branches, while the independently
    // migrated initialized timeline must continue without replaying its -2 pre-roll.
    migrated.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(13), 6);

    ASSERT_EQ(state->calls, 2u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);
    EXPECT_EQ(state->marker, 0x6d756c74u);

    EXPECT_EQ(state->indices[0], 13u);
    EXPECT_EQ(state->block_sizes[0], 4u);
    EXPECT_FLOAT_EQ(state->first_fast[0], 3.0f);
    EXPECT_FLOAT_EQ(state->last_fast[0], 4.0f);
    EXPECT_FLOAT_EQ(state->first_slow[0], 2.0f);
    EXPECT_FLOAT_EQ(state->last_slow[0], 3.0f);
    EXPECT_FLOAT_EQ(state->first_initialized[0], 2.0f);
    EXPECT_FLOAT_EQ(state->last_initialized[0], 3.0f);

    EXPECT_EQ(state->indices[1], 17u);
    EXPECT_EQ(state->block_sizes[1], 2u);
    EXPECT_FLOAT_EQ(state->first_fast[1], 4.0f);
    EXPECT_FLOAT_EQ(state->last_fast[1], 4.0f);
    EXPECT_FLOAT_EQ(state->first_slow[1], 3.0f);
    EXPECT_FLOAT_EQ(state->last_slow[1], 3.0f);
    EXPECT_FLOAT_EQ(state->first_initialized[1], 3.0f);
    EXPECT_FLOAT_EQ(state->last_initialized[1], 4.0f);
}

TEST_F(GraphJitRuntimeFixture, SampleDetachFeedbackPreservesSourceLatencyAndTargetHistory)
{
    auto feedback_graph = configured_temporal_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 1u);
    auto const& connection = analysis->sample_connections.front();
    ASSERT_TRUE(connection.detach.has_value());
    EXPECT_EQ(connection.detach->loop_extra_latency, 6u);
    EXPECT_EQ(connection.source_latency, 2u);
    EXPECT_EQ(connection.read_latency, 2u);
    EXPECT_EQ(connection.target_history, 3u);
    ASSERT_TRUE(connection.detach_initial_value.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(
            *connection.detach_initial_value)),
        -0.625f);

    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);
    auto const& producer = analysis->sample_producer_groups.front();
    // Feedback delay/history remains branch-local, but the canonical producer
    // must retain its own authored latency horizon because OutputPort::update()
    // may revise those already-authored frames on a later invocation.
    EXPECT_EQ(producer.storage_requirements.retained_frames, 2u);
    ASSERT_TRUE(producer.storage_plan.has_value());
    EXPECT_EQ(
        producer.storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 1u);
    ASSERT_EQ(sample_plan->persistent_allocations.size(), 2u);
    ASSERT_EQ(sample_plan->producer_authored_extents.size(), 1u);
    EXPECT_EQ(
        sample_plan->producer_authored_extents.front().source_representation,
        sample_plan->producer_groups.front()->canonical_representation);
    EXPECT_EQ(
        sample_plan->producer_authored_extents.front().maximum_revision_frames,
        2u);
    auto const& timeline = sample_plan->feedback_timelines.front();
    EXPECT_EQ(
        timeline.writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::copy);
    EXPECT_EQ(timeline.writer.revision_frames, 2u);
    auto const ring_representation = timeline.timeline_representation;
    ASSERT_LT(ring_representation, sample_plan->representations.size());
    auto const persistent_index =
        sample_plan->representations[ring_representation].persistent_allocation;
    ASSERT_LT(persistent_index, sample_plan->persistent_allocations.size());
    auto const& persistent = sample_plan->persistent_allocations[persistent_index];
    EXPECT_EQ(persistent.retained_frames, 11u);
    EXPECT_EQ(persistent.frame_capacity, 128u);

    auto compiled = compile_graph(feedback_graph, 124);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<TemporalSampleFeedbackStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 17);

    ASSERT_EQ(state->calls, 5u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);
    EXPECT_EQ(state->marker, 0x7e4fba11u);

    std::array<std::uint64_t, 5> const expected_indices{0, 4, 8, 12, 16};
    std::array<std::uint64_t, 5> const expected_sizes{4, 4, 4, 4, 1};
    std::array<float, 5> const expected_current{
        -0.625f, -0.625f, 0.375f, 0.375f, 1.375f};
    std::array<float, 5> const expected_history_3{
        -0.625f, -0.625f, -0.625f, 0.375f, 0.375f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->current_inputs[slice], expected_current[slice]);
        EXPECT_FLOAT_EQ(
            state->history_3_inputs[slice], expected_history_3[slice]);
        EXPECT_FLOAT_EQ(state->first_inputs[slice], expected_current[slice]);
        EXPECT_FLOAT_EQ(state->last_inputs[slice], expected_current[slice]);
    }
}

TEST_F(GraphJitRuntimeFixture, SampleDetachFeedbackRecopiesAuthoredLatencyHorizon)
{
    auto feedback_graph = configured_revising_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 1u);
    auto const& connection = analysis->sample_connections.front();
    ASSERT_TRUE(connection.detach.has_value());
    EXPECT_EQ(connection.source_latency, 2u);
    EXPECT_EQ(connection.detach->loop_extra_latency, 6u);
    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);
    EXPECT_EQ(
        analysis->sample_producer_groups.front().storage_requirements.retained_frames,
        2u);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 1u);
    auto const& timeline = sample_plan->feedback_timelines.front();
    EXPECT_EQ(
        timeline.writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::copy);
    EXPECT_EQ(timeline.writer.revision_frames, 2u);

    auto compiled = compile_graph(feedback_graph, 152);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<RevisingSampleFeedbackStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(8), 4);

    ASSERT_EQ(state->calls, 3u);
    EXPECT_EQ(state->indices[0], 0u);
    EXPECT_EQ(state->indices[1], 4u);
    EXPECT_EQ(state->indices[2], 8u);
    EXPECT_FLOAT_EQ(state->first_inputs[0], -1.0f);
    EXPECT_FLOAT_EQ(state->last_inputs[0], -1.0f);
    EXPECT_FLOAT_EQ(state->first_inputs[1], -1.0f);
    EXPECT_FLOAT_EQ(state->last_inputs[1], -1.0f);
    EXPECT_FLOAT_EQ(state->first_inputs[2], 0.0f);
    EXPECT_FLOAT_EQ(state->last_inputs[2], 103.0f);
    EXPECT_EQ(state->marker, 0x5a17e001u);
}

TEST_F(GraphJitRuntimeFixture, ProjectedSampleDetachFeedbackRecomputesRevisionHorizon)
{
    auto feedback_graph =
        configured_projected_revising_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 1u);
    auto const& connection = analysis->sample_connections.front();
    EXPECT_FALSE(connection.detach.has_value());
    ASSERT_TRUE(iv::graph_jit::detail::has_detached_source(connection));
    EXPECT_EQ(connection.source_latency, 2u);
    EXPECT_FALSE(connection.canonical_source_port.has_value());
    ASSERT_EQ(connection.source_channel_timings.size(), 3u);
    ASSERT_EQ(connection.routed_contributions.size(), 2u);
    for (auto const& contribution : connection.routed_contributions) {
        ASSERT_TRUE(contribution.detach.has_value());
        EXPECT_EQ(contribution.detach->loop_extra_latency, 6u);
    }

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 2u);
    ASSERT_EQ(sample_plan->compositions.size(), 1u);
    for (auto const& timeline : sample_plan->feedback_timelines) {
        EXPECT_EQ(
            timeline.writer.kind,
            iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::composition);
        EXPECT_EQ(timeline.writer.revision_frames, 2u);
        ASSERT_EQ(timeline.writer.composition_contributions.size(), 1u);
    }

    auto compiled = compile_graph(feedback_graph, 155);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<ProjectedRevisingSampleFeedbackStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(8), 4);

    ASSERT_EQ(state->calls, 3u);
    EXPECT_EQ(state->indices[0], 0u);
    EXPECT_EQ(state->indices[1], 4u);
    EXPECT_EQ(state->indices[2], 8u);
    EXPECT_FLOAT_EQ(state->first_left[0], -1.0f);
    EXPECT_FLOAT_EQ(state->first_right[0], -1.0f);
    EXPECT_FLOAT_EQ(state->last_left[0], -1.0f);
    EXPECT_FLOAT_EQ(state->last_right[0], -1.0f);
    EXPECT_FLOAT_EQ(state->first_left[1], -1.0f);
    EXPECT_FLOAT_EQ(state->first_right[1], -1.0f);
    EXPECT_FLOAT_EQ(state->last_left[1], -1.0f);
    EXPECT_FLOAT_EQ(state->last_right[1], -1.0f);
    EXPECT_FLOAT_EQ(state->first_left[2], 0.0f);
    EXPECT_FLOAT_EQ(state->first_right[2], 0.0f);
    EXPECT_FLOAT_EQ(state->last_left[2], 103.0f);
    EXPECT_FLOAT_EQ(state->last_right[2], 103.0f);
    EXPECT_EQ(state->marker, 0x52e71e55u);
}

TEST_F(GraphJitRuntimeFixture, ConvertedSampleDetachFeedback)
{
    auto feedback_graph = configured_converted_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 1u);
    auto const& connection = analysis->sample_connections.front();
    ASSERT_TRUE(connection.detach.has_value());
    EXPECT_EQ(connection.detach->loop_extra_latency, 6u);
    EXPECT_TRUE(connection.requires_conversion);
    ASSERT_TRUE(connection.canonical_source_layout.has_value());
    EXPECT_EQ(
        connection.canonical_source_layout->channel_type,
        iv::ChannelTypeId::mono);
    EXPECT_EQ(connection.target_layout.channel_type, iv::ChannelTypeId::stereo);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 1u);
    EXPECT_TRUE(sample_plan->materializations.empty());
    auto const& timeline = sample_plan->feedback_timelines.front();
    EXPECT_EQ(
        timeline.writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::copy);
    auto const ring = timeline.timeline_representation;
    EXPECT_EQ(
        sample_plan->representations[ring].channel_layout.channel_type,
        iv::ChannelTypeId::mono);
    EXPECT_FALSE(sample_plan->connection_representations.front().has_value());
    ASSERT_TRUE(sample_plan->connection_channel_bindings.front().has_value());
    auto const& converted_channels =
        *sample_plan->connection_channel_bindings.front();
    ASSERT_EQ(converted_channels.size(), 2u);
    for (auto const& channel : converted_channels) {
        EXPECT_EQ(channel.representation, ring);
        EXPECT_EQ(channel.representation_channel, 0u);
        EXPECT_EQ(channel.frame_delay, 0u);
    }

    auto compiled = compile_graph(feedback_graph, 125);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<ConvertedSampleFeedbackStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 17);

    ASSERT_EQ(state->calls, 5u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);
    EXPECT_EQ(state->marker, 0xc04e7ed1u);

    std::array<std::uint64_t, 5> const expected_indices{0, 4, 8, 12, 16};
    std::array<std::uint64_t, 5> const expected_sizes{4, 4, 4, 4, 1};
    std::array<float, 5> const expected_first{
        -0.25f, -0.25f, 0.75f, 1.75f, 1.75f};
    std::array<float, 5> const expected_last{
        -0.25f, 0.75f, 0.75f, 1.75f, 1.75f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->first_left[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state->first_right[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state->last_left[slice], expected_last[slice]);
        EXPECT_FLOAT_EQ(state->last_right[slice], expected_last[slice]);
    }
}

TEST_F(GraphJitRuntimeFixture, ZeroInitializedConvertedFeedbackWritesDirectlyToProducerHome)
{
    auto feedback_graph = configured_converted_sample_feedback_graph(
        *revision, 6, iv::Sample{0.0f});
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 1u);
    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);
    EXPECT_EQ(
        analysis->sample_producer_groups.front().storage_requirements.retained_frames,
        0u);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 1u);
    EXPECT_EQ(
        sample_plan->feedback_timelines.front().writer.kind,
        iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::producer_home);
    EXPECT_EQ(sample_plan->feedback_timelines.front().writer.revision_frames, 0u);
    ASSERT_EQ(sample_plan->persistent_allocations.size(), 1u);
    EXPECT_TRUE(sample_plan->materializations.empty());
    ASSERT_EQ(sample_plan->representations.size(), 1u);

    auto const canonical =
        sample_plan->producer_groups.front()->canonical_representation;
    EXPECT_TRUE(
        sample_plan->representations[canonical].canonical_producer_representation);
    EXPECT_EQ(
        sample_plan->representations[canonical].storage,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(
        sample_plan->representations[canonical].channel_layout.channel_type,
        iv::ChannelTypeId::mono);
    EXPECT_FALSE(sample_plan->connection_representations.front().has_value());
    ASSERT_TRUE(sample_plan->connection_channel_bindings.front().has_value());
    auto const& converted_channels =
        *sample_plan->connection_channel_bindings.front();
    ASSERT_EQ(converted_channels.size(), 2u);
    for (auto const& channel : converted_channels) {
        EXPECT_EQ(channel.representation, canonical);
        EXPECT_EQ(channel.representation_channel, 0u);
        EXPECT_EQ(channel.frame_delay, 0u);
    }

    auto compiled = compile_graph(feedback_graph, 126);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<ConvertedSampleFeedbackStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 17);

    ASSERT_EQ(state->calls, 5u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);
    EXPECT_EQ(state->marker, 0xc04e7ed1u);

    std::array<std::uint64_t, 5> const expected_indices{0, 4, 8, 12, 16};
    std::array<std::uint64_t, 5> const expected_sizes{4, 4, 4, 4, 1};
    std::array<float, 5> const expected_first{0.0f, 0.0f, 1.0f, 2.0f, 2.0f};
    std::array<float, 5> const expected_last{0.0f, 1.0f, 1.0f, 2.0f, 2.0f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->first_left[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state->first_right[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state->last_left[slice], expected_last[slice]);
        EXPECT_FLOAT_EQ(state->last_right[slice], expected_last[slice]);
    }
}

TEST_F(GraphJitRuntimeFixture, ProjectedSampleDetachFeedback)
{
    auto feedback_graph = configured_projected_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_connections.size(), 2u);
    auto detached = std::ranges::find_if(
        analysis->sample_connections,
        [](auto const& connection) {
            return iv::graph_jit::detail::has_detached_source(connection);
        });
    ASSERT_NE(detached, analysis->sample_connections.end());
    EXPECT_FALSE(detached->detach.has_value());
    EXPECT_FALSE(detached->canonical_source_port.has_value());
    EXPECT_FALSE(detached->canonical_source_layout.has_value());
    ASSERT_EQ(detached->source_channel_timings.size(), 3u);
    EXPECT_EQ(detached->source_channel_timings[0].source.channel, 0u);
    EXPECT_EQ(detached->source_channel_timings[1].source.channel, 0u);
    EXPECT_EQ(detached->source_channel_timings[2].source.channel, 0u);
    EXPECT_EQ(detached->source_channel_timings[0].read_latency, 2u);
    EXPECT_EQ(detached->source_channel_timings[1].read_latency, 2u);
    EXPECT_EQ(detached->source_channel_timings[2].read_latency, 2u);
    EXPECT_TRUE(detached->projection_contributions.empty());
    ASSERT_EQ(detached->routed_contributions.size(), 2u);
    EXPECT_EQ(
        detached->routed_contributions[0].source_coefficients,
        (std::vector<float>{0.5f, 0.5f, 0.0f}));
    EXPECT_EQ(
        detached->routed_contributions[0].target_coefficients,
        (std::vector<float>{1.0f, 0.0f}));
    EXPECT_EQ(
        detached->routed_contributions[1].source_coefficients,
        (std::vector<float>{0.0f, 0.0f, 1.0f}));
    EXPECT_EQ(
        detached->routed_contributions[1].target_coefficients,
        (std::vector<float>{0.0f, 1.0f}));
    EXPECT_EQ(detached->target_layout.channel_type, iv::ChannelTypeId::stereo);
    for (auto const& contribution : detached->routed_contributions) {
        ASSERT_TRUE(contribution.detach.has_value());
        EXPECT_EQ(contribution.detach->loop_extra_latency, 6u);
        ASSERT_TRUE(contribution.detach_region.has_value());
    }
    auto const detach_region =
        *detached->routed_contributions.front().detach_region;
    ASSERT_LT(detach_region, analysis->schedule.regions.size());
    auto const& region = analysis->schedule.regions[detach_region];
    EXPECT_TRUE(region.cyclic);
    EXPECT_EQ(region.maximum_block_size, 4u);
    EXPECT_EQ(region.scc_feedback_latency, 4u);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 2u);
    ASSERT_EQ(sample_plan->compositions.size(), 1u);
    auto const detached_index = static_cast<std::size_t>(
        std::distance(analysis->sample_connections.begin(), detached));
    ASSERT_LT(detached_index, sample_plan->connection_representations.size());
    ASSERT_TRUE(sample_plan->connection_representations[detached_index].has_value());
    auto const composed =
        *sample_plan->connection_representations[detached_index];
    ASSERT_LT(composed, sample_plan->representations.size());
    EXPECT_EQ(
        sample_plan->representations[composed].storage,
        iv::RealtimeBufferStorageKind::transient_stack);
    EXPECT_EQ(
        sample_plan->representations[composed].channel_layout.channel_type,
        iv::ChannelTypeId::stereo);
    for (auto const& timeline : sample_plan->feedback_timelines) {
        EXPECT_EQ(timeline.connection_index, detached_index);
        EXPECT_EQ(timeline.channel_layout.channel_type, iv::ChannelTypeId::mono);
        EXPECT_EQ(timeline.retained_frames, 8u);
        EXPECT_EQ(timeline.loop_extra_latency, 6u);
        EXPECT_EQ(
            timeline.writer.kind,
            iv::graph_jit::detail::SampleFeedbackTimelineWriterKind::composition);
        EXPECT_EQ(timeline.writer.revision_frames, 2u);
        ASSERT_EQ(timeline.writer.composition_contributions.size(), 1u);
        auto const representation = timeline.timeline_representation;
        ASSERT_LT(representation, sample_plan->representations.size());
        EXPECT_EQ(
            sample_plan->representations[representation].storage,
            iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    }
    auto const mixed_timeline = std::ranges::find_if(
        sample_plan->feedback_timelines,
        [](auto const& timeline) {
            return timeline.writer.composition_contributions.front()
                    .sources.size() == 2u;
        });
    ASSERT_NE(mixed_timeline, sample_plan->feedback_timelines.end());
    EXPECT_EQ(
        mixed_timeline->writer.composition_contributions.front()
            .source_layout.channel_type,
        iv::ChannelTypeId::stereo);

    auto compiled = compile_graph(feedback_graph, 127);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 17);

    // TemporalSampleFeedback and ConvertedSampleFeedback intentionally have
    // same-sized state mirrors, so state_size cannot identify the converted
    // node in this two-node fixture. Identify it by the node-specific marker
    // written by tick_block instead.
    ConvertedSampleFeedbackStateMirror* state = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            != sizeof(ConvertedSampleFeedbackStateMirror)) {
            continue;
        }
        auto* candidate = static_cast<ConvertedSampleFeedbackStateMirror*>(
            storage.state_ptr(i));
        if (candidate != nullptr && candidate->marker == 0xc04e7ed1u) {
            ASSERT_EQ(state, nullptr);
            state = candidate;
        }
    }
    ASSERT_NE(state, nullptr);

    ASSERT_EQ(state->calls, 5u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);
    EXPECT_EQ(state->marker, 0xc04e7ed1u);

    std::array<std::uint64_t, 5> const expected_indices{0, 4, 8, 12, 16};
    std::array<std::uint64_t, 5> const expected_sizes{4, 4, 4, 4, 1};
    std::array<float, 5> const expected_first{
        -0.25f, -0.25f, 1.75f, 1.75f, 3.75f};
    std::array<float, 5> const expected_last{
        -0.25f, -0.25f, 1.75f, 1.75f, 3.75f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->first_left[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state->first_right[slice], expected_first[slice]);
        EXPECT_FLOAT_EQ(state->last_left[slice], expected_last[slice]);
        EXPECT_FLOAT_EQ(state->last_right[slice], expected_last[slice]);
    }
}


TEST_F(GraphJitRuntimeFixture, ProjectedSampleDetachFeedbackAlignsUnequalMixingLatencies)
{
    auto feedback_graph =
        configured_unequal_latency_projected_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto detached = std::ranges::find_if(
        analysis->sample_connections,
        [](auto const& connection) {
            return iv::graph_jit::detail::has_detached_source(connection);
        });
    ASSERT_NE(detached, analysis->sample_connections.end());
    ASSERT_EQ(detached->routed_contributions.size(), 2u);
    auto const mixed = std::ranges::find_if(
        detached->routed_contributions,
        [](auto const& contribution) {
            return std::ranges::count_if(
                contribution.source_coefficients,
                [](float coefficient) { return coefficient != 0.0f; }) == 2u;
        });
    ASSERT_NE(mixed, detached->routed_contributions.end());
    std::vector<std::size_t> mixed_source_indices;
    for (std::size_t source = 0;
         source < mixed->source_coefficients.size(); ++source) {
        if (mixed->source_coefficients[source] != 0.0f)
            mixed_source_indices.push_back(source);
    }
    ASSERT_EQ(mixed_source_indices.size(), 2u);
    auto const first_latency = detached->source_channel_timings[
        mixed_source_indices[0]].read_latency;
    auto const second_latency = detached->source_channel_timings[
        mixed_source_indices[1]].read_latency;
    EXPECT_EQ(std::min(first_latency, second_latency), 0u);
    EXPECT_EQ(std::max(first_latency, second_latency), 2u);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    auto const aligned_timeline = std::ranges::find_if(
        sample_plan->feedback_timelines,
        [](auto const& timeline) {
            return std::ranges::any_of(
                timeline.writer.composition_contributions,
                [](auto const& contribution) {
                    return contribution.feedback_alignment_representation
                        != iv::graph_jit::detail::no_sample_representation;
                });
        });
    ASSERT_NE(aligned_timeline, sample_plan->feedback_timelines.end());
    auto const& aligned =
        aligned_timeline->writer.composition_contributions.front();
    ASSERT_NE(
        aligned.feedback_alignment_representation,
        iv::graph_jit::detail::no_sample_representation);
    EXPECT_EQ(aligned.feedback_alignment_write_latency, 0u);

    auto compiled = compile_graph(feedback_graph, 128);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 17);

    ConvertedSampleFeedbackStateMirror* state = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            != sizeof(ConvertedSampleFeedbackStateMirror)) {
            continue;
        }
        auto* candidate = static_cast<ConvertedSampleFeedbackStateMirror*>(
            storage.state_ptr(i));
        if (candidate != nullptr && candidate->marker == 0xc04e7ed1u) {
            ASSERT_EQ(state, nullptr);
            state = candidate;
        }
    }
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->calls, 5u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);

    std::array<std::uint64_t, 5> const expected_indices{0, 4, 8, 12, 16};
    std::array<std::uint64_t, 5> const expected_sizes{4, 4, 4, 4, 1};
    std::array<float, 5> const expected_first_left{
        -0.25f, -0.25f, 1.25f, 1.375f, 3.0f};
    std::array<float, 5> const expected_last_left{
        -0.25f, 0.25f, 1.25f, 2.25f, 3.0f};
    std::array<float, 5> const expected_first_right{
        -0.25f, -0.25f, 1.75f, 1.75f, 3.5f};
    std::array<float, 5> const expected_last_right{
        -0.25f, -0.25f, 1.75f, 2.0f, 3.5f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->first_left[slice], expected_first_left[slice]);
        EXPECT_FLOAT_EQ(state->last_left[slice], expected_last_left[slice]);
        EXPECT_FLOAT_EQ(state->first_right[slice], expected_first_right[slice]);
        EXPECT_FLOAT_EQ(state->last_right[slice], expected_last_right[slice]);
    }
}


TEST_F(GraphJitRuntimeFixture, ProjectedSampleDetachFeedbackPermutesTargetChannels)
{
    auto feedback_graph =
        configured_permuted_unequal_latency_projected_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto detached = std::ranges::find_if(
        analysis->sample_connections,
        [](auto const& connection) {
            return iv::graph_jit::detail::has_detached_source(connection);
        });
    ASSERT_NE(detached, analysis->sample_connections.end());
    ASSERT_EQ(detached->routed_contributions.size(), 2u);
    EXPECT_EQ(
        detached->routed_contributions[0].target_coefficients,
        (std::vector<float>{0.0f, 1.0f}));
    EXPECT_EQ(
        detached->routed_contributions[1].target_coefficients,
        (std::vector<float>{1.0f, 0.0f}));

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->feedback_timelines.size(), 2u);
    ASSERT_EQ(sample_plan->compositions.size(), 1u);
    auto const& final = sample_plan->compositions.front().contributions.front();
    EXPECT_EQ(
        final.transform.coefficients,
        (std::vector<float>{0.0f, 1.0f, 1.0f, 0.0f}));

    auto compiled = compile_graph(feedback_graph, 129);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 17);

    ConvertedSampleFeedbackStateMirror* state = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            != sizeof(ConvertedSampleFeedbackStateMirror)) {
            continue;
        }
        auto* candidate = static_cast<ConvertedSampleFeedbackStateMirror*>(
            storage.state_ptr(i));
        if (candidate != nullptr && candidate->marker == 0xc04e7ed1u) {
            ASSERT_EQ(state, nullptr);
            state = candidate;
        }
    }
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->calls, 5u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);

    std::array<std::uint64_t, 5> const expected_indices{0, 4, 8, 12, 16};
    std::array<std::uint64_t, 5> const expected_sizes{4, 4, 4, 4, 1};
    // This is the unequal-latency feedback waveform with its semantic target
    // projection swapped. If lowering accidentally canonicalizes contribution
    // order instead of honoring target_channels, these pairs are reversed.
    std::array<float, 5> const expected_first_left{
        -0.25f, -0.25f, 1.75f, 1.75f, 3.5f};
    std::array<float, 5> const expected_last_left{
        -0.25f, -0.25f, 1.75f, 2.0f, 3.5f};
    std::array<float, 5> const expected_first_right{
        -0.25f, -0.25f, 1.25f, 1.375f, 3.0f};
    std::array<float, 5> const expected_last_right{
        -0.25f, 0.25f, 1.25f, 2.25f, 3.0f};
    for (std::size_t slice = 0; slice < expected_indices.size(); ++slice) {
        EXPECT_EQ(state->indices[slice], expected_indices[slice]);
        EXPECT_EQ(state->block_sizes[slice], expected_sizes[slice]);
        EXPECT_FLOAT_EQ(state->first_left[slice], expected_first_left[slice]);
        EXPECT_FLOAT_EQ(state->last_left[slice], expected_last_left[slice]);
        EXPECT_FLOAT_EQ(state->first_right[slice], expected_first_right[slice]);
        EXPECT_FLOAT_EQ(state->last_right[slice], expected_last_right[slice]);
    }
}

TEST_F(GraphJitRuntimeFixture, UnequalLatencySampleFeedbackMigratesAlignmentPrehistory)
{
    auto feedback_graph =
        configured_unequal_latency_projected_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto reference = compile_graph(feedback_graph, 129);
    ASSERT_TRUE(reference.succeeded())
        << (reference.diagnostics.empty()
                ? ""
                : reference.diagnostics.front().message);
    auto current = compile_graph(feedback_graph, 130);
    ASSERT_TRUE(current.succeeded())
        << (current.diagnostics.empty()
                ? ""
                : current.diagnostics.front().message);

    auto reference_storage =
        reference.compiled_graph->node_layout.create_storage(resources);
    reference_storage.initialize();
    auto storage = current.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    // After one frame, the detached routed contribution's alignment ring
    // contains one produced frame plus initialized prehistory for the older
    // aligned read. The complete ring must migrate; there is deliberately no
    // separate validity/warmup scalar.
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(0), 1);
    current.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 1);

    auto const reference_regions =
        sample_feedback_alignment_regions(reference_storage);
    auto const current_regions = sample_feedback_alignment_regions(storage);
    ASSERT_EQ(reference_regions.size(), 1u);
    ASSERT_EQ(current_regions.size(), 1u);
    auto const staged_snapshot =
        snapshot_sample_feedback_alignment_regions(storage);

    auto migrated = compile_graph(feedback_graph, 131);
    ASSERT_TRUE(migrated.succeeded())
        << (migrated.diagnostics.empty()
                ? ""
                : migrated.diagnostics.front().message);
    auto migrated_storage =
        migrated.compiled_graph->node_layout.create_storage(resources);
    migrated_storage.initialize(&storage);

    expect_sample_feedback_alignment_regions_equal(
        staged_snapshot, migrated_storage);

    auto* reference_state_before =
        converted_sample_feedback_state(reference_storage);
    ASSERT_NE(reference_state_before, nullptr);
    auto const reference_prefix_calls =
        static_cast<std::size_t>(reference_state_before->calls);

    // Root tick_block() accepts only power-of-two block sizes. Continue the
    // same logical 11-frame suffix using legal calls on both generations.
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(1), 8);
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(9), 2);
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(11), 1);
    migrated.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(1), 8);
    migrated.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(9), 2);
    migrated.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(11), 1);

    auto* reference_state = converted_sample_feedback_state(reference_storage);
    auto* migrated_state = converted_sample_feedback_state(migrated_storage);
    ASSERT_NE(reference_state, nullptr);
    ASSERT_NE(migrated_state, nullptr);
    expect_converted_feedback_suffix_equal(
        *reference_state, reference_prefix_calls, *migrated_state);
}

TEST_F(GraphJitRuntimeFixture, UnequalLatencySampleFeedbackMigratesPopulatedAlignmentRing)
{
    auto feedback_graph =
        configured_unequal_latency_projected_sample_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto reference = compile_graph(feedback_graph, 132);
    ASSERT_TRUE(reference.succeeded())
        << (reference.diagnostics.empty()
                ? ""
                : reference.diagnostics.front().message);
    auto current = compile_graph(feedback_graph, 133);
    ASSERT_TRUE(current.succeeded())
        << (current.diagnostics.empty()
                ? ""
                : current.diagnostics.front().message);

    auto reference_storage =
        reference.compiled_graph->node_layout.create_storage(resources);
    reference_storage.initialize();
    auto storage = current.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    // After three frames, the detached routed contribution's alignment ring
    // contains produced samples alongside its still-valid initialized
    // prehistory. Migration must preserve the complete ring rather than
    // reinitializing either part.
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(0), 2);
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(2), 1);
    current.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 2);
    current.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(2), 1);

    auto const current_regions = sample_feedback_alignment_regions(storage);
    ASSERT_EQ(current_regions.size(), 1u);
    auto const staged_snapshot =
        snapshot_sample_feedback_alignment_regions(storage);

    auto migrated = compile_graph(feedback_graph, 134);
    ASSERT_TRUE(migrated.succeeded())
        << (migrated.diagnostics.empty()
                ? ""
                : migrated.diagnostics.front().message);
    auto migrated_storage =
        migrated.compiled_graph->node_layout.create_storage(resources);
    auto migration = migrated_storage.prepare_migration_from(storage);
    migration.commit();

    // Raw compiler-owned state is copied when the prepared migration commits
    // at the activation boundary, before the new generation can observe it.
    expect_sample_feedback_alignment_regions_equal(
        staged_snapshot, migrated_storage);

    auto* reference_state_before =
        converted_sample_feedback_state(reference_storage);
    ASSERT_NE(reference_state_before, nullptr);
    auto const reference_prefix_calls =
        static_cast<std::size_t>(reference_state_before->calls);

    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(3), 8);
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(11), 2);
    migrated.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(3), 8);
    migrated.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(11), 2);

    auto* reference_state = converted_sample_feedback_state(reference_storage);
    auto* migrated_state = converted_sample_feedback_state(migrated_storage);
    ASSERT_NE(reference_state, nullptr);
    ASSERT_NE(migrated_state, nullptr);
    expect_converted_feedback_suffix_equal(
        *reference_state, reference_prefix_calls, *migrated_state);
}


TEST_F(GraphJitRuntimeFixture, ExactTypeEventDetachFeedback)
{
    auto feedback_graph = configured_event_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
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
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);

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
    // 10, so retained feedback storage must carry time 67 across both the
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
        migrated_storage.buffer().data(), &empty_tick_invocation, index(64), 8);
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
        tail_storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), &empty_tick_invocation, index(4), 2);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), &empty_tick_invocation, index(6), 1);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), &empty_tick_invocation, index(7), 8);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), &empty_tick_invocation, index(15), 1);
    compiled.compiled_graph->root_operations.tick_block(
        tail_storage.buffer().data(), &empty_tick_invocation, index(16), 2);

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

TEST_F(GraphJitRuntimeFixture, MixedEventDetachComposesOrdinaryAndDelayedSources)
{
    auto graph = configured_mixed_event_feedback_graph(*revision);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(*graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const mixed = std::ranges::find_if(
        analysis->event_connections,
        [](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            return connection.requires_source_composition;
        });
    ASSERT_NE(mixed, analysis->event_connections.end());
    ASSERT_EQ(mixed->source_plans.size(), 2u);
    ASSERT_TRUE(mixed->source_plans[0].detach.has_value());
    EXPECT_FALSE(mixed->source_plans[1].detach.has_value());

    auto compiled = compile_graph(graph, 250);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventFeedbackBStateMirror* state = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventFeedbackBStateMirror)) {
            ASSERT_EQ(state, nullptr);
            state = static_cast<EventFeedbackBStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(state->calls, 8u);
    EXPECT_EQ(state->scc_feedback_latency, 8u);
    for (std::size_t slice = 0; slice < 8; ++slice) {
        auto const slice_index = slice * 8u;
        EXPECT_EQ(state->indices[slice], slice_index);
        EXPECT_EQ(state->block_sizes[slice], 8u);
        if (slice == 0) {
            EXPECT_EQ(state->input_counts[slice], 1u);
            EXPECT_EQ(state->first_input_times[slice], 3u);
        } else if (slice == 7) {
            EXPECT_EQ(state->input_counts[slice], 2u);
            EXPECT_EQ(state->first_input_times[slice], 59u);
        } else {
            EXPECT_EQ(state->input_counts[slice], 1u);
            EXPECT_EQ(
                state->first_input_times[slice],
                (slice - 1u) * 8u + 11u);
        }
    }

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 8);
    ASSERT_EQ(state->calls, 9u);
    EXPECT_EQ(state->indices[8], 64u);
    EXPECT_EQ(state->block_sizes[8], 8u);
    EXPECT_EQ(state->input_counts[8], 3u);
    EXPECT_EQ(state->first_input_times[8], 67u);
}

TEST_F(GraphJitRuntimeFixture, EventFanInStagesAcyclicIngressIntoFeedbackScc)
{
    auto graph = configured_staged_event_fan_in_graph(*revision);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const mixed = std::ranges::find_if(
        analysis->event_connections,
        [](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            return !connection.detach && connection.sources.size() == 2;
        });
    ASSERT_NE(mixed, analysis->event_connections.end());
    auto const first_region = analysis->schedule.bundle_to_region[
        mixed->sources[0].bundle];
    auto const second_region = analysis->schedule.bundle_to_region[
        mixed->sources[1].bundle];
    ASSERT_TRUE(first_region.has_value());
    ASSERT_TRUE(second_region.has_value());
    EXPECT_NE(*first_region, *second_region);
    EXPECT_TRUE(analysis->schedule.regions[*first_region].cyclic);
    EXPECT_FALSE(analysis->schedule.regions[*second_region].cyclic);

    auto compiled = compile_graph(graph, 214);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    EXPECT_TRUE(std::ranges::any_of(
        compiled.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.migration_identity.find("kind=ordered_compact_carry")
                != std::string::npos;
        }));
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventFeedbackAStateMirror* state = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventFeedbackAStateMirror)) {
            ASSERT_EQ(state, nullptr);
            state = static_cast<EventFeedbackAStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(state->calls, 8u);
    for (std::size_t slice = 0; slice < 8; ++slice) {
        auto const index = slice * 8u;
        EXPECT_EQ(state->indices[slice], index);
        EXPECT_EQ(state->block_sizes[slice], 8u);
        auto const expected_count = slice == 0
            ? 2u
            : (slice == 1 || slice == 7 ? 3u : 2u);
        EXPECT_EQ(state->input_counts[slice], expected_count);
        EXPECT_EQ(
            state->first_input_times[slice],
            slice == 0 ? 2u : index - 6u);
    }

    // The mixed fan-in's ordered compact carry restores B's event at 58 and
    // ingress event at 63. The next SCC slice inserts B's new event at 66 ahead
    // of the ingress source's 67/71 events without losing semantic tie order.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 8);
    ASSERT_EQ(state->calls, 9u);
    EXPECT_EQ(state->indices[8], 64u);
    EXPECT_EQ(state->block_sizes[8], 8u);
    EXPECT_EQ(state->input_counts[8], 5u);
    EXPECT_EQ(state->first_input_times[8], 58u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccConsumesRetainedAndConvertedHistoryPerSlice)
{
    auto feedback_graph = configured_retained_converted_event_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());

    std::size_t retained_inside_connections = 0;
    bool saw_exact = false;
    bool saw_converted = false;
    for (auto const& connection : analysis->event_connections) {
        if (connection.detach || connection.target_history != 8
            || connection.targets.empty()) {
            continue;
        }
        auto const target = connection.targets.front().bundle;
        ASSERT_LT(target, analysis->schedule.bundle_to_region.size());
        ASSERT_TRUE(analysis->schedule.bundle_to_region[target].has_value());
        auto const region = *analysis->schedule.bundle_to_region[target];
        ASSERT_LT(region, analysis->schedule.regions.size());
        if (!analysis->schedule.regions[region].cyclic) continue;
        ++retained_inside_connections;
        if (connection.source_type == iv::EventTypeId::trigger
            && connection.target_type == iv::EventTypeId::trigger
            && !connection.requires_conversion) {
            saw_exact = true;
        }
        if (connection.source_type == iv::EventTypeId::boundary
            && connection.target_type == iv::EventTypeId::trigger
            && connection.requires_conversion) {
            ASSERT_EQ(connection.conversion.step_count, 1u);
            EXPECT_EQ(
                connection.conversion.steps[0],
                iv::EventConversionStepId::boundary_to_trigger);
            saw_converted = true;
        }
    }
    EXPECT_EQ(retained_inside_connections, 2u);
    EXPECT_TRUE(saw_exact);
    EXPECT_TRUE(saw_converted);

    auto compiled = compile_graph(feedback_graph, 156);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    RetainedDualEventFeedbackStateMirror* state = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(RetainedDualEventFeedbackStateMirror)) {
            ASSERT_EQ(state, nullptr);
            state = static_cast<RetainedDualEventFeedbackStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(state->calls, 8u);
    EXPECT_EQ(state->scc_feedback_latency, 8u);
    EXPECT_EQ(state->marker, 0xa11ce808u);
    for (std::size_t slice = 0; slice < 8; ++slice) {
        auto const index = slice * 8u;
        EXPECT_EQ(state->indices[slice], index);
        EXPECT_EQ(state->block_sizes[slice], 8u);
        auto const expected_count = slice == 0 ? 1u : 2u;
        auto const expected_first = slice == 0 ? 2u : index - 6u;
        auto const expected_last = index + 2u;
        EXPECT_EQ(state->exact_counts[slice], expected_count);
        EXPECT_EQ(state->converted_counts[slice], expected_count);
        EXPECT_EQ(state->exact_first_times[slice], expected_first);
        EXPECT_EQ(state->converted_first_times[slice], expected_first);
        EXPECT_EQ(state->exact_last_times[slice], expected_last);
        EXPECT_EQ(state->converted_last_times[slice], expected_last);
    }

    // Carry restore happens once at SCC entry. The first slice of the next root
    // call therefore sees the prior event at 58 as history together with the
    // newly authored event at 66, for both exact and converted branches.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 8);
    ASSERT_EQ(state->calls, 9u);
    EXPECT_EQ(state->indices[8], 64u);
    EXPECT_EQ(state->block_sizes[8], 8u);
    EXPECT_EQ(state->exact_counts[8], 2u);
    EXPECT_EQ(state->converted_counts[8], 2u);
    EXPECT_EQ(state->exact_first_times[8], 58u);
    EXPECT_EQ(state->converted_first_times[8], 58u);
    EXPECT_EQ(state->exact_last_times[8], 66u);
    EXPECT_EQ(state->converted_last_times[8], 66u);
}

TEST_F(GraphJitRuntimeFixture, EventRawStorageIsInitializedByNodeStorageLifecycle)
{
    auto feedback_graph = configured_event_feedback_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto compiled = compile_graph(feedback_graph, 141);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    std::vector<iv::NodeLayout::RegionHandle> raw_regions;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.regions.size(); ++i) {
        auto const& region = compiled.compiled_graph->node_layout.regions[i];
        if (region.kind != iv::NodeLayout::Region::Kind::raw) continue;
        EXPECT_NE(region.raw_initialize_fn, nullptr);
        EXPECT_TRUE(region.raw_initialize_data.empty());
        raw_regions.push_back(iv::NodeLayout::RegionHandle{.index = i});
        std::ranges::fill(
            storage.region_bytes(raw_regions.back()), std::byte{0xa5});
    }
    ASSERT_FALSE(raw_regions.empty());

    storage.initialize();
    for (auto const region : raw_regions) {
        auto const bytes = storage.region_bytes(region);
        EXPECT_TRUE(std::ranges::all_of(
            bytes, [](std::byte value) { return value == std::byte{}; }));
    }
}

TEST(GraphJitEventMergeRuntime, KWayMergePreservesSemanticSourceOrder)
{
    auto midi = [](std::uint8_t note) {
        iv::MidiEvent event{};
        event.bytes = {0x90, note, 100};
        event.size = 3;
        return event;
    };
    auto timed = [&](std::uint64_t time, std::uint8_t note) {
        return iv::TimedEvent{
            .time = iv::GlobalIndex::from_bits(time), .value = midi(note)};
    };

    std::array<iv::TimedEvent, 16> target{};
    target[0] = timed(2, 10);
    target[1] = timed(5, 11);
    target[2] = timed(9, 12);
    std::array source_1{
        timed(1, 20),
        timed(5, 21),
        timed(9, 22),
    };
    std::array source_2{
        timed(5, 31),
        timed(7, 32),
        timed(9, 33),
    };
    std::array<void const*, 2> source_events{
        source_1.data(), source_2.data()};
    std::array<std::size_t, 2> remaining{
        source_1.size(), source_2.size()};

    auto const count =
        iv::graph_jit::detail::iv_graph_jit_merge_event_sequences_into_home(
            target.data(),
            target.size(),
            3,
            source_events.data(),
            remaining.data(),
            source_events.size(),
            index(0));

    ASSERT_EQ(count, 9u);
    EXPECT_EQ(remaining[0], 0u);
    EXPECT_EQ(remaining[1], 0u);
    std::array<iv::GlobalIndex, 9> const expected_times{
        index(1), index(2), index(5), index(5), index(5),
        index(7), index(9), index(9), index(9)};
    std::array<std::uint8_t, 9> const expected_notes{
        20, 10, 11, 21, 31, 32, 12, 22, 33};
    for (std::size_t i = 0; i < count; ++i) {
        EXPECT_EQ(target[i].time, expected_times[i]);
        auto const* event = std::get_if<iv::MidiEvent>(&target[i].value);
        ASSERT_NE(event, nullptr);
        ASSERT_GE(event->size, 2u);
        EXPECT_EQ(event->bytes[1], expected_notes[i]);
    }
}

TEST(GraphJitEventMergeRuntime, KWayMergeUsesRangeRelativeGlobalOrder)
{
    auto midi = [](std::uint8_t note) {
        iv::MidiEvent event{};
        event.bytes = {0x90, note, 100};
        event.size = 3;
        return event;
    };
    auto timed = [&](std::int64_t time, std::uint8_t note) {
        return iv::TimedEvent{.time = index(time), .value = midi(note)};
    };

    std::array<iv::TimedEvent, 8> target{};
    target[0] = timed(-2, 10);
    target[1] = timed(1, 11);
    std::array source{timed(-1, 20), timed(0, 21)};
    std::array<void const*, 1> source_events{source.data()};
    std::array<std::size_t, 1> remaining{source.size()};

    auto const count =
        iv::graph_jit::detail::iv_graph_jit_merge_event_sequences_into_home(
            target.data(),
            target.size(),
            2,
            source_events.data(),
            remaining.data(),
            source_events.size(),
            index(-2));

    ASSERT_EQ(count, 4u);
    EXPECT_EQ(remaining[0], 0u);
    std::array<iv::GlobalIndex, 4> const expected_times{
        index(-2), index(-1), index(0), index(1)};
    std::array<std::uint8_t, 4> const expected_notes{10, 20, 21, 11};
    for (std::size_t i = 0; i < count; ++i) {
        EXPECT_EQ(target[i].time, expected_times[i]);
        auto const* event = std::get_if<iv::MidiEvent>(&target[i].value);
        ASSERT_NE(event, nullptr);
        ASSERT_GE(event->size, 2u);
        EXPECT_EQ(event->bytes[1], expected_notes[i]);
    }
}

TEST(GraphJitEventMergeRuntime, StagedMergePreservesSemanticSourceOrder)
{
    auto midi = [](std::uint8_t note) {
        iv::MidiEvent event{};
        event.bytes = {0x90, note, 100};
        event.size = 3;
        return event;
    };
    auto timed = [&](std::uint64_t time, std::uint8_t note) {
        return iv::TimedEvent{
            .time = iv::GlobalIndex::from_bits(time), .value = midi(note)};
    };

    std::array<iv::TimedEvent, 8> target{};
    std::array<std::size_t, 8> source_indices{};
    target[0] = timed(5, 21);
    source_indices[0] = 1;
    std::array semantic_source_0{timed(5, 10)};
    std::array semantic_source_2{timed(5, 32)};

    auto count =
        iv::graph_jit::detail::iv_graph_jit_merge_ordered_event_sequence(
            target.data(),
            source_indices.data(),
            target.size(),
            0,
            1,
            semantic_source_0.data(),
            semantic_source_0.size(),
            semantic_source_0.size(),
            0,
            index(0));
    count = iv::graph_jit::detail::iv_graph_jit_merge_ordered_event_sequence(
        target.data(),
        source_indices.data(),
        target.size(),
        0,
        count,
        semantic_source_2.data(),
        semantic_source_2.size(),
        semantic_source_2.size(),
        2,
        index(0));

    ASSERT_EQ(count, 3u);
    std::array<std::uint8_t, 3> const expected_notes{10, 21, 32};
    std::array<std::size_t, 3> const expected_indices{0, 1, 2};
    for (std::size_t i = 0; i < count; ++i) {
        auto const* event = std::get_if<iv::MidiEvent>(&target[i].value);
        ASSERT_NE(event, nullptr);
        ASSERT_GE(event->size, 2u);
        EXPECT_EQ(event->bytes[1], expected_notes[i]);
        EXPECT_EQ(source_indices[i], expected_indices[i]);
    }
}

TEST(GraphJitEventMergeRuntime, StagedMergePreservesWrappedRingOrder)
{
    auto midi = [](std::uint8_t note) {
        iv::MidiEvent event{};
        event.bytes = {0x90, note, 100};
        event.size = 3;
        return event;
    };
    auto timed = [&](std::uint64_t time, std::uint8_t note) {
        return iv::TimedEvent{
            .time = iv::GlobalIndex::from_bits(time), .value = midi(note)};
    };

    std::array<iv::TimedEvent, 8> ring{};
    std::array<std::size_t, 8> source_indices{};
    constexpr std::size_t read_index = 6;
    ring[6] = timed(3, 30);
    ring[7] = timed(5, 51);
    ring[0] = timed(7, 70);
    source_indices[6] = 2;
    source_indices[7] = 1;
    source_indices[0] = 0;
    std::array source{
        timed(2, 20),
        timed(5, 50),
        timed(8, 80),
    };

    auto const write_index =
        iv::graph_jit::detail::iv_graph_jit_merge_ordered_event_sequence(
            ring.data(),
            source_indices.data(),
            ring.size(),
            read_index,
            read_index + 3,
            source.data(),
            source.size(),
            source.size(),
            0,
            index(0));

    ASSERT_EQ(write_index, read_index + 6);
    std::array<std::uint8_t, 6> const expected_notes{20, 30, 50, 51, 70, 80};
    std::array<std::size_t, 6> const expected_indices{0, 2, 0, 1, 0, 0};
    for (std::size_t i = 0; i < expected_notes.size(); ++i) {
        auto const index = (read_index + i) & (ring.size() - 1);
        auto const* event = std::get_if<iv::MidiEvent>(&ring[index].value);
        ASSERT_NE(event, nullptr);
        ASSERT_GE(event->size, 2u);
        EXPECT_EQ(event->bytes[1], expected_notes[i]);
        EXPECT_EQ(source_indices[index], expected_indices[i]);
    }
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInMergesSlicedSourcesAndFansOut)
{
    auto graph = configured_merged_feed_forward_event_graph(*revision);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_connections.size(), 2u);
    ASSERT_EQ(analysis->event_producer_groups.size(), 1u);
    EXPECT_EQ(analysis->event_connections.front().sources.size(), 2u);
    EXPECT_TRUE(analysis->event_connections.front().requires_conversion);
    ASSERT_TRUE(analysis->event_producer_groups.front().storage_plan);
    EXPECT_EQ(
        analysis->event_producer_groups.front().storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);

    auto compiled = compile_graph(graph, 143);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    // The raw EventOutputPortId sequences and fan-in sequences live in the
    // root stack arena. NodeStorage owns only one overflow counter for each
    // concrete producer output.
    EXPECT_EQ(count_raw_regions(compiled.compiled_graph->node_layout), 2u);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    std::vector<EventConsumerProbeStateMirror*> probes;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            probes.push_back(static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(probes.size(), 2u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    for (auto const* probe : probes) {
        ASSERT_NE(probe, nullptr);
        EXPECT_EQ(probe->calls, 1u);
        EXPECT_EQ(probe->event_count, 10u);
        EXPECT_EQ(probe->trigger_count, 10u);
        EXPECT_EQ(probe->first_time, 3u);
        EXPECT_EQ(probe->last_time, 63u);
    }
}

TEST_F(GraphJitRuntimeFixture,
       OneEventOutputCanParticipateInTwoDifferentFanInSets)
{
    auto graph = configured_overlapping_event_fan_in_graph(*revision);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(*graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_producer_groups.size(), 2u);

    auto compiled = compile_graph(graph, 252);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    std::vector<EventConsumerProbeStateMirror*> probes;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            probes.push_back(static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(probes.size(), 2u);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    for (auto const* probe : probes) {
        ASSERT_NE(probe, nullptr);
        EXPECT_EQ(probe->event_count, 4u);
        EXPECT_EQ(probe->trigger_count, 4u);
        EXPECT_EQ(probe->first_time, 3u);
        EXPECT_EQ(probe->last_time, 63u);
    }
}

TEST_F(GraphJitRuntimeFixture,
       StructuralEventSourcesConvertBeforeTheirCommonFanIn)
{
    auto graph =
        configured_structural_event_source_conversion_fan_in_graph(*revision);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(*graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_connections.size(), 1u);
    auto const& connection = analysis->event_connections.front();
    ASSERT_EQ(connection.source_plans.size(), 2u);
    EXPECT_EQ(connection.source_type, iv::EventTypeId::trigger);
    EXPECT_EQ(connection.conversion.step_count, 0u);
    EXPECT_EQ(connection.source_plans[0].source_type, iv::EventTypeId::midi);
    ASSERT_EQ(connection.source_plans[0].conversion.step_count, 1u);
    EXPECT_EQ(
        connection.source_plans[0].conversion.steps[0],
        iv::EventConversionStepId::midi_to_trigger);
    EXPECT_EQ(
        connection.source_plans[1].source_type,
        iv::EventTypeId::trigger);
    EXPECT_EQ(connection.source_plans[1].conversion.step_count, 0u);

    auto compiled = compile_graph(graph, 253);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    EventConsumerProbeStateMirror* probe = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            probe = static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(probe, nullptr);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    // midi_to_trigger emits only MIDI note-on messages. The MIDI source emits
    // two note-ons and one note-off; the direct source emits two triggers.
    EXPECT_EQ(probe->event_count, 4u);
    EXPECT_EQ(probe->trigger_count, 4u);
    EXPECT_EQ(probe->first_time, 3u);
    EXPECT_EQ(probe->last_time, 63u);
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInUsesPlannedAggregateCapacity)
{
    auto graph = configured_bounded_merged_feed_forward_event_graph(*revision);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_producer_groups.size(), 1u);
    ASSERT_TRUE(analysis->event_producer_groups.front().storage_plan);
    EXPECT_EQ(
        analysis->event_producer_groups.front().storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
    // Lowering selects the fan-in aggregate capacity after it creates the raw
    // EventOutputPortId sequences and merge operations.
    auto compiled = compile_graph(graph, 147);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    EXPECT_EQ(count_raw_regions(compiled.compiled_graph->node_layout), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    EventConsumerProbeStateMirror* probe = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            probe = static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(probe, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);

    // Each source declares 0.125 events/sample, so the 64-frame sizing input
    // reserves eight events per producer. Source 0 emits eight and source 1
    // emits four; runtime correctness depends only on the resulting storage
    // aggregate capacity, not on a separate per-producer write quota.
    EXPECT_EQ(probe->calls, 1u);
    EXPECT_EQ(probe->event_count, 12u);
    EXPECT_EQ(probe->trigger_count, 12u);
    EXPECT_EQ(probe->first_time, 3u);
    EXPECT_EQ(probe->last_time, 3u);
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInRejectsUnavoidableLocalStackOverBudget)
{
    auto graph = configured_bounded_merged_feed_forward_event_graph(*revision);
    ASSERT_TRUE(graph);

    // Connection analysis does not create raw EventOutputPortId sequences or
    // fan-in sequences. The hard stack failure belongs to lowering, where
    // those concrete allocations exist.
    auto analysis = build_connection_analysis_plan(
        *graph,
        64,
        iv::RealtimeStorageCostModel{
            .stack_budget_bytes = 7u * sizeof(iv::TimedEvent),
        });
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());

    iv::GraphJit budgeted_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .realtime_storage_cost_model = iv::RealtimeStorageCostModel{
            .stack_budget_bytes = 7u * sizeof(iv::TimedEvent),
        },
    });
    auto compiled = budgeted_jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 212,
        .graph = graph,
        .definitions = definitions,
    });
    ASSERT_FALSE(compiled.succeeded());
    ASSERT_FALSE(compiled.diagnostics.empty());
    EXPECT_NE(
        compiled.diagnostics.front().message.find(
            "packed realtime stack requires"),
        std::string::npos)
        << compiled.diagnostics.front().message;
    EXPECT_NE(
        compiled.diagnostics.front().message.find(
            "after all eligible buffers were moved to NodeStorage"),
        std::string::npos)
        << compiled.diagnostics.front().message;
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInConvertsAfterMerge)
{
    auto graph = configured_merged_converted_event_graph(*revision);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_producer_groups.size(), 1u);
    EXPECT_EQ(analysis->event_producer_groups.front().sources.size(), 2u);
    ASSERT_EQ(analysis->event_connections.size(), 2u);
    for (auto const& connection : analysis->event_connections) {
        EXPECT_EQ(connection.source_type, iv::EventTypeId::midi);
        EXPECT_EQ(connection.target_type, iv::EventTypeId::trigger);
        ASSERT_EQ(connection.conversion.step_count, 1u);
        EXPECT_EQ(
            connection.conversion.steps[0],
            iv::EventConversionStepId::midi_to_trigger);
    }

    auto compiled = compile_graph(graph, 144);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    // The raw EventOutputPortId sequences, fan-in sequence, and converted
    // target sequence are transient stack allocations. NodeStorage owns only
    // the two concrete producer overflow counters.
    EXPECT_EQ(count_raw_regions(compiled.compiled_graph->node_layout), 2u);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    std::vector<EventConsumerProbeStateMirror*> probes;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            probes.push_back(static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(probes.size(), 2u);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    for (auto const* probe : probes) {
        EXPECT_EQ(probe->event_count, 4u);
        EXPECT_EQ(probe->trigger_count, 4u);
        EXPECT_EQ(probe->first_time, 5u);
        EXPECT_EQ(probe->last_time, 17u);
    }
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInTargetHistoryUsesRetainedFanIn)
{
    auto graph = configured_merged_target_history_event_graph(*revision);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_producer_groups.size(), 1u);
    auto const& group = analysis->event_producer_groups.front();
    ASSERT_TRUE(group.storage_plan);
    EXPECT_EQ(
        group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    // Connection analysis carries only the root-call and retained event bounds.
    // Merge copies and raw callback stack buffers are costed from the concrete
    // fan-in operations during lowering.
    EXPECT_EQ(group.storage_requirements.operations.invariant_copied_values, 0u);
    EXPECT_EQ(group.storage_requirements.operations.carry_extra_copied_values, 0u);

    auto compiled = compile_graph(graph, 148);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    RetainedEventConsumerProbeStateMirror* probe = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(RetainedEventConsumerProbeStateMirror)) {
            probe = static_cast<RetainedEventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(probe, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(probe->event_counts[0], 4u);
    EXPECT_EQ(probe->first_times[0], 3u);
    EXPECT_EQ(probe->second_times[0], 3u);
    EXPECT_EQ(probe->last_times[0], 63u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(probe->event_counts[1], 6u);
    EXPECT_EQ(probe->first_times[1], 63u);
    EXPECT_EQ(probe->second_times[1], 63u);
    EXPECT_EQ(probe->last_times[1], 127u);
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInCompactCarryMigrates)
{
    auto graph = configured_merged_retained_event_graph(*revision, false);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_producer_groups.size(), 1u);
    ASSERT_TRUE(analysis->event_producer_groups.front().storage_plan);
    EXPECT_EQ(
        analysis->event_producer_groups.front().storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    // Each callback output has its own retained canonical sequence. The
    // fan-in carry is a separate derived sequence for the consumer input.
    auto compiled = compile_graph(graph, 145);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& state_requirements = compiled.compiled_graph
        ->realtime_port_state_requirements.states;
    auto const& state_realizations = compiled.compiled_graph
        ->realtime_port_state_realizations.event_states;
    ASSERT_EQ(state_requirements.size(), 3u);
    ASSERT_EQ(state_realizations.size(), 3u);
    std::size_t output_state_count = 0;
    std::size_t input_state_count = 0;
    for (auto const& realization : state_realizations) {
        ASSERT_LT(realization.requirement_index, state_requirements.size());
        auto const& requirement =
            state_requirements[realization.requirement_index];
        ASSERT_TRUE(realization.retained_storage);
        EXPECT_EQ(
            realization.retained_storage->storage,
            iv::graph_jit::EventRealtimePortStateStorage::compact_carry);
        EXPECT_FALSE(realization.retained_storage->has_source_indices);
        EXPECT_EQ(
            realization.retained_access,
            iv::graph_jit::EventRealtimePortStateAccess::direct);
        EXPECT_FALSE(realization.retained_source_index);
        EXPECT_FALSE(realization.materialization_conversion);
        if (requirement.direction
            == iv::graph_jit::PortDirection::output) {
            ++output_state_count;
        } else {
            ++input_state_count;
        }
    }
    EXPECT_EQ(output_state_count, 2u);
    EXPECT_EQ(input_state_count, 1u);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    RetainedEventConsumerProbeStateMirror* probe = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(RetainedEventConsumerProbeStateMirror)) {
            probe = static_cast<RetainedEventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(probe, nullptr);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(probe->event_counts[0], 4u);
    EXPECT_EQ(probe->first_times[0], 5u);
    EXPECT_EQ(probe->second_times[0], 5u);
    EXPECT_EQ(probe->last_times[0], 61u);

    auto recompiled = compile_graph(graph, 146);
    ASSERT_TRUE(recompiled.succeeded())
        << (recompiled.diagnostics.empty()
                ? ""
                : recompiled.diagnostics.front().message);
    auto migrated =
        recompiled.compiled_graph->node_layout.create_storage(resources);
    migrated.initialize(&storage);
    RetainedEventConsumerProbeStateMirror* migrated_probe = nullptr;
    for (std::size_t i = 0;
         i < recompiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (recompiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(RetainedEventConsumerProbeStateMirror)) {
            migrated_probe = static_cast<RetainedEventConsumerProbeStateMirror*>(
                migrated.state_ptr(i));
        }
    }
    ASSERT_NE(migrated_probe, nullptr);
    recompiled.compiled_graph->root_operations.tick_block(
        migrated.buffer().data(), &empty_tick_invocation, index(64), 64);
    // The compiler-owned carry migrates; this authored probe state starts fresh
    // in the new NodeStorage generation, so the resumed observation is index 0.
    EXPECT_EQ(migrated_probe->calls, 1u);
    EXPECT_EQ(migrated_probe->event_counts[0], 8u);
    EXPECT_EQ(migrated_probe->first_times[0], 61u);
    EXPECT_EQ(migrated_probe->second_times[0], 61u);
    EXPECT_EQ(migrated_probe->last_times[0], 125u);
}

TEST_F(GraphJitRuntimeFixture, DisconnectedTickEventOutputRetainsItsLatency)
{
    auto graph = configured_disconnected_retained_event_output_graph(*revision);
    ASSERT_TRUE(graph);
    auto compiled = compile_graph(graph, 249);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto const& requirements = compiled.compiled_graph
        ->realtime_port_state_requirements.states;
    auto const& realizations = compiled.compiled_graph
        ->realtime_port_state_realizations.event_states;
    ASSERT_EQ(requirements.size(), 1u);
    ASSERT_EQ(realizations.size(), 1u);
    auto const& realization = realizations.front();
    ASSERT_LT(realization.requirement_index, requirements.size());
    auto const& requirement = requirements[realization.requirement_index];
    EXPECT_EQ(requirement.direction, iv::graph_jit::PortDirection::output);
    EXPECT_EQ(
        requirement.role,
        iv::graph_jit::RealtimePortStateRole::tick_output_latency);
    EXPECT_EQ(requirement.extent_samples, 8u);
    EXPECT_EQ(
        realization.authored_storage.storage,
        iv::graph_jit::EventRealtimePortStateStorage::callback_transient);
    ASSERT_TRUE(realization.retained_storage);
    EXPECT_EQ(
        realization.retained_storage->storage,
        iv::graph_jit::EventRealtimePortStateStorage::compact_carry);
    EXPECT_EQ(
        realization.retained_access,
        iv::graph_jit::EventRealtimePortStateAccess::direct);
    EXPECT_FALSE(realization.retained_source_index);
    EXPECT_FALSE(realization.retained_storage->has_source_indices);
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInPersistentRingRetainsBursts)
{
    auto graph = configured_merged_retained_event_graph(*revision, true);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_producer_groups.size(), 1u);
    ASSERT_TRUE(analysis->event_producer_groups.front().storage_plan);
    EXPECT_EQ(
        analysis->event_producer_groups.front().storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    auto compiled = compile_graph(graph, 147);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    PersistentEventRingConsumerProbeStateMirror* probe = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(PersistentEventRingConsumerProbeStateMirror)) {
            probe = static_cast<PersistentEventRingConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(probe, nullptr);
    for (std::uint64_t index : {0u, 64u, 128u, 192u}) {
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(),
            &empty_tick_invocation,
            iv::GlobalIndex::from_bits(index),
            64);
    }
    EXPECT_EQ(probe->event_counts[0], 64u);
    EXPECT_EQ(probe->event_counts[1], 128u);
    EXPECT_EQ(probe->event_counts[2], 192u);
    EXPECT_EQ(probe->event_counts[3], 192u);
    EXPECT_EQ(probe->first_times[3], 69u);
    EXPECT_EQ(probe->last_times[3], 197u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccFansOutToAcyclicConsumer)
{
    auto feedback_graph =
        configured_event_feedback_scc_external_fanout_graph(*revision);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [&](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            if (connection.detach || connection.sources.empty()
                || connection.targets.empty()) {
                return false;
            }
            auto const source = connection.sources.front().bundle;
            auto const target = connection.targets.front().bundle;
            if (source >= analysis->schedule.bundle_to_region.size()
                || target >= analysis->schedule.bundle_to_region.size()
                || !analysis->schedule.bundle_to_region[source]
                || !analysis->schedule.bundle_to_region[target]) {
                return false;
            }
            auto const source_region =
                *analysis->schedule.bundle_to_region[source];
            auto const target_region =
                *analysis->schedule.bundle_to_region[target];
            return source_region < analysis->schedule.regions.size()
                && target_region < analysis->schedule.regions.size()
                && analysis->schedule.regions[source_region].cyclic
                && !analysis->schedule.regions[target_region].cyclic;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_TRUE(fanout->requires_block_materialization);

    auto compiled = compile_graph(feedback_graph, 142);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventConsumerProbeStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(observer->calls, 1u);
    EXPECT_EQ(observer->last_index, 0u);
    EXPECT_EQ(observer->last_block_size, 64u);
    EXPECT_EQ(observer->event_count, 8u);
    EXPECT_EQ(observer->trigger_count, 8u);
    EXPECT_EQ(observer->first_time, 1u);
    EXPECT_EQ(observer->last_time, 57u);
    EXPECT_EQ(observer->marker, 0xe71e17u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 8);
    EXPECT_EQ(observer->calls, 2u);
    EXPECT_EQ(observer->last_index, 64u);
    EXPECT_EQ(observer->last_block_size, 8u);
    EXPECT_EQ(observer->event_count, 1u);
    EXPECT_EQ(observer->trigger_count, 1u);
    EXPECT_EQ(observer->first_time, 65u);
    EXPECT_EQ(observer->last_time, 65u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccRetainsAuthoredFutureEvents)
{
    auto feedback_graph = configured_event_feedback_scc_external_fanout_graph(
        *revision,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        graph_jit_latent_event_feedback_a_id,
        graph_jit_event_feedback_b_id);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());

    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [&](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            if (connection.detach || connection.sources.empty()
                || connection.targets.empty()) {
                return false;
            }
            auto const source = connection.sources.front().bundle;
            auto const target = connection.targets.front().bundle;
            if (source >= analysis->schedule.bundle_to_region.size()
                || target >= analysis->schedule.bundle_to_region.size()
                || !analysis->schedule.bundle_to_region[source]
                || !analysis->schedule.bundle_to_region[target]) {
                return false;
            }
            auto const source_region =
                *analysis->schedule.bundle_to_region[source];
            auto const target_region =
                *analysis->schedule.bundle_to_region[target];
            return source_region < analysis->schedule.regions.size()
                && target_region < analysis->schedule.regions.size()
                && analysis->schedule.regions[source_region].cyclic
                && !analysis->schedule.regions[target_region].cyclic;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_EQ(fanout->source_history, 0u);
    EXPECT_EQ(fanout->source_latency, 16u);
    EXPECT_EQ(fanout->target_history, 0u);
    EXPECT_TRUE(fanout->requires_block_materialization);

    auto const detached = std::ranges::find_if(
        analysis->event_connections,
        [](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            return connection.detach.has_value();
        });
    ASSERT_NE(detached, analysis->event_connections.end());
    EXPECT_EQ(detached->source_history, 0u);
    EXPECT_EQ(detached->source_latency, 16u);
    EXPECT_EQ(detached->target_history, 0u);

    auto const fanout_index = static_cast<std::size_t>(
        std::distance(analysis->event_connections.begin(), fanout));
    auto const group = std::ranges::find_if(
        analysis->event_producer_groups,
        [&](iv::graph_jit::detail::EventProducerGroupPlan const& candidate) {
            return std::ranges::find(
                       candidate.connection_indices, fanout_index)
                != candidate.connection_indices.end();
        });
    ASSERT_NE(group, analysis->event_producer_groups.end());
    ASSERT_TRUE(group->storage_plan.has_value());
    EXPECT_EQ(
        group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(group->storage_requirements.retained_window_samples, 16u);

    auto compiled = compile_graph(feedback_graph, 151);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventConsumerProbeStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    std::optional<iv::NodeLayout::RegionHandle> feedback_carry;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.regions.size(); ++i) {
        auto const& region = compiled.compiled_graph->node_layout.regions[i];
        if (region.kind == iv::NodeLayout::Region::Kind::raw
            && region.migration_identity.starts_with(
                "graphjit.event.feedback:")) {
            ASSERT_FALSE(feedback_carry.has_value());
            EXPECT_NE(
                region.migration_identity.find("kind=compact_carry"),
                std::string::npos);
            feedback_carry = iv::NodeLayout::RegionHandle{.index = i};
        }
    }
    ASSERT_TRUE(feedback_carry.has_value());
    auto const feedback_carry_count = [&]() {
        auto const bytes = storage.region_bytes(*feedback_carry);
        EXPECT_GE(bytes.size(), sizeof(std::size_t));
        std::size_t value = 0;
        if (bytes.size() >= sizeof(std::size_t)) {
            std::memcpy(&value, bytes.data(), sizeof(value));
        }
        return value;
    };

    // The eight SCC slices author 9,17,...,65. Delaying those events by ten
    // samples leaves exactly 67 and 75 live across the root boundary, so the
    // feedback stream uses a two-event compact carry here.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(observer->calls, 1u);
    EXPECT_EQ(observer->last_index, 0u);
    EXPECT_EQ(observer->last_block_size, 64u);
    EXPECT_EQ(observer->event_count, 7u);
    EXPECT_EQ(observer->trigger_count, 7u);
    EXPECT_EQ(observer->first_time, 9u);
    EXPECT_EQ(observer->last_time, 57u);
    EXPECT_EQ(feedback_carry_count(), 2u);

    // Source carry restore prepends the previously authored event at 65. New SCC
    // slices then append 73,81,...,129. Feedback restore independently brings
    // back delayed events 67 and 75; cursor initializeing prevents source event 65
    // from being delayed and enqueued a second time.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(observer->calls, 2u);
    EXPECT_EQ(observer->last_index, 64u);
    EXPECT_EQ(observer->last_block_size, 64u);
    EXPECT_EQ(observer->event_count, 8u);
    EXPECT_EQ(observer->trigger_count, 8u);
    EXPECT_EQ(observer->first_time, 65u);
    EXPECT_EQ(observer->last_time, 121u);
    EXPECT_EQ(feedback_carry_count(), 2u);

    // A short root call restores source event 129 and authors 137. The delayed
    // feedback carry still retains exactly the two events live beyond the new
    // root boundary.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(128), 8);
    EXPECT_EQ(observer->calls, 3u);
    EXPECT_EQ(observer->last_index, 128u);
    EXPECT_EQ(observer->last_block_size, 8u);
    EXPECT_EQ(observer->event_count, 1u);
    EXPECT_EQ(observer->trigger_count, 1u);
    EXPECT_EQ(observer->first_time, 129u);
    EXPECT_EQ(observer->last_time, 129u);
    EXPECT_EQ(feedback_carry_count(), 2u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccPersistentRingRetainsAuthoredFutureEvents)
{
    auto feedback_graph = configured_event_feedback_scc_external_fanout_graph(
        *revision,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        graph_jit_persistent_latent_event_feedback_a_id,
        graph_jit_event_feedback_b_id);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());

    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [&](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            if (connection.detach || connection.sources.empty()
                || connection.targets.empty()) {
                return false;
            }
            auto const source = connection.sources.front().bundle;
            auto const target = connection.targets.front().bundle;
            if (source >= analysis->schedule.bundle_to_region.size()
                || target >= analysis->schedule.bundle_to_region.size()
                || !analysis->schedule.bundle_to_region[source]
                || !analysis->schedule.bundle_to_region[target]) {
                return false;
            }
            auto const source_region =
                *analysis->schedule.bundle_to_region[source];
            auto const target_region =
                *analysis->schedule.bundle_to_region[target];
            return source_region < analysis->schedule.regions.size()
                && target_region < analysis->schedule.regions.size()
                && analysis->schedule.regions[source_region].cyclic
                && !analysis->schedule.regions[target_region].cyclic;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_EQ(fanout->source_history, 0u);
    EXPECT_EQ(fanout->source_latency, 320u);
    EXPECT_EQ(fanout->target_history, 0u);
    EXPECT_TRUE(fanout->requires_block_materialization);

    auto const fanout_index = static_cast<std::size_t>(
        std::distance(analysis->event_connections.begin(), fanout));
    auto const group = std::ranges::find_if(
        analysis->event_producer_groups,
        [&](iv::graph_jit::detail::EventProducerGroupPlan const& candidate) {
            return std::ranges::find(
                       candidate.connection_indices, fanout_index)
                != candidate.connection_indices.end();
        });
    ASSERT_NE(group, analysis->event_producer_groups.end());
    ASSERT_TRUE(group->storage_plan.has_value());
    EXPECT_EQ(
        group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(group->storage_requirements.retained_window_samples, 320u);
    EXPECT_GE(
        group->storage_requirements.retained_event_capacity,
        group->storage_requirements.current_event_capacity);

    auto compiled = compile_graph(feedback_graph, 152);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventConsumerProbeStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    std::optional<iv::NodeLayout::RegionHandle> source_ring;
    std::optional<iv::NodeLayout::RegionHandle> feedback_ring;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.regions.size(); ++i) {
        auto const& region = compiled.compiled_graph->node_layout.regions[i];
        if (region.kind != iv::NodeLayout::Region::Kind::raw) continue;
        if (region.migration_identity.starts_with("graphjit.event.feedback:")) {
            ASSERT_FALSE(feedback_ring.has_value());
            feedback_ring = iv::NodeLayout::RegionHandle{.index = i};
        } else if (region.migration_identity.find("kind=persistent_ring")
                       != std::string::npos
                   && region.migration_identity.find("latency=320")
                       != std::string::npos) {
            ASSERT_FALSE(source_ring.has_value());
            source_ring = iv::NodeLayout::RegionHandle{.index = i};
        }
    }
    ASSERT_TRUE(source_ring.has_value());
    ASSERT_TRUE(feedback_ring.has_value());

    auto const indices = [&](iv::NodeLayout::RegionHandle region) {
        auto const bytes = storage.region_bytes(region);
        EXPECT_GE(bytes.size(), 2 * sizeof(std::size_t));
        std::array<std::size_t, 2> value{};
        if (bytes.size() >= 2 * sizeof(std::size_t)) {
            std::memcpy(value.data(), bytes.data(), 2 * sizeof(std::size_t));
        }
        return value;
    };

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(observer->calls, 1u);
    EXPECT_EQ(observer->last_index, 0u);
    EXPECT_EQ(observer->last_block_size, 64u);
    EXPECT_EQ(observer->event_count, 7u);
    EXPECT_EQ(observer->trigger_count, 7u);
    EXPECT_EQ(observer->first_time, 9u);
    EXPECT_EQ(observer->last_time, 57u);
    EXPECT_EQ(indices(*source_ring)[0], 0u);
    EXPECT_EQ(indices(*source_ring)[1], 8u);
    EXPECT_EQ(indices(*feedback_ring)[1], 8u);

    // Root-entry pruning removes events before 64 but keeps the previously
    // authored future event at 65. The root-call feedback cursor starts at the
    // source ring's prior write index, so that retained event is not copied a
    // second time into detached feedback.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(observer->calls, 2u);
    EXPECT_EQ(observer->last_index, 64u);
    EXPECT_EQ(observer->last_block_size, 64u);
    EXPECT_EQ(observer->event_count, 8u);
    EXPECT_EQ(observer->trigger_count, 8u);
    EXPECT_EQ(observer->first_time, 65u);
    EXPECT_EQ(observer->last_time, 121u);
    EXPECT_EQ(indices(*source_ring)[0], 7u);
    EXPECT_EQ(indices(*source_ring)[1], 16u);
    EXPECT_EQ(indices(*feedback_ring)[1], 16u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(128), 8);
    EXPECT_EQ(observer->calls, 3u);
    EXPECT_EQ(observer->last_index, 128u);
    EXPECT_EQ(observer->last_block_size, 8u);
    EXPECT_EQ(observer->event_count, 1u);
    EXPECT_EQ(observer->trigger_count, 1u);
    EXPECT_EQ(observer->first_time, 129u);
    EXPECT_EQ(observer->last_time, 129u);
    EXPECT_EQ(indices(*source_ring)[0], 15u);
    EXPECT_EQ(indices(*source_ring)[1], 17u);
    EXPECT_EQ(indices(*feedback_ring)[1], 17u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccConvertsFanoutToAcyclicConsumer)
{
    auto feedback_graph = configured_event_feedback_scc_external_fanout_graph(
        *revision,
        "iv.test.graph_jit.state_context.trigger_event_consumer",
        graph_jit_boundary_event_feedback_a_id,
        graph_jit_boundary_event_feedback_b_id);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            return !connection.detach
                && connection.source_type == iv::EventTypeId::boundary
                && connection.target_type == iv::EventTypeId::trigger;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_TRUE(fanout->requires_conversion);
    EXPECT_TRUE(fanout->requires_block_materialization);
    ASSERT_EQ(fanout->conversion.step_count, 1u);
    EXPECT_EQ(
        fanout->conversion.steps[0],
        iv::EventConversionStepId::boundary_to_trigger);

    auto const group = std::ranges::find_if(
        analysis->event_producer_groups,
        [&](iv::graph_jit::detail::EventProducerGroupPlan const& candidate) {
            return std::ranges::find(
                       candidate.connection_indices,
                       static_cast<std::size_t>(
                           std::distance(
                               analysis->event_connections.begin(), fanout)))
                != candidate.connection_indices.end();
        });
    ASSERT_NE(group, analysis->event_producer_groups.end());
    ASSERT_TRUE(group->storage_plan.has_value());
    EXPECT_EQ(
        group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);

    auto compiled = compile_graph(feedback_graph, 148);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    EventConsumerProbeStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(EventConsumerProbeStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<EventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(observer->calls, 1u);
    EXPECT_EQ(observer->last_index, 0u);
    EXPECT_EQ(observer->last_block_size, 64u);
    EXPECT_EQ(observer->event_count, 8u);
    EXPECT_EQ(observer->trigger_count, 8u);
    EXPECT_EQ(observer->first_time, 1u);
    EXPECT_EQ(observer->last_time, 57u);
    EXPECT_EQ(observer->marker, 0xe71e17u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 8);
    EXPECT_EQ(observer->calls, 2u);
    EXPECT_EQ(observer->last_index, 64u);
    EXPECT_EQ(observer->last_block_size, 8u);
    EXPECT_EQ(observer->event_count, 1u);
    EXPECT_EQ(observer->trigger_count, 1u);
    EXPECT_EQ(observer->first_time, 65u);
    EXPECT_EQ(observer->last_time, 65u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccRetainsOutboundTargetHistory)
{
    auto feedback_graph = configured_event_feedback_scc_external_fanout_graph(
        *revision,
        "iv.test.graph_jit.state_context.retained_trigger_event_consumer");
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [&](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            if (connection.detach || connection.target_history != 8
                || connection.sources.empty() || connection.targets.empty()) {
                return false;
            }
            auto const source = connection.sources.front().bundle;
            auto const target = connection.targets.front().bundle;
            if (source >= analysis->schedule.bundle_to_region.size()
                || target >= analysis->schedule.bundle_to_region.size()
                || !analysis->schedule.bundle_to_region[source]
                || !analysis->schedule.bundle_to_region[target]) {
                return false;
            }
            auto const source_region =
                *analysis->schedule.bundle_to_region[source];
            auto const target_region =
                *analysis->schedule.bundle_to_region[target];
            return source_region < analysis->schedule.regions.size()
                && target_region < analysis->schedule.regions.size()
                && analysis->schedule.regions[source_region].cyclic
                && !analysis->schedule.regions[target_region].cyclic;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_EQ(fanout->source_history, 0u);
    EXPECT_EQ(fanout->source_latency, 0u);
    EXPECT_EQ(fanout->target_history, 8u);
    EXPECT_TRUE(fanout->requires_block_materialization);

    auto const fanout_index = static_cast<std::size_t>(
        std::distance(analysis->event_connections.begin(), fanout));
    auto const group = std::ranges::find_if(
        analysis->event_producer_groups,
        [&](iv::graph_jit::detail::EventProducerGroupPlan const& candidate) {
            return std::ranges::find(
                       candidate.connection_indices, fanout_index)
                != candidate.connection_indices.end();
        });
    ASSERT_NE(group, analysis->event_producer_groups.end());
    ASSERT_TRUE(group->storage_plan.has_value());
    EXPECT_EQ(
        group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(group->storage_requirements.retained_window_samples, 8u);

    auto compiled = compile_graph(feedback_graph, 149);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    RetainedEventConsumerProbeStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            compiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(RetainedEventConsumerProbeStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<RetainedEventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    std::optional<iv::NodeLayout::RegionHandle> feedback_carry;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.regions.size(); ++i) {
        auto const& region = compiled.compiled_graph->node_layout.regions[i];
        if (region.kind == iv::NodeLayout::Region::Kind::raw
            && region.migration_identity.starts_with(
                "graphjit.event.feedback:")) {
            ASSERT_FALSE(feedback_carry.has_value());
            EXPECT_NE(
                region.migration_identity.find("kind=compact_carry"),
                std::string::npos);
            feedback_carry = iv::NodeLayout::RegionHandle{.index = i};
        }
    }
    ASSERT_TRUE(feedback_carry.has_value());
    auto const feedback_carry_count = [&]() {
        auto const bytes = storage.region_bytes(*feedback_carry);
        EXPECT_GE(bytes.size(), sizeof(std::size_t));
        std::size_t value = 0;
        if (bytes.size() >= sizeof(std::size_t)) {
            std::memcpy(&value, bytes.data(), sizeof(value));
        }
        return value;
    };

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(observer->calls, 1u);
    EXPECT_EQ(observer->indices[0], 0u);
    EXPECT_EQ(observer->event_counts[0], 8u);
    EXPECT_EQ(observer->first_times[0], 1u);
    EXPECT_EQ(observer->second_times[0], 9u);
    EXPECT_EQ(observer->last_times[0], 57u);
    EXPECT_EQ(feedback_carry_count(), 1u);

    // The carry retains [56, 64), so event 57 becomes history for the next
    // root call. The SCC then appends eight current events before one root-exit
    // materialization selects [56, 128).
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    ASSERT_EQ(observer->calls, 2u);
    EXPECT_EQ(observer->indices[1], 64u);
    EXPECT_EQ(observer->event_counts[1], 9u);
    EXPECT_EQ(observer->first_times[1], 57u);
    EXPECT_EQ(observer->second_times[1], 65u);
    EXPECT_EQ(observer->last_times[1], 121u);

    // The restored event 57 is historical context, not newly authored output.
    // Detached feedback retains only the one delayed event that lies beyond
    // this root boundary; restored source history is never re-enqueued.
    EXPECT_EQ(feedback_carry_count(), 1u);

    // Changing the root-call size still uses the root history window, not an
    // SCC-slice-sized window. Only prior event 121 and current event 129 fit.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(128), 8);
    ASSERT_EQ(observer->calls, 3u);
    EXPECT_EQ(observer->indices[2], 128u);
    EXPECT_EQ(observer->event_counts[2], 2u);
    EXPECT_EQ(observer->first_times[2], 121u);
    EXPECT_EQ(observer->second_times[2], 129u);
    EXPECT_EQ(observer->last_times[2], 129u);
    EXPECT_EQ(feedback_carry_count(), 1u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccComposesLatencyHistoryAndConversion)
{
    auto feedback_graph = configured_event_feedback_scc_external_fanout_graph(
        *revision,
        "iv.test.graph_jit.state_context.persistent_event_feedback_consumer",
        graph_jit_persistent_latent_boundary_event_feedback_a_id,
        graph_jit_boundary_event_feedback_b_id);
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());

    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [&](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            if (connection.detach || connection.sources.empty()
                || connection.targets.empty()) {
                return false;
            }
            auto const source = connection.sources.front().bundle;
            auto const target = connection.targets.front().bundle;
            if (source >= analysis->schedule.bundle_to_region.size()
                || target >= analysis->schedule.bundle_to_region.size()
                || !analysis->schedule.bundle_to_region[source]
                || !analysis->schedule.bundle_to_region[target]) {
                return false;
            }
            auto const source_region =
                *analysis->schedule.bundle_to_region[source];
            auto const target_region =
                *analysis->schedule.bundle_to_region[target];
            return source_region < analysis->schedule.regions.size()
                && target_region < analysis->schedule.regions.size()
                && analysis->schedule.regions[source_region].cyclic
                && !analysis->schedule.regions[target_region].cyclic;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_EQ(fanout->source_type, iv::EventTypeId::boundary);
    EXPECT_EQ(fanout->target_type, iv::EventTypeId::trigger);
    EXPECT_EQ(fanout->source_history, 0u);
    EXPECT_EQ(fanout->source_latency, 320u);
    EXPECT_EQ(fanout->target_history, 320u);
    EXPECT_TRUE(fanout->requires_conversion);
    EXPECT_TRUE(fanout->requires_block_materialization);
    ASSERT_EQ(fanout->conversion.step_count, 1u);
    EXPECT_EQ(
        fanout->conversion.steps[0],
        iv::EventConversionStepId::boundary_to_trigger);

    auto const fanout_index = static_cast<std::size_t>(
        std::distance(analysis->event_connections.begin(), fanout));
    auto const group = std::ranges::find_if(
        analysis->event_producer_groups,
        [&](iv::graph_jit::detail::EventProducerGroupPlan const& candidate) {
            return std::ranges::find(
                       candidate.connection_indices, fanout_index)
                != candidate.connection_indices.end();
        });
    ASSERT_NE(group, analysis->event_producer_groups.end());
    ASSERT_TRUE(group->storage_plan.has_value());
    EXPECT_EQ(
        group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    // The canonical producer timeline must retain both sides of the root
    // window: 320 samples of target history behind it and 320 samples of
    // authored source latency ahead of it.
    EXPECT_EQ(group->storage_requirements.retained_window_samples, 640u);
    EXPECT_GE(
        group->storage_requirements.retained_event_capacity,
        group->storage_requirements.current_event_capacity);

    auto compiled = compile_graph(feedback_graph, 153);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    PersistentEventFeedbackConsumerStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(PersistentEventFeedbackConsumerStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<PersistentEventFeedbackConsumerStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    std::optional<iv::NodeLayout::RegionHandle> source_ring;
    std::optional<iv::NodeLayout::RegionHandle> feedback_ring;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.regions.size(); ++i) {
        auto const& region = compiled.compiled_graph->node_layout.regions[i];
        if (region.kind != iv::NodeLayout::Region::Kind::raw) continue;
        if (region.migration_identity.starts_with("graphjit.event.feedback:")) {
            ASSERT_FALSE(feedback_ring.has_value());
            feedback_ring = iv::NodeLayout::RegionHandle{.index = i};
        } else if (region.migration_identity.find("kind=persistent_ring")
                       != std::string::npos
                   && region.migration_identity.find("latency=320")
                       != std::string::npos) {
            ASSERT_FALSE(source_ring.has_value());
            source_ring = iv::NodeLayout::RegionHandle{.index = i};
        }
    }
    ASSERT_TRUE(source_ring.has_value());
    ASSERT_TRUE(feedback_ring.has_value());

    auto const indices = [&](iv::NodeLayout::RegionHandle region) {
        auto const bytes = storage.region_bytes(region);
        EXPECT_GE(bytes.size(), 2 * sizeof(std::size_t));
        std::array<std::size_t, 2> value{};
        if (bytes.size() >= 2 * sizeof(std::size_t)) {
            std::memcpy(value.data(), bytes.data(), 2 * sizeof(std::size_t));
        }
        return value;
    };

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(observer->calls, 1u);
    EXPECT_EQ(observer->indices[0], 0u);
    EXPECT_EQ(observer->event_counts[0], 7u);
    EXPECT_EQ(observer->first_times[0], 9u);
    EXPECT_EQ(observer->last_times[0], 57u);
    EXPECT_EQ(observer->marker, 0xfeed320u);
    EXPECT_EQ(indices(*source_ring)[0], 0u);
    EXPECT_EQ(indices(*source_ring)[1], 8u);
    EXPECT_EQ(indices(*feedback_ring)[1], 8u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    ASSERT_EQ(observer->calls, 2u);
    EXPECT_EQ(observer->indices[1], 64u);
    EXPECT_EQ(observer->event_counts[1], 15u);
    EXPECT_EQ(observer->first_times[1], 9u);
    EXPECT_EQ(observer->last_times[1], 121u);
    EXPECT_EQ(indices(*source_ring)[0], 0u);
    EXPECT_EQ(indices(*source_ring)[1], 16u);
    EXPECT_EQ(indices(*feedback_ring)[1], 16u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(128), 64);
    ASSERT_EQ(observer->calls, 3u);
    EXPECT_EQ(observer->indices[2], 128u);
    EXPECT_EQ(observer->event_counts[2], 23u);
    EXPECT_EQ(observer->first_times[2], 9u);
    EXPECT_EQ(observer->last_times[2], 185u);
    EXPECT_EQ(indices(*source_ring)[0], 0u);
    EXPECT_EQ(indices(*source_ring)[1], 24u);
    EXPECT_EQ(indices(*feedback_ring)[1], 24u);
}

TEST_F(GraphJitRuntimeFixture, EventFeedbackSccPersistentRingRetainsOutboundTargetHistory)
{
    auto feedback_graph = configured_event_feedback_scc_external_fanout_graph(
        *revision,
        "iv.test.graph_jit.state_context.persistent_event_feedback_consumer");
    ASSERT_TRUE(feedback_graph);

    auto analysis = build_connection_analysis_plan(
        *feedback_graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    auto const fanout = std::ranges::find_if(
        analysis->event_connections,
        [&](iv::graph_jit::detail::EventConnectionPlan const& connection) {
            if (connection.detach || connection.target_history != 320
                || connection.sources.empty() || connection.targets.empty()) {
                return false;
            }
            auto const source = connection.sources.front().bundle;
            auto const target = connection.targets.front().bundle;
            if (source >= analysis->schedule.bundle_to_region.size()
                || target >= analysis->schedule.bundle_to_region.size()
                || !analysis->schedule.bundle_to_region[source]
                || !analysis->schedule.bundle_to_region[target]) {
                return false;
            }
            auto const source_region =
                *analysis->schedule.bundle_to_region[source];
            auto const target_region =
                *analysis->schedule.bundle_to_region[target];
            return source_region < analysis->schedule.regions.size()
                && target_region < analysis->schedule.regions.size()
                && analysis->schedule.regions[source_region].cyclic
                && !analysis->schedule.regions[target_region].cyclic;
        });
    ASSERT_NE(fanout, analysis->event_connections.end());
    EXPECT_EQ(fanout->source_history, 0u);
    EXPECT_EQ(fanout->source_latency, 0u);
    EXPECT_EQ(fanout->target_history, 320u);
    EXPECT_TRUE(fanout->requires_block_materialization);

    auto const fanout_index = static_cast<std::size_t>(
        std::distance(analysis->event_connections.begin(), fanout));
    auto const group = std::ranges::find_if(
        analysis->event_producer_groups,
        [&](iv::graph_jit::detail::EventProducerGroupPlan const& candidate) {
            return std::ranges::find(
                       candidate.connection_indices, fanout_index)
                != candidate.connection_indices.end();
        });
    ASSERT_NE(group, analysis->event_producer_groups.end());
    ASSERT_TRUE(group->storage_plan.has_value());
    EXPECT_EQ(
        group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(group->storage_requirements.retained_window_samples, 320u);
    EXPECT_GE(
        group->storage_requirements.retained_event_capacity,
        group->storage_requirements.current_event_capacity);

    auto compiled = compile_graph(feedback_graph, 150);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    PersistentEventFeedbackConsumerStateMirror* observer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            compiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(PersistentEventFeedbackConsumerStateMirror)) {
            ASSERT_EQ(observer, nullptr);
            observer = static_cast<PersistentEventFeedbackConsumerStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(observer, nullptr);

    std::optional<iv::NodeLayout::RegionHandle> source_ring;
    std::optional<iv::NodeLayout::RegionHandle> feedback_carry;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.regions.size(); ++i) {
        auto const& region = compiled.compiled_graph->node_layout.regions[i];
        if (region.kind != iv::NodeLayout::Region::Kind::raw) continue;
        if (region.migration_identity.starts_with("graphjit.event.feedback:")) {
            ASSERT_FALSE(feedback_carry.has_value());
            EXPECT_NE(
                region.migration_identity.find("kind=compact_carry"),
                std::string::npos);
            feedback_carry = iv::NodeLayout::RegionHandle{.index = i};
        } else if (region.migration_identity.starts_with("graphjit.event:")
                   && region.migration_identity.find("kind=persistent_ring")
                       != std::string::npos) {
            ASSERT_FALSE(source_ring.has_value());
            source_ring = iv::NodeLayout::RegionHandle{.index = i};
        }
    }
    ASSERT_TRUE(source_ring.has_value());
    ASSERT_TRUE(feedback_carry.has_value());

    auto const write_index = [&](iv::NodeLayout::RegionHandle region) {
        auto const bytes = storage.region_bytes(region);
        EXPECT_GE(bytes.size(), 2 * sizeof(std::size_t));
        std::size_t value = 0;
        if (bytes.size() >= 2 * sizeof(std::size_t)) {
            std::memcpy(
                &value, bytes.data() + sizeof(std::size_t), sizeof(value));
        }
        return value;
    };
    auto const feedback_carry_count = [&]() {
        auto const bytes = storage.region_bytes(*feedback_carry);
        EXPECT_GE(bytes.size(), sizeof(std::size_t));
        std::size_t value = 0;
        if (bytes.size() >= sizeof(std::size_t)) {
            std::memcpy(&value, bytes.data(), sizeof(value));
        }
        return value;
    };

    for (std::size_t call = 0; call < 4; ++call) {
        auto const index = call * 64;
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(),
            &empty_tick_invocation,
            iv::GlobalIndex::from_bits(index),
            64);
        ASSERT_EQ(observer->calls, call + 1);
        EXPECT_EQ(observer->indices[call], index);
        EXPECT_EQ(observer->event_counts[call], (call + 1) * 8);
        EXPECT_EQ(observer->first_times[call], 1u);
        EXPECT_EQ(observer->last_times[call], index + 57);
        EXPECT_EQ(observer->marker, 0xfeed320u);

        // The canonical retained ring advances monotonically with authored
        // source events. Detached feedback stores only the one delayed event
        // crossing the current root boundary.
        EXPECT_EQ(write_index(*source_ring), (call + 1) * 8);
        EXPECT_EQ(feedback_carry_count(), 1u);
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
                return std::ranges::any_of(
                    connection.sources,
                    [](auto const& source) {
                        return source.detach.has_value();
                    });
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
        storage.buffer().data(), &empty_tick_invocation, index(0), 24);
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

    // Each one-sample root call publishes the declared 16-event/sample peak at
    // one timestamp. The exact ten-sample delayed live span therefore requires
    // 160 events, rounded to a 256-event compact carry.
    for (std::size_t index = 0; index < 15; ++index) {
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(),
            &empty_tick_invocation,
            iv::GlobalIndex::from_bits(index),
            1);
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
        build_connection_analysis_plan(
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
    ASSERT_TRUE(transient_event_group.storage_plan.has_value());
    EXPECT_EQ(
        transient_event_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
    EXPECT_TRUE(transient_event_group.requires_invocation_aggregate);

    auto transient_event = compile_graph(transient_event_graph, 124);
    ASSERT_TRUE(transient_event.succeeded())
        << (transient_event.diagnostics.empty()
                ? ""
                : transient_event.diagnostics.front().message);
    ASSERT_EQ(transient_event.compiled_graph->node_layout.nodes.size(), 2u);
    // Both sequences live in the root stack arena. NodeStorage owns only the
    // logical producer's overflow counter.
    ASSERT_EQ(count_raw_regions(transient_event.compiled_graph->node_layout), 1u);

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
        transient_event_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
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
        transient_event_storage.buffer().data(), &empty_tick_invocation, index(64), 48);
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

    // Event conversion is a transient storage operation owned by the producer
    // group. Two consumers requesting the same MIDI->trigger branch must share
    // one converted sequence rather than materializing the same fanout twice.
}

TEST_F(GraphJitRuntimeFixture, ConvertedEventFanout)
{
    auto converted_event_graph = configured_module_graph(
        *revision, graph_jit_converted_event_fanout_module_id);
    ASSERT_TRUE(converted_event_graph);
    auto converted_event_analysis =
        build_connection_analysis_plan(
            *converted_event_graph, 64);
    ASSERT_TRUE(converted_event_analysis.has_value())
        << (converted_event_analysis
                ? std::string{}
                : converted_event_analysis.error());
    ASSERT_EQ(converted_event_analysis->event_connections.size(), 2u);
    ASSERT_EQ(converted_event_analysis->event_producer_groups.size(), 1u);
    auto const& converted_event_group =
        converted_event_analysis->event_producer_groups.front();
    ASSERT_TRUE(converted_event_group.storage_plan.has_value());
    EXPECT_EQ(
        converted_event_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
    EXPECT_TRUE(converted_event_group.requires_invocation_aggregate);
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
    // Both sequences are root-stack allocations; the sole raw region is the
    // MIDI producer's overflow counter.
    ASSERT_EQ(count_raw_regions(converted_event.compiled_graph->node_layout), 1u);

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
        converted_event_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    for (auto const* probe : converted_event_probes) {
        EXPECT_EQ(probe->calls, 1u);
        EXPECT_EQ(probe->event_count, 2u);
        EXPECT_EQ(probe->trigger_count, 2u);
        EXPECT_EQ(probe->first_time, 5u);
        EXPECT_EQ(probe->last_time, 17u);
    }

    converted_event.compiled_graph->root_operations.tick_block(
        converted_event_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
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
        build_connection_analysis_plan(
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
    EXPECT_DOUBLE_EQ(retained_event_connection.max_events_per_index, 0.5);
    auto const& retained_event_group =
        retained_event_analysis->event_producer_groups.front();
    EXPECT_DOUBLE_EQ(retained_event_group.max_events_per_index, 0.5);
    ASSERT_TRUE(retained_event_group.storage_plan.has_value());
    EXPECT_EQ(
        retained_event_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(
        retained_event_group.storage_requirements.retained_window_samples,
        16u);
    EXPECT_EQ(
        retained_event_group.storage_requirements.retained_event_capacity,
        8u);
    EXPECT_LT(
        retained_event_group.storage_requirements.retained_event_capacity,
        retained_event_group.storage_requirements.current_event_capacity);

    auto retained_event = compile_graph(retained_event_graph, 126);
    ASSERT_TRUE(retained_event.succeeded())
        << (retained_event.diagnostics.empty()
                ? ""
                : retained_event.diagnostics.front().message);
    ASSERT_EQ(retained_event.compiled_graph->node_layout.nodes.size(), 2u);
    // The producer-local callback sequence is separate from the fan-in
    // working sequence and its persistent compact carry sequence.
    ASSERT_EQ(count_raw_regions(retained_event.compiled_graph->node_layout), 3u);
    auto const& retained_state_requirements = retained_event.compiled_graph
        ->realtime_port_state_requirements.states;
    auto const& retained_state_realizations = retained_event.compiled_graph
        ->realtime_port_state_realizations.event_states;
    ASSERT_EQ(retained_state_requirements.size(), 2u);
    ASSERT_EQ(retained_state_realizations.size(), 2u);
    for (auto const& realization : retained_state_realizations) {
        ASSERT_LT(
            realization.requirement_index,
            retained_state_requirements.size());
        auto const& requirement =
            retained_state_requirements[realization.requirement_index];
        EXPECT_EQ(requirement.configured_port.port_kind, iv::PortKind::event);
        EXPECT_EQ(
            realization.authored_storage.storage,
            iv::graph_jit::EventRealtimePortStateStorage::callback_transient);
        EXPECT_EQ(
            realization.retained_access,
            iv::graph_jit::EventRealtimePortStateAccess::direct);
        ASSERT_TRUE(realization.retained_storage);
        EXPECT_EQ(
            realization.retained_storage->storage,
            iv::graph_jit::EventRealtimePortStateStorage::compact_carry);
        ASSERT_TRUE(realization.retained_storage->node_storage_offset);
        auto const retained_region = std::ranges::find_if(
            retained_event.compiled_graph->node_layout.regions,
            [&](iv::NodeLayout::Region const& region) {
                return region.kind == iv::NodeLayout::Region::Kind::raw
                    && region.storage_offset
                        == *realization.retained_storage->node_storage_offset;
            });
        ASSERT_NE(
            retained_region,
            retained_event.compiled_graph->node_layout.regions.end());
        EXPECT_FALSE(retained_region->migration_identity.empty());
        EXPECT_FALSE(realization.retained_storage->callback_arena_offset);
        EXPECT_FALSE(realization.retained_storage->has_source_indices);
        EXPECT_FALSE(realization.retained_source_index);
        EXPECT_FALSE(realization.materialization_conversion);
    }

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
        retained_event_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(retained_event_probe->calls, 1u);
    EXPECT_EQ(retained_event_probe->indices[0], 0u);
    EXPECT_EQ(retained_event_probe->event_counts[0], 2u);
    EXPECT_EQ(retained_event_probe->first_times[0], 5u);
    EXPECT_EQ(retained_event_probe->last_times[0], 61u);

    // The compact carry committed [56, 72): timestamp 61 is historical input
    // on the next call and timestamp 67 was authored ahead by output latency.
    // Current-call events 69 and 125 are appended after the restored carry.
    retained_event.compiled_graph->root_operations.tick_block(
        retained_event_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    ASSERT_EQ(retained_event_probe->calls, 2u);
    EXPECT_EQ(retained_event_probe->indices[1], 64u);
    EXPECT_EQ(retained_event_probe->event_counts[1], 4u);
    EXPECT_EQ(retained_event_probe->first_times[1], 61u);
    EXPECT_EQ(retained_event_probe->second_times[1], 67u);
    EXPECT_EQ(retained_event_probe->last_times[1], 125u);

    // The next retained window is [120, 136), proving stale events from the
    // first call were trimmed rather than accumulating indefinitely.
    retained_event.compiled_graph->root_operations.tick_block(
        retained_event_storage.buffer().data(), &empty_tick_invocation, index(128), 64);
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

TEST_F(GraphJitRuntimeFixture,
       SeparateEventOutputAndInputStateNeedsNoTransitionRoot)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_retained_event_module_id);
    ASSERT_TRUE(graph);
    auto predecessor = compile_graph(graph, 254);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);

    iv::GraphJitCompileRequest successor_request{
        .project_generation = 255,
        .graph = graph,
        .definitions = definitions,
    };
    auto steady_successor = jit->compile(successor_request);
    ASSERT_TRUE(steady_successor.succeeded())
        << (steady_successor.diagnostics.empty()
                ? ""
                : steady_successor.diagnostics.front().message);
    auto specialized = jit->compile_transition(
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
    ASSERT_TRUE(specialized.compiled_graph->cutover);
    EXPECT_EQ(
        specialized.compiled_graph->root_operations.transition_tick_block,
        nullptr);
    auto const& transition = specialized.compiled_graph->cutover
        ->state_transition;
    ASSERT_EQ(transition.event_transfers.size(), 3u);
    EXPECT_TRUE(std::ranges::all_of(
        transition.event_transfers,
        [](auto const& transfer) { return !transfer.target_transition_only; }));
    EXPECT_EQ(
        std::ranges::count_if(
            transition.event_transfers,
            [](auto const& transfer) {
                return transfer.supplemental_target;
            }),
        1u);

    auto predecessor_storage = predecessor.compiled_graph
        ->node_layout.create_storage(resources);
    predecessor_storage.initialize();
    predecessor.compiled_graph->root_operations.tick_block(
        predecessor_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        64);

    auto successor_storage = specialized.compiled_graph
        ->node_layout.create_storage(resources);
    successor_storage.initialize(&predecessor_storage);
    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        transition,
        predecessor_storage.buffer(),
        successor_storage.buffer());
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared,
        predecessor_storage.buffer(),
        successor_storage.buffer(),
        index(64));

    RetainedEventConsumerProbeStateMirror* probe = nullptr;
    for (std::size_t node = 0;
         node < specialized.compiled_graph->node_layout.nodes.size();
         ++node) {
        if (specialized.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(RetainedEventConsumerProbeStateMirror)) {
            probe = static_cast<RetainedEventConsumerProbeStateMirror*>(
                successor_storage.state_ptr(node));
        }
    }
    ASSERT_NE(probe, nullptr);
    specialized.compiled_graph->root_operations.tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(64),
        64);
    ASSERT_EQ(probe->calls, 1u);
    EXPECT_EQ(probe->event_counts[0], 4u);
    EXPECT_EQ(probe->first_times[0], 61u);
    EXPECT_EQ(probe->second_times[0], 67u);
    EXPECT_EQ(probe->last_times[0], 125u);
}

TEST_F(GraphJitRuntimeFixture, PersistentEventRing)
{
    auto persistent_event_ring_graph = configured_module_graph(
        *revision, graph_jit_persistent_event_ring_module_id);
    ASSERT_TRUE(persistent_event_ring_graph);
    auto persistent_event_ring_analysis =
        build_connection_analysis_plan(
            *persistent_event_ring_graph, 64);
    ASSERT_TRUE(persistent_event_ring_analysis.has_value())
        << (persistent_event_ring_analysis
                ? std::string{}
                : persistent_event_ring_analysis.error());
    ASSERT_EQ(persistent_event_ring_analysis->event_producer_groups.size(), 1u);
    auto const& persistent_event_ring_group =
        persistent_event_ring_analysis->event_producer_groups.front();
    ASSERT_TRUE(persistent_event_ring_group.storage_plan.has_value());
    EXPECT_EQ(
        persistent_event_ring_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(
        persistent_event_ring_group.storage_requirements.retained_window_samples,
        160u);
    EXPECT_EQ(
        persistent_event_ring_group.storage_requirements.retained_event_capacity,
        80u);
    EXPECT_GE(
        persistent_event_ring_group.storage_requirements.retained_event_capacity,
        persistent_event_ring_group.storage_requirements.current_event_capacity);

    auto persistent_event_ring = compile_graph(
        persistent_event_ring_graph, 127);
    ASSERT_TRUE(persistent_event_ring.succeeded())
        << (persistent_event_ring.diagnostics.empty()
                ? ""
                : persistent_event_ring.diagnostics.front().message);
    ASSERT_EQ(
        persistent_event_ring.compiled_graph->node_layout.nodes.size(),
        2u);
    // The persistent ring is the canonical producer storage; the only
    // other raw region is its independent producer overflow counter.
    ASSERT_EQ(
        count_raw_regions(persistent_event_ring.compiled_graph->node_layout),
        2u);
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

    auto const& ring_state_requirements = persistent_event_ring.compiled_graph
        ->realtime_port_state_requirements.states;
    auto const& ring_state_realizations = persistent_event_ring.compiled_graph
        ->realtime_port_state_realizations.event_states;
    ASSERT_EQ(ring_state_requirements.size(), 1u);
    ASSERT_EQ(ring_state_realizations.size(), 1u);
    auto const& ring_state = ring_state_realizations.front();
    ASSERT_LT(ring_state.requirement_index, ring_state_requirements.size());
    EXPECT_EQ(
        ring_state_requirements[ring_state.requirement_index].role,
        iv::graph_jit::RealtimePortStateRole::sequential_input_history);
    EXPECT_EQ(
        ring_state.authored_storage.storage,
        iv::graph_jit::EventRealtimePortStateStorage::ring);
    EXPECT_EQ(
        ring_state.retained_access,
        iv::graph_jit::EventRealtimePortStateAccess::direct);
    ASSERT_TRUE(ring_state.retained_storage);
    EXPECT_EQ(
        ring_state.retained_storage->storage,
        iv::graph_jit::EventRealtimePortStateStorage::ring);
    ASSERT_TRUE(ring_state.retained_storage->node_storage_offset);
    EXPECT_EQ(
        *ring_state.retained_storage->node_storage_offset,
        persistent_event_ring_region->storage_offset);
    EXPECT_FALSE(ring_state.retained_storage->callback_arena_offset);
    EXPECT_FALSE(ring_state.retained_storage->has_source_indices);
    EXPECT_FALSE(ring_state.retained_source_index);

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
        persistent_event_ring_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 1u);
    EXPECT_EQ(persistent_event_ring_probe->indices[0], 0u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[0], 32u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[0], 5u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[0], 5u);

    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 2u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[1], 64u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[1], 5u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[1], 69u);

    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), &empty_tick_invocation, index(128), 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 3u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[2], 96u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[2], 5u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[2], 133u);

    // At index 192 the requested history begins at sample 32, so the first
    // block's 32 co-timestamped events expire in-place. The retained data
    // itself is not copied.
    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), &empty_tick_invocation, index(192), 64);
    ASSERT_EQ(persistent_event_ring_probe->calls, 4u);
    EXPECT_EQ(persistent_event_ring_probe->event_counts[3], 96u);
    EXPECT_EQ(persistent_event_ring_probe->first_times[3], 69u);
    EXPECT_EQ(persistent_event_ring_probe->last_times[3], 197u);

    // Advancing by another root block moves the history start to 96, expires
    // the second block, and appends through the storage end of the 128-index
    // ring. The consumer therefore exercises a wrapped BlockView.
    persistent_event_ring.compiled_graph->root_operations.tick_block(
        persistent_event_ring_storage.buffer().data(), &empty_tick_invocation, index(256), 64);
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
        build_connection_analysis_plan(
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
    ASSERT_TRUE(retained_converted_event_group.storage_plan.has_value());
    EXPECT_EQ(
        retained_converted_event_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(
        retained_converted_event_group.storage_requirements.retained_window_samples,
        160u);
    EXPECT_TRUE(retained_converted_event_group.requires_invocation_aggregate);
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
    // One persistent canonical MIDI ring plus its producer overflow counter;
    // the deduplicated converted trigger storage is root-stack storage.
    ASSERT_EQ(
        count_raw_regions(retained_converted_event.compiled_graph->node_layout),
        2u);
    auto retained_converted_persistent_region = std::ranges::find_if(
        retained_converted_event.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw
                && !region.migration_identity.empty();
        });
    auto retained_converted_telemetry_region = std::ranges::find_if(
        retained_converted_event.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw
                && region.migration_identity.empty();
        });
    ASSERT_NE(
        retained_converted_persistent_region,
        retained_converted_event.compiled_graph->node_layout.regions.end());
    ASSERT_NE(
        retained_converted_telemetry_region,
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
        retained_converted_event_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
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
        retained_converted_event_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
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
    auto const* overflow_count = reinterpret_cast<std::uint64_t const*>(
        retained_converted_event_storage.buffer().data()
        + retained_converted_telemetry_region->storage_offset);
    EXPECT_EQ(*overflow_count, 0u);

}

TEST_F(GraphJitRuntimeFixture, SampleHistoryCarryAndMigration)
{
    auto history_graph = configured_module_graph(
        *revision, graph_jit_history_fanout_module_id);
    ASSERT_TRUE(history_graph);
    auto history_analysis = build_connection_analysis_plan(
        *history_graph, 64);
    ASSERT_TRUE(history_analysis.has_value())
        << (history_analysis ? std::string{} : history_analysis.error());
    ASSERT_EQ(history_analysis->sample_producer_groups.size(), 1u);
    auto const& history_group = history_analysis->sample_producer_groups[0];
    ASSERT_TRUE(history_group.storage_plan.has_value());
    EXPECT_EQ(
        history_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(history_group.storage_requirements.retained_frames, 7u);

    auto history_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *history_analysis, 64);
    ASSERT_TRUE(history_plan.has_value())
        << (history_plan ? std::string{} : history_plan.error());
    ASSERT_EQ(history_plan->carry_operations.size(), 1u);
    ASSERT_EQ(history_plan->persistent_allocations.size(), 1u);
    EXPECT_TRUE(history_plan->materializations.empty());
    EXPECT_EQ(
        history_plan->persistent_allocations[0].kind,
        iv::graph_jit::detail::SamplePersistentStorageKind::compact_carry);
    EXPECT_EQ(
        history_plan->persistent_allocations[0].size_bytes,
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
        history_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_FLOAT_EQ(history_source_state->previous_output, 0.0f);
    EXPECT_FLOAT_EQ(history_mono_state->current, 0.0f);
    EXPECT_FLOAT_EQ(history_mono_state->history_5, 0.0f);
    EXPECT_FLOAT_EQ(history_stereo_state->current_left, 0.0f);
    EXPECT_FLOAT_EQ(history_stereo_state->current_right, 0.0f);

    history.compiled_graph->root_operations.tick_block(
        history_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
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
        history_storage.buffer().data(), &empty_tick_invocation, index(128), 4);
    EXPECT_FLOAT_EQ(history_source_state->previous_output, 127.0f);
    EXPECT_FLOAT_EQ(history_mono_state->current, 126.0f);
    EXPECT_FLOAT_EQ(history_mono_state->history_5, 121.0f);
    history.compiled_graph->root_operations.tick_block(
        history_storage.buffer().data(), &empty_tick_invocation, index(132), 4);
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
        history_next_storage.buffer().data(), &empty_tick_invocation, index(136), 4);
    EXPECT_FLOAT_EQ(migrated_source->previous_output, 135.0f);
    EXPECT_FLOAT_EQ(migrated_mono->current, 134.0f);
    EXPECT_FLOAT_EQ(migrated_mono->history_1, 133.0f);
    EXPECT_FLOAT_EQ(migrated_mono->history_5, 129.0f);

    // Tick remains Sequential across a playback seam. The callback observes
    // the selected global coordinate, while latency/history address the values
    // immediately preceding this callback in invocation order.
    history_next.compiled_graph->root_operations.tick_block(
        history_next_storage.buffer().data(), &empty_tick_invocation, index(1000), 4);
    EXPECT_EQ(migrated_mono->last_index, 1000u);
    EXPECT_FLOAT_EQ(migrated_source->previous_output, 139.0f);
    EXPECT_FLOAT_EQ(migrated_mono->current, 138.0f);
    EXPECT_FLOAT_EQ(migrated_mono->history_1, 137.0f);
    EXPECT_FLOAT_EQ(migrated_mono->history_5, 133.0f);

}

TEST_F(GraphJitRuntimeFixture, PersistentSampleHistory)
{
    auto persistent_history_graph = configured_module_graph(
        *revision, graph_jit_persistent_history_module_id);
    ASSERT_TRUE(persistent_history_graph);
    auto persistent_history_analysis =
        build_connection_analysis_plan(
            *persistent_history_graph, 64);
    ASSERT_TRUE(persistent_history_analysis.has_value())
        << (persistent_history_analysis
                ? std::string{}
                : persistent_history_analysis.error());
    ASSERT_EQ(persistent_history_analysis->sample_producer_groups.size(), 1u);
    ASSERT_TRUE(
        persistent_history_analysis->sample_producer_groups[0]
            .storage_plan.has_value());
    EXPECT_EQ(
        persistent_history_analysis->sample_producer_groups[0].storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(
        persistent_history_analysis->sample_producer_groups[0]
            .storage_requirements.retained_frames,
        5000u);

    auto persistent_history = compile_graph(persistent_history_graph, 118);
    ASSERT_TRUE(persistent_history.succeeded())
        << (persistent_history.diagnostics.empty()
                ? ""
                : persistent_history.diagnostics.front().message);
    ASSERT_EQ(count_raw_regions(persistent_history.compiled_graph->node_layout), 2u);
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

    auto const& requirements = persistent_history.compiled_graph
        ->realtime_port_state_requirements.states;
    auto const& realizations = persistent_history.compiled_graph
        ->realtime_port_state_realizations.sample_states;
    ASSERT_EQ(requirements.size(), 1u);
    ASSERT_EQ(realizations.size(), 1u);
    ASSERT_LT(realizations[0].requirement_index, requirements.size());
    EXPECT_EQ(
        requirements[realizations[0].requirement_index].role,
        iv::graph_jit::RealtimePortStateRole::sequential_input_history);
    EXPECT_EQ(
        realizations[0].authored_storage.storage,
        iv::graph_jit::SampleRealtimePortStateStorage::ring);
    EXPECT_EQ(
        realizations[0].retained_access,
        iv::graph_jit::SampleRealtimePortStateAccess::direct);
    ASSERT_EQ(realizations[0].retained_sources.size(), 1u);
    auto const& retained_ring = realizations[0].retained_sources.front();
    EXPECT_EQ(
        retained_ring.storage,
        iv::graph_jit::SampleRealtimePortStateStorage::ring);
    EXPECT_EQ(
        realizations[0].authored_storage.working_frame_capacity,
        8192u);
    EXPECT_EQ(retained_ring.working_frame_capacity, 8192u);
    EXPECT_EQ(retained_ring.storage_frame_count, 8192u);
    EXPECT_EQ(
        retained_ring.storage_size_bytes,
        8192u * sizeof(iv::Sample));
    ASSERT_TRUE(retained_ring.node_storage_offset);
    EXPECT_EQ(
        *retained_ring.node_storage_offset,
        persistent_ring_region->storage_offset);
    EXPECT_FALSE(retained_ring.callback_arena_offset);
    EXPECT_FALSE(realizations[0].materialization);

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
            persistent_history_storage.buffer().data(),
            &empty_tick_invocation,
            iv::GlobalIndex::from_bits(index),
            64);
    }
    EXPECT_EQ(large_history_state->calls, 80u);
    EXPECT_EQ(large_history_state->last_index, 5056u);
    EXPECT_FLOAT_EQ(large_history_state->current, 5056.0f);
    EXPECT_FLOAT_EQ(large_history_state->history_1, 5055.0f);
    EXPECT_FLOAT_EQ(large_history_state->history_5000, 56.0f);

}

TEST_F(
    GraphJitRuntimeFixture,
    TransitionRootPreservesRewiredHistoryAndMirrorsNewProducerSamples)
{
    auto predecessor_graph = configured_module_graph(
        *revision, graph_jit_large_history_rewire_module_id);
    ASSERT_TRUE(predecessor_graph);
    auto successor_graph = rewired_large_history_graph(predecessor_graph);
    auto predecessor = compile_graph(predecessor_graph, 240);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);
    EXPECT_EQ(
        std::ranges::count_if(
            predecessor.compiled_graph->realtime_port_state_requirements.states,
            [](auto const& requirement) {
                return requirement.role
                        == iv::graph_jit::RealtimePortStateRole::
                            sequential_input_history
                    && requirement.stable_identity.has_value();
            }),
        2);

    iv::GraphJitCompileRequest successor_request{
        .project_generation = 241,
        .graph = successor_graph,
        .definitions = definitions,
    };
    auto steady_successor = jit->compile(successor_request);
    ASSERT_TRUE(steady_successor.succeeded())
        << (steady_successor.diagnostics.empty()
                ? ""
                : steady_successor.diagnostics.front().message);
    auto specialized = jit->compile_transition(
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
    ASSERT_TRUE(specialized.compiled_graph->cutover);
    ASSERT_NE(
        specialized.compiled_graph->root_operations.transition_tick_block,
        nullptr);
    ASSERT_EQ(
        specialized.compiled_graph->cutover->state_transition
            .sample_expiry_cursors.size(),
        1u);
    EXPECT_EQ(
        specialized.compiled_graph->cutover->state_transition
            .sample_expiry_cursors.front().value_count,
        5000u);
    EXPECT_EQ(
        specialized.compiled_graph->node_layout.steady_storage_size,
        steady_successor.compiled_graph->node_layout.storage_size);
    EXPECT_GT(
        specialized.compiled_graph->node_layout.storage_size,
        specialized.compiled_graph->node_layout.steady_storage_size);

    auto predecessor_storage = predecessor.compiled_graph
        ->node_layout.create_storage(resources);
    predecessor_storage.initialize();
    for (std::uint64_t sample = 0; sample <= 5056; sample += 64) {
        predecessor.compiled_graph->root_operations.tick_block(
            predecessor_storage.buffer().data(),
            &empty_tick_invocation,
            iv::GlobalIndex::from_bits(sample),
            64);
    }

    auto successor_storage = specialized.compiled_graph
        ->node_layout.create_storage(resources);
    successor_storage.initialize(&predecessor_storage);
    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        specialized.compiled_graph->cutover->state_transition,
        predecessor_storage.buffer(),
        successor_storage.buffer());
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared,
        predecessor_storage.buffer(),
        successor_storage.buffer(),
        index(5120));

    std::vector<LargeHistoryConsumerStateMirror*> consumers;
    for (std::size_t node = 0;
         node < specialized.compiled_graph->node_layout.nodes.size(); ++node) {
        if (specialized.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(LargeHistoryConsumerStateMirror)) {
            consumers.push_back(
                static_cast<LargeHistoryConsumerStateMirror*>(
                    successor_storage.state_ptr(node)));
        }
    }
    ASSERT_EQ(consumers.size(), 2u);

    specialized.compiled_graph->root_operations.transition_tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(5120),
        64);
    for (auto const* consumer : consumers) {
        EXPECT_FLOAT_EQ(consumer->current, 5120.0f);
        EXPECT_FLOAT_EQ(consumer->history_1, 5119.0f);
        EXPECT_FLOAT_EQ(consumer->history_5000, 120.0f);
    }

    for (std::uint64_t sample = 5184; sample < 10112; sample += 64) {
        specialized.compiled_graph->root_operations.transition_tick_block(
            successor_storage.buffer().data(),
            &empty_tick_invocation,
            index(sample),
            64);
    }
    specialized.compiled_graph->root_operations.transition_tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(10112),
        8);
    specialized.compiled_graph->root_operations.tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(10120),
        64);
    for (auto const* consumer : consumers) {
        EXPECT_FLOAT_EQ(consumer->current, 10120.0f);
        EXPECT_FLOAT_EQ(consumer->history_1, 10119.0f);
        EXPECT_FLOAT_EQ(consumer->history_5000, 5120.0f);
    }
}

TEST_F(GraphJitRuntimeFixture, RootGraphBoundaryPortsRejected)
{
    auto ported_graph = configured_module_graph(*revision, graph_jit_ported_module_id);
    ASSERT_TRUE(ported_graph);
    auto ported = compile_graph(ported_graph, 119);
    expect_lowering_failure(ported, "root graph must not declare boundary ports");

    auto disconnected_ported_graph =
        std::make_shared<iv::ConfiguredGraph>(*ported_graph);
    disconnected_ported_graph->connections = {};
    auto disconnected_ported = compile_graph(disconnected_ported_graph, 120);
    expect_lowering_failure(
        disconnected_ported, "root graph must not declare boundary ports");
}

TEST_F(GraphJitRuntimeFixture, CompiledGraphsRetainPackageAndOrcOwnership)
{
    auto stateful = compile(graph_jit_stateful_module_id, 130);
    ASSERT_TRUE(stateful.succeeded())
        << (stateful.diagnostics.empty() ? "" : stateful.diagnostics.front().message);
    auto stateful_survivor = stateful.compiled_graph;
    auto stateful_storage = stateful_survivor->node_layout.create_storage(resources);
    stateful_storage.initialize();
    auto stateful_tock_states =
        stateful_survivor->tock_state_layout.create_storage(resources);
    stateful_tock_states.initialize();
    auto* state = static_cast<StatefulProbeStateMirror*>(stateful_storage.state_ptr(0));
    auto* background = static_cast<StatefulProbeBackgroundStateMirror*>(
        stateful_tock_states.state_ptr(0));
    ASSERT_NE(state, nullptr);
    ASSERT_NE(background, nullptr);
    stateful_survivor->root_operations.tick_block(
        stateful_storage.buffer().data(), &empty_tick_invocation, index(17), 32);
    stateful_survivor->root_operations.tick_block(
        stateful_storage.buffer().data(), &empty_tick_invocation, index(73), 64);

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
        stateful_storage.buffer().data(), &empty_tick_invocation, index(137), 32);
    EXPECT_EQ(state->tick_calls, 3u);
    EXPECT_EQ(state->skip_calls, 0u);
    EXPECT_EQ(state->last_index, 137u);
    EXPECT_EQ(state->last_block_size, 32u);
    EXPECT_EQ(background->tick_calls, 0u);
    EXPECT_EQ(background->skip_calls, 0u);
    EXPECT_EQ(background->last_index, 0u);
    EXPECT_EQ(background->last_block_size, 0u);

    auto pointer_survivor_storage =
        pointer_survivor->node_layout.create_storage(resources);
    pointer_survivor_storage.initialize();
    auto* pointer_survivor_state = static_cast<PointerConfiguredProbeStateMirror*>(
        pointer_survivor_storage.state_ptr(0));
    ASSERT_NE(pointer_survivor_state, nullptr);
    pointer_survivor->root_operations.tick_block(
        pointer_survivor_storage.buffer().data(), &empty_tick_invocation, index(149), 32);
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

namespace {

constexpr char graph_jit_playback_sample_history_module_id[] =
    "iv.test.graph_jit.state_context.playback_sample_history_module";
constexpr char graph_jit_playback_sample_history_replacement_module_id[] =
    "iv.test.graph_jit.state_context.playback_sample_history_replacement_module";
constexpr char graph_jit_playback_sample_window_module_id[] =
    "iv.test.graph_jit.state_context.playback_sample_window_module";
constexpr char graph_jit_playback_mixed_sample_module_id[] =
    "iv.test.graph_jit.state_context.playback_mixed_sample_module";
constexpr char graph_jit_playback_event_history_module_id[] =
    "iv.test.graph_jit.state_context.playback_event_history_module";
constexpr char graph_jit_playback_event_history_replacement_module_id[] =
    "iv.test.graph_jit.state_context.playback_event_history_replacement_module";
constexpr char graph_jit_playback_mixed_event_module_id[] =
    "iv.test.graph_jit.state_context.playback_mixed_event_module";
constexpr char graph_jit_playback_sample_rewire_module_id[] =
    "iv.test.graph_jit.state_context.playback_sample_rewire_module";
constexpr char graph_jit_playback_event_rewire_module_id[] =
    "iv.test.graph_jit.state_context.playback_event_rewire_module";

struct PlaybackHistoryConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::array<float, 5> window{};
};

struct PlaybackHistoryPeerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::array<float, 5> window{};
    std::uint64_t marker = 0;
};

struct PlaybackStereoHistoryConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::array<float, 3> left{};
    std::array<float, 3> right{};
};

struct PlaybackEventHistoryConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t count = 0;
    std::array<std::uint64_t, 8> times{};
};

struct PlaybackEventHistoryPeerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t count = 0;
    std::array<std::uint64_t, 8> times{};
    std::uint64_t marker = 0;
};

// Drives a compiled root through real TickInvocationCall slots. Each callback's
// background playback is published as the TickMaterializationSnapshot that a
// background transaction would promote, so the test controls exactly what
// every slot delivers.
class TickPlayback {
    iv::PersistedDataStore data_{};
    iv::PersistedDataStore::ReaderSlot data_reader_ = data_.register_reader();
    iv::TickMaterializationStore materializations_{};
    iv::TickMaterializationStore::ReaderSlot materialization_reader_ =
        materializations_.register_reader();
    iv::CompiledGraph const& graph_;
    iv::TickInvocationWorkspace workspace_;
    std::uint64_t semantic_version_ = 0;

public:
    explicit TickPlayback(iv::CompiledGraph const& graph)
        : graph_(graph)
        , workspace_(
            graph.background_evaluation_plan,
            graph.project_generation,
            data_,
            graph.specialization.block_size)
    {}

    void publish(
        std::vector<iv::TickMaterializedSampleInput> samples,
        std::vector<iv::TickMaterializedEventInput> events = {})
    {
        (void)materializations_.promote(
            std::make_unique<iv::TickMaterializationSnapshot>(
                graph_.project_generation,
                ++semantic_version_,
                iv::PersistedDataVersion{},
                std::move(samples),
                std::move(events)));
    }

    template<class Storage>
    void tick(
        Storage& storage,
        iv::GlobalIndex sample_index,
        std::size_t block_size,
        bool transition = false)
    {
        iv::TickInvocationFrame frame{
            data_reader_,
            materialization_reader_,
            workspace_,
            sample_index,
            block_size};
        auto const operation = transition
            ? graph_.root_operations.transition_tick_block
            : graph_.root_operations.tick_block;
        ASSERT_NE(operation, nullptr);
        operation(
            storage.buffer().data(), &frame.call(), sample_index, block_size);
    }
};

// Background playback of the graph's one slot-fed sample input over
// [begin, end). value(position, channel) supplies every channel.
template<class Value>
iv::TickMaterializedSampleInput playback_samples(
    iv::CompiledGraph const& graph,
    std::int64_t begin,
    std::int64_t end,
    Value value)
{
    auto const& plan = graph.background_evaluation_plan;
    EXPECT_EQ(plan.tick_runtime.sequential_sample_inputs.size(), 1u);
    auto const port = plan.tick_runtime.sequential_sample_inputs.at(0).port;
    auto const layout = plan.ports.at(port).sample_layout;
    std::vector<std::size_t> channels(iv::channel_count(layout));
    std::iota(channels.begin(), channels.end(), std::size_t{0});
    std::vector<iv::Sample> values;
    for (auto const channel : channels) {
        for (auto position = begin; position < end; ++position) {
            values.push_back(iv::Sample{value(position, channel)});
        }
    }
    return iv::make_tick_materialized_sample_input(
        port,
        layout,
        std::move(channels),
        iv::IndexCoverage{range(begin, end)},
        std::move(values));
}

// Background playback of the graph's one slot-fed event input over
// [begin, end), with trigger events at times.
iv::TickMaterializedEventInput playback_events(
    iv::CompiledGraph const& graph,
    std::int64_t begin,
    std::int64_t end,
    std::vector<std::int64_t> const& times,
    std::size_t semantic_source_index)
{
    auto const& plan = graph.background_evaluation_plan;
    EXPECT_EQ(plan.tick_runtime.sequential_event_inputs.size(), 1u);
    auto const port = plan.tick_runtime.sequential_event_inputs.at(0).port;
    std::vector<iv::TimedEvent> events;
    for (auto const time : times) {
        events.push_back({.time = index(time), .value = iv::TriggerEvent{}});
    }
    return iv::make_tick_materialized_event_input(
        port,
        plan.ports.at(port).event_type,
        iv::IndexCoverage{range(begin, end)},
        std::move(events),
        std::vector<std::size_t>(times.size(), semantic_source_index));
}

template<class Mirror, class Storage>
Mirror* playback_state(iv::CompiledGraph const& graph, Storage& storage)
{
    Mirror* result = nullptr;
    for (std::size_t node = 0; node < graph.node_layout.nodes.size(); ++node) {
        if (graph.node_layout.nodes[node].state_size != sizeof(Mirror)) continue;
        EXPECT_EQ(result, nullptr) << "playback state size is ambiguous";
        result = static_cast<Mirror*>(storage.state_ptr(node));
    }
    EXPECT_NE(result, nullptr);
    return result;
}

std::vector<std::uint64_t> recorded_times(
    std::uint64_t count, std::array<std::uint64_t, 8> const& times)
{
    return {
        times.begin(),
        times.begin() + static_cast<std::ptrdiff_t>(
            std::min<std::uint64_t>(count, times.size())),
    };
}

// The rewired successor exchanges the sources of the module's two
// connections, so each consumer changes delivery kind while keeping its
// stable port-state identity.
std::shared_ptr<iv::ConfiguredGraph const> exchange_connection_sources(
    std::shared_ptr<iv::ConfiguredGraph const> const& predecessor)
{
    auto successor = std::make_shared<iv::ConfiguredGraph>(*predecessor);
    auto sample_connections = std::vector<iv::ConfiguredSampleConnection>(
        predecessor->connections.configured_sample_connections().begin(),
        predecessor->connections.configured_sample_connections().end());
    auto event_connections = std::vector<iv::ConfiguredEventConnection>(
        predecessor->connections.configured_event_connections().begin(),
        predecessor->connections.configured_event_connections().end());
    if (sample_connections.size() + event_connections.size() != 2
        || (sample_connections.size() != 2 && event_connections.size() != 2)) {
        throw std::runtime_error(
            "unexpected playback rewire connection count");
    }
    if (sample_connections.size() == 2) {
        std::swap(
            sample_connections[0].source_type,
            sample_connections[1].source_type);
        std::swap(
            sample_connections[0].source_channels,
            sample_connections[1].source_channels);
    } else {
        std::swap(
            event_connections[0].source_type,
            event_connections[1].source_type);
        std::swap(event_connections[0].sources, event_connections[1].sources);
    }
    successor->connections =
        iv::GraphBuilderConnections::from_configured_connections(
            sample_connections, event_connections);
    return successor;
}

// Rebuild the same ConfiguredGraph with one unused scope-boundary bundle in
// front of every original bundle. Stable virtual-node records and graph
// identity are preserved; only generation-local bundle coordinates move.
std::shared_ptr<iv::ConfiguredGraph const> shift_bundle_handles(
    std::shared_ptr<iv::ConfiguredGraph const> const& graph)
{
    auto shifted = std::make_shared<iv::ConfiguredGraph>(*graph);

    iv::GraphBuilderNodeBundles bundles;
    (void)bundles.append_scope_boundary();
    auto const offset = bundles.import_child(graph->node_bundles);
    if (offset == 0) {
        throw std::runtime_error(
            "playback bundle-shift fixture did not create an offset");
    }
    shifted->node_bundles = std::move(bundles);

    iv::GraphBuilderConnections connections;
    connections.import_child(graph->connections, offset);
    shifted->connections = std::move(connections);

    auto virtual_nodes = graph->virtual_nodes.records();
    auto shift_port = [offset](iv::NodeBundlePortId& port) {
        port.node_bundle_handle += offset;
    };
    auto shift_channel = [offset](auto& channel) {
        channel.bundle += offset;
    };
    for (auto& node : virtual_nodes) {
        for (auto& bundle : node.node_bundle_handles) bundle += offset;
        for (auto& mapping : node.sample_inputs) {
            for (auto& channel : mapping.channels) shift_channel(channel);
            for (auto& member : mapping.member_channels) {
                for (auto& channel : member) shift_channel(channel);
            }
        }
        for (auto& mapping : node.sample_outputs) {
            for (auto& channel : mapping.channels) shift_channel(channel);
            for (auto& member : mapping.member_channels) {
                for (auto& channel : member) shift_channel(channel);
            }
        }
        for (auto& mapping : node.event_inputs) {
            for (auto& port : mapping.node_bundle_ports) shift_port(port);
        }
        for (auto& mapping : node.event_outputs) {
            for (auto& port : mapping.node_bundle_ports) shift_port(port);
        }
    }
    // GraphBuilderNodeBundles::import_child() clears inverse virtual-node
    // handles because ordinary child import rebuilds them through
    // GraphBuilderVirtualNodes::import_child(). This fixture preserves the
    // original virtual-node IDs instead, so restore the inverse relation
    // explicitly after shifting every record's bundle coordinates.
    for (std::size_t virtual_handle = 0;
         virtual_handle < virtual_nodes.size();
         ++virtual_handle) {
        for (auto const bundle :
             virtual_nodes[virtual_handle].node_bundle_handles) {
            auto& inverse = shifted->node_bundles
                .bundle(bundle)
                .virtual_node_handles();
            if (!std::ranges::contains(inverse, virtual_handle)) {
                inverse.push_back(virtual_handle);
            }
        }
    }
    shifted->virtual_nodes =
        iv::GraphBuilderVirtualNodes::from_configured_records(virtual_nodes);

    auto public_ports = graph->public_ports.configured_record();
    public_ports.boundary += offset;
    shifted->public_ports =
        iv::GraphBuilderPublicPorts::from_configured_record(public_ports);
    return shifted;
}

iv::NodeLayout::Region const& input_timeline_region(
    iv::CompiledGraph const& graph,
    std::string_view prefix)
{
    iv::NodeLayout::Region const* result = nullptr;
    for (auto const& region : graph.node_layout.regions) {
        if (region.kind != iv::NodeLayout::Region::Kind::raw
            || !region.migration_identity.starts_with(prefix)) {
            continue;
        }
        if (result) {
            throw std::runtime_error(
                "playback fixture has multiple input timeline regions");
        }
        result = &region;
    }
    if (!result) {
        throw std::runtime_error(
            "playback fixture has no stable input timeline region");
    }
    return *result;
}

iv::graph_jit::RealtimePortStateRequirement const& stable_input_history(
    iv::CompiledGraph const& graph,
    iv::PortKind kind)
{
    auto const found = std::ranges::find_if(
        graph.realtime_port_state_requirements.states,
        [kind](auto const& requirement) {
            return requirement.direction
                    == iv::graph_jit::PortDirection::input
                && requirement.configured_port.port_kind == kind
                && requirement.role == iv::graph_jit::RealtimePortStateRole::
                    sequential_input_history
                && requirement.stable_identity.has_value();
        });
    if (found == graph.realtime_port_state_requirements.states.end()) {
        throw std::runtime_error(
            "playback fixture has no stable input-history requirement");
    }
    return *found;
}

} // namespace

TEST_F(
    GraphJitRuntimeFixture,
    InputTimelineRawMigrationUsesStablePortIdentityForSamplesAndEvents)
{
    // RealtimePortStateTransition.MatchesStableStateAcrossGenerationLocalPorts
    // already covers semantic requirement matching after a bundle-number
    // change. This fixture verifies that the exact-shape NodeStorage fast path
    // uses that same identity rule and does not copy a different node's bytes.
    auto verify = [&] (
        std::string_view module_id,
        std::string_view replacement_module_id,
        iv::PortKind kind,
        std::string_view region_prefix,
        std::uint64_t generation) {
        SCOPED_TRACE(region_prefix);
        auto graph = configured_module_graph(*revision, module_id);
        auto replacement_graph = configured_module_graph(
            *revision, replacement_module_id);
        ASSERT_TRUE(graph);
        ASSERT_TRUE(replacement_graph);
        auto shifted_graph = shift_bundle_handles(graph);

        auto predecessor = compile_graph(graph, generation);
        auto replacement = compile_graph(replacement_graph, generation + 1);
        auto shifted = compile_graph(shifted_graph, generation + 2);
        ASSERT_TRUE(predecessor.succeeded())
            << (predecessor.diagnostics.empty()
                    ? ""
                    : predecessor.diagnostics.front().message);
        ASSERT_TRUE(replacement.succeeded())
            << (replacement.diagnostics.empty()
                    ? ""
                    : replacement.diagnostics.front().message);
        ASSERT_TRUE(shifted.succeeded())
            << (shifted.diagnostics.empty()
                    ? ""
                    : shifted.diagnostics.front().message);

        auto const& predecessor_compiled = *predecessor.compiled_graph;
        auto const& replacement_compiled = *replacement.compiled_graph;
        auto const& shifted_compiled = *shifted.compiled_graph;
        auto const& predecessor_requirement = stable_input_history(
            predecessor_compiled, kind);
        auto const& replacement_requirement = stable_input_history(
            replacement_compiled, kind);
        auto const& shifted_requirement = stable_input_history(
            shifted_compiled, kind);
        ASSERT_TRUE(predecessor_requirement.stable_identity);
        ASSERT_TRUE(replacement_requirement.stable_identity);
        ASSERT_TRUE(shifted_requirement.stable_identity);
        EXPECT_EQ(
            predecessor_requirement.configured_port,
            replacement_requirement.configured_port);
        EXPECT_NE(
            predecessor_requirement.stable_identity->node,
            replacement_requirement.stable_identity->node);
        EXPECT_NE(
            predecessor_requirement.configured_port.node_bundle_handle,
            shifted_requirement.configured_port.node_bundle_handle);
        EXPECT_EQ(
            predecessor_requirement.stable_identity,
            shifted_requirement.stable_identity);

        auto const& predecessor_region = input_timeline_region(
            predecessor_compiled, region_prefix);
        auto const& replacement_region = input_timeline_region(
            replacement_compiled, region_prefix);
        auto const& shifted_region = input_timeline_region(
            shifted_compiled, region_prefix);
        EXPECT_NE(
            predecessor_region.migration_identity,
            replacement_region.migration_identity);
        EXPECT_EQ(
            predecessor_region.migration_identity,
            shifted_region.migration_identity);
        ASSERT_EQ(predecessor_region.size, replacement_region.size);
        ASSERT_EQ(predecessor_region.size, shifted_region.size);
        ASSERT_EQ(predecessor_region.alignment, replacement_region.alignment);
        ASSERT_EQ(predecessor_region.alignment, shifted_region.alignment);

        auto predecessor_storage =
            predecessor_compiled.node_layout.create_storage(resources);
        predecessor_storage.initialize();
        auto predecessor_bytes = predecessor_storage.buffer().subspan(
            predecessor_region.storage_offset, predecessor_region.size);
        std::ranges::fill(predecessor_bytes, std::byte{0x5a});

        auto fresh_replacement =
            replacement_compiled.node_layout.create_storage(resources);
        fresh_replacement.initialize();
        auto const fresh_region_bytes = fresh_replacement.buffer().subspan(
            replacement_region.storage_offset, replacement_region.size);
        auto const fresh_bytes = std::vector<std::byte>(
            fresh_region_bytes.begin(), fresh_region_bytes.end());
        auto migrated_replacement =
            replacement_compiled.node_layout.create_storage(resources);
        migrated_replacement.initialize(&predecessor_storage);
        EXPECT_TRUE(std::ranges::equal(
            fresh_bytes,
            migrated_replacement.buffer().subspan(
                replacement_region.storage_offset, replacement_region.size)));

        auto migrated_shifted =
            shifted_compiled.node_layout.create_storage(resources);
        migrated_shifted.initialize(&predecessor_storage);
        EXPECT_TRUE(std::ranges::all_of(
            migrated_shifted.buffer().subspan(
                shifted_region.storage_offset, shifted_region.size),
            [](std::byte value) { return value == std::byte{0x5a}; }));
    };

    verify(
        graph_jit_playback_sample_history_module_id,
        graph_jit_playback_sample_history_replacement_module_id,
        iv::PortKind::sample,
        "graphjit.sample.input_timeline",
        621);
    verify(
        graph_jit_playback_event_history_module_id,
        graph_jit_playback_event_history_replacement_module_id,
        iv::PortKind::event,
        "graphjit.event.input_timeline",
        631);
}

TEST_F(GraphJitRuntimeFixture, PlaybackSampleInputKeepsItsConsumedHistory)
{
    auto compiled = compile(graph_jit_playback_sample_history_module_id, 501);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto storage = graph.node_layout.create_storage(resources);
    storage.initialize();
    auto* consumer =
        playback_state<PlaybackHistoryConsumerStateMirror>(graph, storage);
    ASSERT_NE(consumer, nullptr);
    TickPlayback playback{graph};

    // History before the first invocation is the input's neutral value.
    playback.publish({playback_samples(graph, 0, 64, [](auto position, auto) {
        return 1000.0f + static_cast<float>(position);
    })});
    playback.tick(storage, index(0), 64);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_EQ(
        consumer->window,
        (std::array<float, 5>{0.0f, 0.0f, 0.0f, 0.0f, 1000.0f}));

    // Republished earlier positions do not rewrite consumed history.
    playback.publish({playback_samples(graph, 0, 128, [](auto position, auto) {
        return position < 64 ? -1.0f : 2000.0f + static_cast<float>(position);
    })});
    playback.tick(storage, index(64), 64);
    EXPECT_EQ(
        consumer->window,
        (std::array<float, 5>{1060.0f, 1061.0f, 1062.0f, 1063.0f, 2064.0f}));

    // After a seek, history remains the preceding values in invocation order.
    playback.publish({playback_samples(graph, 1000, 1064, [](auto position, auto) {
        return 3000.0f + static_cast<float>(position);
    })});
    playback.tick(storage, index(1000), 64);
    EXPECT_EQ(consumer->last_index, index(1000).bits());
    EXPECT_EQ(
        consumer->window,
        (std::array<float, 5>{2124.0f, 2125.0f, 2126.0f, 2127.0f, 4000.0f}));
}

TEST_F(GraphJitRuntimeFixture, PlaybackSampleInputWithoutHistoryReadsItsWindow)
{
    auto compiled = compile(graph_jit_playback_sample_window_module_id, 502);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto storage = graph.node_layout.create_storage(resources);
    storage.initialize();
    auto* consumer =
        playback_state<SampleConsumerProbeStateMirror>(graph, storage);
    ASSERT_NE(consumer, nullptr);
    TickPlayback playback{graph};

    // The window begins at a position that is not a multiple of the slot
    // capacity.
    playback.publish({playback_samples(graph, 100, 108, [](auto position, auto) {
        return 1000.0f + static_cast<float>(position);
    })});
    playback.tick(storage, index(100), 8);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_EQ(consumer->last_block_size, 8u);
    EXPECT_FLOAT_EQ(consumer->first, 1100.0f);
    EXPECT_FLOAT_EQ(consumer->last, 1107.0f);
    EXPECT_FLOAT_EQ(consumer->sum, 8828.0f);
}

TEST_F(GraphJitRuntimeFixture, MixedSampleInputKeepsItsCombinedHistory)
{
    auto compiled = compile(graph_jit_playback_mixed_sample_module_id, 503);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto storage = graph.node_layout.create_storage(resources);
    storage.initialize();
    auto* consumer =
        playback_state<PlaybackStereoHistoryConsumerStateMirror>(graph, storage);
    ASSERT_NE(consumer, nullptr);
    TickPlayback playback{graph};

    // The left channel is the realtime ramp; the background supplies only
    // the right channel.
    playback.publish({playback_samples(graph, 64, 128, [](auto position, auto channel) {
        return channel == 0 ? 0.0f : 1000.0f + static_cast<float>(position);
    })});
    playback.tick(storage, index(64), 64);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_EQ(consumer->left, (std::array<float, 3>{0.0f, 0.0f, 64.0f}));
    EXPECT_EQ(consumer->right, (std::array<float, 3>{0.0f, 0.0f, 1064.0f}));

    playback.publish({playback_samples(graph, 64, 192, [](auto position, auto channel) {
        if (channel == 0) return 0.0f;
        return position < 128 ? -1.0f : 2000.0f + static_cast<float>(position);
    })});
    playback.tick(storage, index(128), 64);
    EXPECT_EQ(consumer->left, (std::array<float, 3>{126.0f, 127.0f, 128.0f}));
    EXPECT_EQ(
        consumer->right, (std::array<float, 3>{1126.0f, 1127.0f, 2128.0f}));
}

TEST_F(GraphJitRuntimeFixture, PlaybackEventInputKeepsItsConsumedHistory)
{
    auto compiled = compile(graph_jit_playback_event_history_module_id, 504);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto storage = graph.node_layout.create_storage(resources);
    storage.initialize();
    auto* consumer =
        playback_state<PlaybackEventHistoryConsumerStateMirror>(graph, storage);
    ASSERT_NE(consumer, nullptr);
    TickPlayback playback{graph};
    auto constexpr unindexed = std::numeric_limits<std::size_t>::max();

    playback.publish({}, {playback_events(graph, 0, 64, {10, 50}, unindexed)});
    playback.tick(storage, index(0), 64);
    EXPECT_EQ(
        recorded_times(consumer->count, consumer->times),
        (std::vector<std::uint64_t>{10, 50}));

    // Republished earlier positions do not rewrite consumed history.
    playback.publish(
        {}, {playback_events(graph, 0, 128, {70, 120}, unindexed)});
    playback.tick(storage, index(64), 64);
    EXPECT_EQ(
        recorded_times(consumer->count, consumer->times),
        (std::vector<std::uint64_t>{50, 70, 120}));

    // A seek moves retained history with the transport: the event consumed
    // at 120 stays 8 samples before the block that follows it.
    playback.publish(
        {}, {playback_events(graph, 1000, 1064, {1010}, unindexed)});
    playback.tick(storage, index(1000), 64);
    EXPECT_EQ(
        recorded_times(consumer->count, consumer->times),
        (std::vector<std::uint64_t>{992, 1010}));
}

TEST_F(GraphJitRuntimeFixture, MixedEventInputKeepsItsMergedHistory)
{
    auto compiled = compile(graph_jit_playback_mixed_event_module_id, 505);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto storage = graph.node_layout.create_storage(resources);
    storage.initialize();
    auto* consumer =
        playback_state<PlaybackEventHistoryConsumerStateMirror>(graph, storage);
    ASSERT_NE(consumer, nullptr);
    TickPlayback playback{graph};

    // The realtime source triggers at index + 3 and index + block - 1; the
    // background source is the second semantic source.
    playback.publish({}, {playback_events(graph, 0, 64, {20, 40}, 1)});
    playback.tick(storage, index(0), 64);
    EXPECT_EQ(
        recorded_times(consumer->count, consumer->times),
        (std::vector<std::uint64_t>{3, 20, 40, 63}));

    playback.publish({}, {playback_events(graph, 0, 128, {80}, 1)});
    playback.tick(storage, index(64), 64);
    EXPECT_EQ(
        recorded_times(consumer->count, consumer->times),
        (std::vector<std::uint64_t>{40, 63, 67, 80, 127}));
}

TEST_F(GraphJitRuntimeFixture, RevisionMovesSampleHistoryAcrossDeliveryKinds)
{
    auto predecessor_graph = configured_module_graph(
        *revision, graph_jit_playback_sample_rewire_module_id);
    ASSERT_TRUE(predecessor_graph);
    auto predecessor = compile_graph(predecessor_graph, 601);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);
    auto const& predecessor_compiled = *predecessor.compiled_graph;
    auto predecessor_storage =
        predecessor_compiled.node_layout.create_storage(resources);
    predecessor_storage.initialize();
    auto* realtime_fed = playback_state<PlaybackHistoryConsumerStateMirror>(
        predecessor_compiled, predecessor_storage);
    auto* background_fed = playback_state<PlaybackHistoryPeerStateMirror>(
        predecessor_compiled, predecessor_storage);
    ASSERT_NE(realtime_fed, nullptr);
    ASSERT_NE(background_fed, nullptr);
    {
        TickPlayback playback{predecessor_compiled};
        for (std::int64_t begin = 0; begin < 128; begin += 64) {
            playback.publish({playback_samples(
                predecessor_compiled, begin, begin + 64,
                [](auto position, auto) {
                    return 1000.0f + static_cast<float>(position);
                })});
            playback.tick(predecessor_storage, index(begin), 64);
        }
    }
    ASSERT_EQ(
        realtime_fed->window,
        (std::array<float, 5>{60.0f, 61.0f, 62.0f, 63.0f, 64.0f}));
    ASSERT_EQ(
        background_fed->window,
        (std::array<float, 5>{
            1060.0f, 1061.0f, 1062.0f, 1063.0f, 1064.0f}));

    iv::GraphJitCompileRequest successor_request{
        .project_generation = 602,
        .graph = exchange_connection_sources(predecessor_graph),
        .definitions = definitions,
    };
    auto steady_successor = jit->compile(successor_request);
    ASSERT_TRUE(steady_successor.succeeded())
        << (steady_successor.diagnostics.empty()
                ? ""
                : steady_successor.diagnostics.front().message);
    auto specialized = jit->compile_transition(
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
        index(128));

    // The consumer that was realtime-fed is now fed by playback, and the
    // other way around. Each keeps the history it consumed before cutover.
    auto* now_background_fed = playback_state<PlaybackHistoryConsumerStateMirror>(
        successor, successor_storage);
    auto* now_realtime_fed = playback_state<PlaybackHistoryPeerStateMirror>(
        successor, successor_storage);
    ASSERT_NE(now_background_fed, nullptr);
    ASSERT_NE(now_realtime_fed, nullptr);
    TickPlayback playback{successor};
    auto const playback_value = [](auto position, auto) {
        return 5000.0f + static_cast<float>(position);
    };
    playback.publish({playback_samples(successor, 128, 192, playback_value)});
    playback.tick(
        successor_storage,
        index(128),
        64,
        successor.root_operations.transition_tick_block != nullptr);
    EXPECT_EQ(
        now_background_fed->window,
        (std::array<float, 5>{124.0f, 125.0f, 126.0f, 127.0f, 5128.0f}));
    EXPECT_EQ(
        now_realtime_fed->window,
        (std::array<float, 5>{1124.0f, 1125.0f, 1126.0f, 1127.0f, 128.0f}));

    // Four values of history have elapsed, so the steady root continues.
    playback.publish({playback_samples(successor, 192, 256, playback_value)});
    playback.tick(successor_storage, index(192), 64);
    EXPECT_EQ(
        now_background_fed->window,
        (std::array<float, 5>{5188.0f, 5189.0f, 5190.0f, 5191.0f, 5192.0f}));
    EXPECT_EQ(
        now_realtime_fed->window,
        (std::array<float, 5>{188.0f, 189.0f, 190.0f, 191.0f, 192.0f}));
}

TEST_F(GraphJitRuntimeFixture, RevisionMovesEventHistoryAcrossDeliveryKinds)
{
    auto predecessor_graph = configured_module_graph(
        *revision, graph_jit_playback_event_rewire_module_id);
    ASSERT_TRUE(predecessor_graph);
    auto predecessor = compile_graph(predecessor_graph, 611);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);
    auto const& predecessor_compiled = *predecessor.compiled_graph;
    auto predecessor_storage =
        predecessor_compiled.node_layout.create_storage(resources);
    predecessor_storage.initialize();
    auto* realtime_fed = playback_state<PlaybackEventHistoryConsumerStateMirror>(
        predecessor_compiled, predecessor_storage);
    auto* background_fed = playback_state<PlaybackEventHistoryPeerStateMirror>(
        predecessor_compiled, predecessor_storage);
    ASSERT_NE(realtime_fed, nullptr);
    ASSERT_NE(background_fed, nullptr);
    auto constexpr unindexed = std::numeric_limits<std::size_t>::max();
    {
        TickPlayback playback{predecessor_compiled};
        playback.publish(
            {}, {playback_events(predecessor_compiled, 0, 64, {10}, unindexed)});
        playback.tick(predecessor_storage, index(0), 64);
        playback.publish(
            {}, {playback_events(predecessor_compiled, 64, 128, {100}, unindexed)});
        playback.tick(predecessor_storage, index(64), 64);
    }
    // The realtime source triggers at index + 3 and index + block - 1.
    ASSERT_EQ(
        recorded_times(realtime_fed->count, realtime_fed->times),
        (std::vector<std::uint64_t>{63, 67, 127}));
    ASSERT_EQ(
        recorded_times(background_fed->count, background_fed->times),
        (std::vector<std::uint64_t>{100}));

    iv::GraphJitCompileRequest successor_request{
        .project_generation = 612,
        .graph = exchange_connection_sources(predecessor_graph),
        .definitions = definitions,
    };
    auto steady_successor = jit->compile(successor_request);
    ASSERT_TRUE(steady_successor.succeeded())
        << (steady_successor.diagnostics.empty()
                ? ""
                : steady_successor.diagnostics.front().message);
    auto specialized = jit->compile_transition(
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
        index(128));

    auto* now_background_fed =
        playback_state<PlaybackEventHistoryConsumerStateMirror>(
            successor, successor_storage);
    auto* now_realtime_fed = playback_state<PlaybackEventHistoryPeerStateMirror>(
        successor, successor_storage);
    ASSERT_NE(now_background_fed, nullptr);
    ASSERT_NE(now_realtime_fed, nullptr);
    TickPlayback playback{successor};
    playback.publish(
        {}, {playback_events(successor, 128, 192, {150}, unindexed)});
    playback.tick(
        successor_storage,
        index(128),
        64,
        successor.root_operations.transition_tick_block != nullptr);
    EXPECT_EQ(
        recorded_times(now_background_fed->count, now_background_fed->times),
        (std::vector<std::uint64_t>{127, 150}));
    EXPECT_EQ(
        recorded_times(now_realtime_fed->count, now_realtime_fed->times),
        (std::vector<std::uint64_t>{100, 131, 191}));
}
