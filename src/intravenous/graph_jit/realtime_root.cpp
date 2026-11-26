#include <intravenous/graph_jit/realtime_root.h>

#include <intravenous/channel_layout.h>
#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/graph_jit/activity_runtime.h>
#include <intravenous/graph_jit/event_conversion_runtime.h>
#include <intravenous/graph_jit/event_retention_runtime.h>
#include <intravenous/graph_jit/realtime/root_plan.h>
#include <intravenous/graph_jit/tick_invocation_call.h>
#include <intravenous/node/manual_control.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#include <alloca.h>

namespace iv::graph_jit {
namespace {
using namespace realtime;

template<class T>
T load_raw(std::byte const* base, std::size_t offset) noexcept
{
    T value{};
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

template<class T>
void store_raw(std::byte* base, std::size_t offset, T value) noexcept
{
    std::memcpy(base + offset, &value, sizeof(value));
}

struct LatticeProjection {
    std::uint64_t begin_index = 0;
    std::size_t value_count = 0;
};

LatticeProjection project_lattice(
    GlobalIndex project_index,
    std::size_t project_count,
    std::int32_t pace_exponent) noexcept
{
    auto const bits = project_index.bits();
    if (pace_exponent >= 0) {
        auto const shift = static_cast<unsigned>(pace_exponent);
        return {
            .begin_index = bits << shift,
            .value_count = project_count << shift,
        };
    }
    auto const shift = static_cast<unsigned>(-pace_exponent);
    if (shift == sizeof(std::size_t) * 8) {
        auto const offset = std::uint64_t{0} - bits;
        return {
            .begin_index = 0,
            .value_count = offset < project_count ? 1u : 0u,
        };
    }
    auto const spacing = std::uint64_t{1} << shift;
    auto const mask = spacing - 1;
    auto const offset = (spacing - (bits & mask)) & mask;
    if (offset >= project_count) {
        return {.begin_index = (bits + offset) >> shift};
    }
    return {
        .begin_index = (bits + offset) >> shift,
        .value_count = 1 + ((project_count - offset - 1) >> shift),
    };
}

// Values of pace_exponent's lattice in [root_index, callback_index).
std::size_t projected_prefix(
    GlobalIndex root_index,
    GlobalIndex callback_index,
    std::int32_t pace_exponent) noexcept
{
    return project_lattice(
        root_index,
        static_cast<std::size_t>(root_index.forward_distance_to(callback_index)),
        pace_exponent).value_count;
}

float mixed_value(
    ChannelMix const& mix,
    std::size_t converted_channel,
    float const* sources) noexcept
{
    auto const begin = mix.term_begins[converted_channel];
    auto const end = mix.term_begins[converted_channel + 1];
    if (begin == end) return 0.0f;
    auto term = [&](ChannelTerm const& selected) noexcept {
        auto const value = sources[selected.source_channel];
        return selected.coefficient == 1.0f
            ? value
            : value * selected.coefficient;
    };
    auto value = term(mix.terms[begin]);
    for (auto index = begin + 1; index < end; ++index) {
        value += term(mix.terms[index]);
    }
    return value;
}

// ---------------------------------------------------------------------------
// Root call
// ---------------------------------------------------------------------------

struct RootFrame {
    RealtimeRootPlan const& plan;
    std::byte* storage_base = nullptr;
    std::byte* scratch = nullptr;
    std::span<std::byte*> sample_representations{};
    std::span<ManualControlLatch> manual_control_latches{};
    std::span<std::byte*> event_representations{};
    std::span<std::size_t> producer_authored_extents{};
    std::span<std::uint64_t> sequential_pace_cursors{};
    std::span<std::size_t> sample_representation_epochs{};
    std::span<std::size_t> event_representation_epochs{};
    std::span<ContinuousActivitySummary> continuous_activity_summaries{};
    std::span<EventActivitySummary> event_activity_summaries{};
    std::span<ReflectedNodeTickBatchLane> tick_lanes{};
    std::span<ReflectedNodeTickBatchLane> skip_lanes{};
    std::span<PrimitiveActivityDecision> tick_decisions{};
    std::span<std::size_t> tick_lane_primitives{};
    std::span<RegionSpecializationStep> specialization_steps{};
    std::span<std::uint64_t> specialization_begin_bases{};
    std::span<std::byte*> specialization_sample_representations{};
    std::span<std::byte> specialization_workspace{};
    TickInvocationCall const* invocation = nullptr;
    GlobalIndex root_index{};
};

template<class T>
std::span<T> frame_array(
    std::byte* scratch,
    std::size_t offset,
    std::size_t count) noexcept
{
    if (count == 0) return {};
    return {reinterpret_cast<T*>(scratch + offset), count};
}

std::byte* resolve_address(
    RootAddress const& address,
    std::byte* storage_base,
    std::byte* scratch) noexcept
{
    switch (address.base) {
    case RootAddressBase::immutable:
        return const_cast<std::byte*>(address.immutable);
    case RootAddressBase::node_storage:
        return storage_base + address.offset;
    case RootAddressBase::scratch:
        return scratch + address.offset;
    }
    return nullptr;
}

// The physical Sequential index of callback_index on one generation pace
// cursor.
std::uint64_t sequential_begin_index(
    RootFrame const& frame,
    std::size_t cursor,
    std::int32_t pace_exponent,
    GlobalIndex callback_index) noexcept
{
    return frame.sequential_pace_cursors[cursor]
        + projected_prefix(frame.root_index, callback_index, pace_exponent);
}

std::uint64_t representation_begin_index(
    RootFrame const& frame,
    std::size_t representation,
    GlobalIndex callback_index) noexcept
{
    return sequential_begin_index(
        frame,
        frame.plan.representation_cursors[representation],
        frame.plan.sample_ports.storage.representations[representation]
            .absolute_pace_exponent,
        callback_index);
}

std::uint64_t pace_begin_index(
    RootFrame const& frame,
    std::int32_t pace_exponent,
    GlobalIndex callback_index) noexcept
{
    auto const& cursors = frame.plan.sample_ports.storage.sequential_pace_cursors;
    std::size_t cursor = 0;
    while (cursors[cursor].absolute_pace_exponent != pace_exponent) ++cursor;
    return sequential_begin_index(frame, cursor, pace_exponent, callback_index);
}

std::byte& live_coverage_value(RootFrame const& frame,
    detail::LiveTickCoverageHistoryPlan const& history, std::uint64_t physical) noexcept
{
    return frame.storage_base[history.storage_offset + (physical & (history.capacity - 1))];
}

SequentialCoveragePresence live_projection_presence(RootFrame const& frame,
    std::span<SequentialCoverageSource const> sources, std::uint64_t physical,
    GlobalIndex position, std::int32_t pace) noexcept
{
    return select_projection_presence(sources, [&](SequentialCoverageSource const& source) {
        auto const change = *source.output_change;
        if (change < frame.plan.live_coverage_histories.size()) {
            if (auto const& history = frame.plan.live_coverage_histories[change]) {
                IV_ASSERT(history->pace == pace, "live Sequential coverage has a different pace than its value");
                return live_coverage_value(frame, *history, physical - source.read_latency) != std::byte{0}
                    ? SequentialCoveragePresence::covered : SequentialCoveragePresence::absent;
            }
        }
        if (!frame.invocation) return SequentialCoveragePresence::unknown;
        auto const held = frame.invocation->held_background_position;
        auto const selected = frame.invocation->output_coverages;
        if (change >= selected.size()) return SequentialCoveragePresence::unknown;
        auto const address = held ? GlobalIndex::from_bits(project_lattice(*held, 1, pace).begin_index)
                                  : position;
        return selected.data()[change].contains(address.retreated_by(source.read_latency))
            ? SequentialCoveragePresence::covered : SequentialCoveragePresence::absent;
    });
}

SequentialCoveragePresence live_input_presence(RootFrame const& frame,
    std::span<SequentialCoverageProjection const> projections, std::uint64_t physical,
    GlobalIndex position, std::int32_t pace, std::optional<std::size_t> channel = {}) noexcept
{
    return select_input_presence(projections, [&](auto const& sources) {
        return live_projection_presence(frame, sources, physical, position, pace);
    }, channel);
}

std::byte* continuous_value(
    RootFrame const& frame,
    std::size_t representation_index,
    std::uint64_t value_index,
    std::size_t channel) noexcept
{
    auto const& representation = frame.plan.sample_ports.storage
        .representations[representation_index];
    auto const channels = channel_count(representation.channel_layout);
    auto const slot = static_cast<std::size_t>(value_index)
        & (representation.frame_capacity - 1);
    auto const value = representation.channel_layout.sample_layout
            == SampleStreamLayout::planar
        ? channel * representation.frame_capacity + slot
        : slot * channels + channel;
    return frame.sample_representations[representation_index]
        + value * representation.geometry.value_size_bytes;
}

float load_lane(std::byte const* value, std::size_t lane) noexcept
{
    return load_raw<float>(value, lane * sizeof(float));
}

void store_lane(std::byte* value, std::size_t lane, float sample) noexcept
{
    store_raw(value, lane * sizeof(float), sample);
}

void materialize_sample(
    RootFrame& frame,
    std::size_t materialization_index,
    GlobalIndex callback_index,
    std::size_t callback_size) noexcept
{
    auto const& storage = frame.plan.sample_ports.storage;
    auto const& operation = storage.materializations[materialization_index];
    auto const& derived = frame.plan.materializations[materialization_index];
    auto const& target = storage.representations[operation.target_representation];
    auto const projected = project_lattice(
        callback_index, callback_size, target.absolute_pace_exponent).value_count;
    if (projected == 0) return;
    auto const begin = representation_begin_index(
        frame, operation.target_representation, callback_index);
    auto const first = begin - operation.retained_before;
    auto const count = projected
        + (operation.retained_before - operation.latest_read_latency);
    // A transition root's copy into an input's transition-only storage skips
    // the frames before the end of the window the cutover placed there.
    std::size_t first_offset = 0;
    if (operation.transition_cutover_index_storage_offset) {
        auto const cutover = load_raw<std::uint64_t>(
            frame.storage_base,
            *operation.transition_cutover_index_storage_offset);
        auto const skip = static_cast<std::int64_t>(
            cutover + static_cast<std::uint64_t>(operation.transition_first_frame)
            - first);
        if (skip > 0) {
            first_offset = std::min(static_cast<std::size_t>(skip), count);
        }
    }
    auto const source_channels = channel_count(operation.source_layout);
    auto const target_channels = derived.mix.converted_channel_count();
    auto const lanes = target.geometry.channel_scalar_lane_count;
    std::array<float, maximum_supported_channel_count> sources{};
    for (auto offset = first_offset; offset < count; ++offset) {
        auto const value_index = first + offset;
        for (std::size_t lane = 0; lane < lanes; ++lane) {
            for (std::size_t channel = 0; channel < source_channels; ++channel) {
                sources[channel] = load_lane(
                    continuous_value(
                        frame,
                        operation.source_representation,
                        value_index,
                        channel),
                    lane);
            }
            for (std::size_t channel = 0; channel < target_channels; ++channel) {
                if ((derived.written_channels >> channel & 1u) == 0) continue;
                store_lane(
                    continuous_value(
                        frame,
                        operation.target_representation,
                        value_index,
                        channel),
                    lane,
                    mixed_value(derived.mix, channel, sources.data()));
            }
        }
    }
}

std::size_t available_revision(
    RootFrame const& frame,
    std::size_t source_representation,
    std::size_t requested,
    GlobalIndex callback_index) noexcept
{
    if (requested == 0) return 0;
    auto const extent_index = frame.plan.authored_extents[source_representation];
    auto const& extent = frame.plan.sample_ports.storage
        .producer_authored_extents[extent_index];
    auto const prior = std::min(
        frame.producer_authored_extents[extent_index],
        extent.maximum_revision_frames);
    auto const prefix = projected_prefix(
        frame.root_index, callback_index, extent.absolute_pace_exponent);
    return std::min(
        requested,
        prior + std::min(prefix, extent.maximum_revision_frames - prior));
}

void compose_samples(
    RootFrame& frame,
    std::span<detail::SampleCompositionContributionPlan const> contributions,
    std::span<RootContribution const> derived,
    std::size_t target_representation,
    std::size_t target_history,
    bool shift_writes_by_read_latency,
    GlobalIndex callback_index,
    std::size_t callback_size) noexcept
{
    auto const& storage = frame.plan.sample_ports.storage;
    auto const& target = storage.representations[target_representation];
    auto const count = project_lattice(
        callback_index, callback_size, target.absolute_pace_exponent).value_count
        + target_history;
    if (count == 0) return;
    auto const first = representation_begin_index(
        frame, target_representation, callback_index) - target_history;
    auto const global_first = project_lattice(
        callback_index, callback_size, target.absolute_pace_exponent).begin_index
        - target_history;
    auto const lanes = target.geometry.channel_scalar_lane_count;

    // Shifted feedback writes rewrite only the history frames that every
    // source of a contribution has already authored.
    std::array<std::size_t, maximum_supported_channel_count> first_authored{};
    if (shift_writes_by_read_latency) {
        for (std::size_t index = 0; index < contributions.size(); ++index) {
            auto available = target_history;
            for (auto const& source : contributions[index].sources) {
                available = std::min(
                    available,
                    available_revision(
                        frame,
                        source.source_representation,
                        target_history,
                        callback_index));
            }
            first_authored[index] = target_history - available;
        }
    }

    std::array<float, maximum_supported_channel_count> local_sources{};
    auto* sources = frame.plan.maximum_composition_sources <= local_sources.size()
        ? local_sources.data()
        : reinterpret_cast<float*>(frame.scratch + frame.plan.composition_sources_offset);
    for (std::size_t offset = 0; offset < count; ++offset) {
        auto const target_frame = first + offset;
        for (std::size_t index = 0; index < contributions.size(); ++index) {
            auto const& contribution = contributions[index];
            auto const& mix = derived[index].mix;
            auto const authored = !shift_writes_by_read_latency || offset >= first_authored[index];
            auto const aligned = contribution.feedback_alignment_representation
                != detail::no_sample_representation;
            for (std::size_t lane = 0; lane < lanes; ++lane) {
                for (std::size_t source_index = 0;
                     source_index < contribution.sources.size();
                     ++source_index) {
                    auto const& source = contribution.sources[source_index];
                    auto const source_frame = shift_writes_by_read_latency
                        ? target_frame
                        : target_frame - source.read_latency;
                    sources[source_index] = load_lane(
                        continuous_value(
                            frame,
                            source.source_representation,
                            source_frame,
                            source.source_channel),
                        lane);
                }
                if (aligned) {
                    // Stage the current source values, then read each source
                    // where it aligns with the contribution's write latency.
                    for (std::size_t source_index = 0;
                         source_index < contribution.sources.size();
                         ++source_index) {
                        if (!authored) continue;
                        store_lane(
                            continuous_value(
                                frame,
                                contribution.feedback_alignment_representation,
                                target_frame,
                                source_index),
                            lane,
                            sources[source_index]);
                    }
                    for (std::size_t source_index = 0;
                         source_index < contribution.sources.size();
                         ++source_index) {
                        auto const latency_delta =
                            contribution.sources[source_index].read_latency
                            - contribution.feedback_alignment_write_latency;
                        sources[source_index] = load_lane(
                            continuous_value(
                                frame,
                                contribution.feedback_alignment_representation,
                                target_frame - latency_delta,
                                source_index),
                            lane);
                    }
                }
                for (std::size_t converted = 0;
                     converted < contribution.target_channels.size();
                     ++converted) {
                    if (shift_writes_by_read_latency && !authored) continue;
                    auto const write_frame = shift_writes_by_read_latency
                        ? target_frame + derived[index].write_latencies[converted]
                        : target_frame;
                    auto const channel = contribution.target_channels[converted];
                    auto* destination = continuous_value(frame, target_representation, write_frame, channel);
                    auto const covered = derived[index].coverage_sources.empty()
                        || live_projection_presence(frame, derived[index].coverage_sources, write_frame,
                            GlobalIndex::from_bits(global_first + offset + (write_frame - target_frame)),
                            target.absolute_pace_exponent) == SequentialCoveragePresence::covered;
                    if (derived[index].additive_target_channels & (std::uint64_t{1} << channel)) {
                        if (covered)
                            store_lane(destination, lane, load_lane(destination, lane) + mixed_value(mix, converted, sources));
                    } else {
                        store_lane(destination, lane, covered ? mixed_value(mix, converted, sources) : 0.0f);
                    }
                }
            }
        }
    }
}

void write_feedback_samples(
    RootFrame& frame,
    std::size_t timeline_index,
    GlobalIndex callback_index,
    std::size_t callback_size) noexcept
{
    auto const& storage = frame.plan.sample_ports.storage;
    auto const& timeline = storage.feedback_timelines[timeline_index];
    if (timeline.writer.kind
        == detail::SampleFeedbackTimelineWriterKind::composition) {
        compose_samples(
            frame,
            timeline.writer.composition_contributions,
            frame.plan.feedback_writes[timeline_index],
            timeline.timeline_representation,
            timeline.writer.revision_frames,
            true,
            callback_index,
            callback_size);
        return;
    }
    auto const& target = storage.representations[timeline.timeline_representation];
    auto const projected = project_lattice(
        callback_index, callback_size, target.absolute_pace_exponent).value_count;
    auto const begin = representation_begin_index(
        frame, timeline.timeline_representation, callback_index);
    auto const revision = available_revision(
        frame,
        timeline.writer.source_representation,
        timeline.writer.revision_frames,
        callback_index);
    auto const first = begin - revision;
    auto const count = projected + revision;
    auto const channels = channel_count(timeline.channel_layout);
    for (std::size_t offset = 0; offset < count; ++offset) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            std::memcpy(
                continuous_value(
                    frame,
                    timeline.timeline_representation,
                    first + offset,
                    channel),
                continuous_value(
                    frame,
                    timeline.writer.source_representation,
                    first + offset,
                    channel),
                target.geometry.value_size_bytes);
        }
    }
}

void sample_carry(
    RootFrame& frame,
    detail::SampleCarryOperationPlan const& operation,
    GlobalIndex callback_index,
    std::size_t callback_size,
    bool restore) noexcept
{
    auto const& storage = frame.plan.sample_ports.storage;
    auto const& representation = storage.representations[
        operation.representation_index];
    auto const projected = project_lattice(
        callback_index, callback_size, representation.absolute_pace_exponent)
        .value_count;
    if (projected == 0) return;
    auto const& allocation = storage.persistent_allocations[
        operation.persistent_allocation];
    auto const begin = representation_begin_index(
        frame, operation.representation_index, callback_index);
    auto const retained_before = operation.retained_frames
        - operation.future_frames;
    auto const first = restore
        ? begin - retained_before
        : begin + projected - retained_before;
    auto* carry = frame.storage_base + allocation.storage_offset;
    auto const channels = channel_count(allocation.channel_layout);
    auto const value_size = allocation.geometry.value_size_bytes;
    auto const planar = allocation.channel_layout.sample_layout
        == SampleStreamLayout::planar;
    for (std::size_t offset = 0; offset < operation.retained_frames; ++offset) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            auto* working = continuous_value(
                frame, operation.representation_index, first + offset, channel);
            auto* retained = carry + value_size * (planar
                ? channel * operation.retained_frames + offset
                : offset * channels + channel);
            if (restore) {
                std::memcpy(working, retained, value_size);
            } else {
                std::memcpy(retained, working, value_size);
            }
        }
    }
}

void event_carry(
    RootFrame& frame,
    detail::EventCarryPlan const& carry,
    GlobalIndex index,
    std::size_t block_size,
    bool restore) noexcept
{
    auto const& ports = frame.plan.event_ports.ports;
    auto const& working = ports[carry.working_representation];
    auto const& persistent = ports[carry.persistent_representation];
    auto* working_base = frame.event_representations[carry.working_representation];
    auto* persistent_base = frame.event_representations[carry.persistent_representation];
    auto* working_events = working_base + working.events_relative_offset;
    auto* persistent_events = persistent_base + persistent.events_relative_offset;
    auto* working_indices = working.has_source_indices
        ? reinterpret_cast<std::size_t*>(
            working_base + working.source_indices_relative_offset)
        : nullptr;
    auto* persistent_indices = persistent.has_source_indices
        ? reinterpret_cast<std::size_t*>(
            persistent_base + persistent.source_indices_relative_offset)
        : nullptr;
    if (restore) {
        auto const count = load_raw<std::size_t>(
            persistent_base, persistent.count_relative_offset);
        auto const initialized = load_raw<std::uint8_t>(
            persistent_base, persistent.timeline_initialized_relative_offset);
        auto const expected = initialized != 0
            ? GlobalIndex::from_bits(load_raw<std::uint64_t>(
                persistent_base, persistent.expected_next_time_relative_offset))
            : index;
        detail::iv_graph_jit_rebase_event_sequence(
            persistent_events, count, expected, index);
        store_raw<std::uint8_t>(
            persistent_base, persistent.timeline_initialized_relative_offset, 1);
        store_raw<std::uint64_t>(
            persistent_base,
            persistent.expected_next_time_relative_offset,
            index.advanced_by(block_size).bits());
        auto const restored = working.has_source_indices
            ? detail::iv_graph_jit_restore_ordered_event_carry(
                persistent_events,
                persistent_indices,
                count,
                working_events,
                working_indices,
                working.event_capacity)
            : detail::iv_graph_jit_restore_event_carry(
                persistent_events,
                count,
                working_events,
                working.event_capacity);
        store_raw(working_base, working.count_relative_offset, restored);
        return;
    }
    auto const count = load_raw<std::size_t>(
        working_base, working.count_relative_offset);
    auto const committed = working.has_source_indices
        ? detail::iv_graph_jit_commit_ordered_event_carry(
            working_events,
            working_indices,
            count,
            index,
            block_size,
            carry.retained_history_samples,
            carry.retained_latency_samples,
            persistent_events,
            persistent_indices,
            persistent.event_capacity)
        : detail::iv_graph_jit_commit_event_carry(
            working_events,
            count,
            index,
            block_size,
            carry.retained_history_samples,
            carry.retained_latency_samples,
            persistent_events,
            persistent.event_capacity);
    store_raw(persistent_base, persistent.count_relative_offset, committed);
}

void prune_event_ring(
    RootFrame& frame,
    detail::EventPersistentRingPlan const& ring,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& storage = frame.plan.event_ports.ports[ring.storage];
    auto* base = frame.event_representations[ring.storage];
    auto const read = load_raw<std::size_t>(base, storage.read_index_relative_offset);
    auto const write = load_raw<std::size_t>(base, storage.write_index_relative_offset);
    auto* events = base + storage.events_relative_offset;
    auto const initialized = load_raw<std::uint8_t>(
        base, storage.timeline_initialized_relative_offset);
    auto const expected = initialized != 0
        ? GlobalIndex::from_bits(load_raw<std::uint64_t>(
            base, storage.expected_next_time_relative_offset))
        : index;
    detail::iv_graph_jit_rebase_event_ring(
        events, storage.event_capacity, read, write, expected, index);
    store_raw<std::uint8_t>(base, storage.timeline_initialized_relative_offset, 1);
    store_raw<std::uint64_t>(
        base,
        storage.expected_next_time_relative_offset,
        index.advanced_by(block_size).bits());
    store_raw(
        base,
        storage.read_index_relative_offset,
        detail::iv_graph_jit_prune_event_persistent_ring(
            events,
            storage.event_capacity,
            read,
            write,
            index,
            ring.retained_history_samples));
}

void merge_events(RootFrame& frame, detail::EventMergePlan const& merge) noexcept
{
    auto const& ports = frame.plan.event_ports.ports;
    auto const& target = ports[merge.target_representation];
    auto* target_base = frame.event_representations[merge.target_representation];
    auto* target_events = target_base + target.events_relative_offset;
    auto* target_indices = target.has_source_indices
        ? reinterpret_cast<std::size_t*>(
            target_base + target.source_indices_relative_offset)
        : nullptr;
    auto const read = target.persistent_ring
        ? load_raw<std::size_t>(target_base, target.read_index_relative_offset)
        : 0;
    auto write = target.persistent_ring
        ? load_raw<std::size_t>(target_base, target.write_index_relative_offset)
        : merge.preserve_existing_target
            ? load_raw<std::size_t>(target_base, target.count_relative_offset)
            : 0;
    // Every merge into one sequence orders from the root call's
    // history-inclusive origin, including each slice of an SCC.
    auto const origin = frame.root_index.retreated_by(
        merge.ordering_history_samples);
    for (std::size_t index = 0; index < merge.source_representations.size(); ++index) {
        auto const representation = merge.source_representations[index];
        auto const& source = ports[representation];
        auto* source_base = frame.event_representations[representation];
        auto const source_count = load_raw<std::size_t>(
            source_base, source.count_relative_offset);
        auto* source_events = source_base + source.events_relative_offset;
        auto const delay = merge.source_delays.empty()
            ? std::size_t{0}
            : merge.source_delays[index];
        write = target.has_source_indices
            ? detail::iv_graph_jit_merge_ordered_event_sequence(
                target_events,
                target_indices,
                target.event_capacity,
                read,
                write,
                source_events,
                source.event_capacity,
                source_count,
                merge.source_indices[index],
                origin,
                delay)
            : detail::iv_graph_jit_merge_event_sequence(
                target_events,
                target.event_capacity,
                read,
                write,
                source_events,
                source.event_capacity,
                source_count,
                origin,
                delay);
    }
    store_raw(
        target_base,
        target.persistent_ring
            ? target.write_index_relative_offset
            : target.count_relative_offset,
        write);
}

std::uint32_t conversion_step(
    EventConversionPlan const& conversion,
    std::size_t index) noexcept
{
    return index < conversion.step_count
        ? static_cast<std::uint32_t>(conversion.steps[index])
        : 0;
}

void materialize_events(
    RootFrame& frame,
    detail::EventMaterializationPlan const& operation,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& ports = frame.plan.event_ports.ports;
    auto const& source = ports[operation.source_representation];
    auto const& target = ports[operation.target_representation];
    auto* source_base = frame.event_representations[operation.source_representation];
    auto* target_base = frame.event_representations[operation.target_representation];
    auto* source_events = source_base + source.events_relative_offset;
    auto* target_events = target_base + target.events_relative_offset;
    auto const* source_indices =
        operation.source_index_filter || operation.preserve_source_indices
        ? reinterpret_cast<std::size_t const*>(
            source_base + source.source_indices_relative_offset)
        : nullptr;
    auto* target_indices = operation.preserve_source_indices
        ? reinterpret_cast<std::size_t*>(
            target_base + target.source_indices_relative_offset)
        : nullptr;
    std::size_t count = 0;
    if (operation.source_index_filter
        || operation.select_invocation_window
        || source.persistent_ring) {
        auto const read = source.persistent_ring
            ? load_raw<std::size_t>(source_base, source.read_index_relative_offset)
            : 0;
        auto const write = source.persistent_ring
            ? load_raw<std::size_t>(source_base, source.write_index_relative_offset)
            : load_raw<std::size_t>(source_base, source.count_relative_offset);
        count = detail::iv_graph_jit_materialize_event_sequence(
            static_cast<std::uint32_t>(source.type),
            static_cast<std::uint32_t>(target.type),
            conversion_step(operation.conversion, 0),
            conversion_step(operation.conversion, 1),
            conversion_step(operation.conversion, 2),
            operation.conversion.step_count,
            source_events,
            source_indices,
            operation.source_index_filter.value_or(0),
            static_cast<std::uint32_t>(operation.source_index_filter.has_value()),
            source.event_capacity,
            read,
            write,
            index,
            block_size,
            operation.history_samples,
            target_events,
            target.event_capacity,
            target_indices);
    } else {
        auto const source_count = std::min(
            load_raw<std::size_t>(source_base, source.count_relative_offset),
            source.event_capacity);
        if (operation.conversion.step_count == 0) {
            count = std::min(source_count, target.event_capacity);
            std::memcpy(target_events, source_events, count * sizeof(TimedEvent));
            if (target_indices) {
                std::memcpy(
                    target_indices, source_indices, count * sizeof(std::size_t));
            }
        } else {
            count = detail::iv_graph_jit_convert_event_sequence(
                static_cast<std::uint32_t>(source.type),
                static_cast<std::uint32_t>(target.type),
                conversion_step(operation.conversion, 0),
                conversion_step(operation.conversion, 1),
                conversion_step(operation.conversion, 2),
                operation.conversion.step_count,
                source_events,
                source_count,
                target_events,
                target.event_capacity,
                source_indices,
                target_indices);
        }
    }
    store_raw(target_base, target.count_relative_offset, count);
}

void append_event_feedback(
    RootFrame& frame,
    detail::EventFeedbackPlan const& feedback,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& ports = frame.plan.event_ports.ports;
    auto const& source = ports[feedback.source_representation];
    auto const& target = ports[feedback.target_representation];
    auto* source_base = frame.event_representations[feedback.source_representation];
    auto* target_base = frame.event_representations[feedback.target_representation];
    auto const source_begin = source.persistent_ring
        ? load_raw<std::size_t>(source_base, source.read_index_relative_offset)
        : 0;
    auto const source_end = source.persistent_ring
        ? load_raw<std::size_t>(source_base, source.write_index_relative_offset)
        : std::min(
            load_raw<std::size_t>(source_base, source.count_relative_offset),
            source.event_capacity);
    // Most slices produce no events; they never enter the feedback helper.
    if (source_begin >= source_end) return;
    auto* source_events = source_base + source.events_relative_offset;
    auto const* source_indices = feedback.source_index_filter
        ? reinterpret_cast<std::size_t const*>(
            source_base + source.source_indices_relative_offset)
        : nullptr;
    auto* target_events = target_base + target.events_relative_offset;
    auto const origin = index.retreated_by(feedback.ordering_history_samples);
    auto const filter = feedback.source_index_filter.value_or(0);
    auto const has_filter = static_cast<std::uint32_t>(
        feedback.source_index_filter.has_value());
    auto const select_window = static_cast<std::uint32_t>(
        feedback.selects_final_window);
    auto const window_extent = feedback.selects_final_window ? block_size : 0;
    if (target.persistent_ring) {
        auto* read = reinterpret_cast<std::size_t*>(
            target_base + target.read_index_relative_offset);
        auto* write = reinterpret_cast<std::size_t*>(
            target_base + target.write_index_relative_offset);
        if (source.persistent_ring) {
            detail::iv_graph_jit_append_event_feedback_ring_source(
                source_events, source_indices, filter, has_filter,
                source.event_capacity, source_begin, source_end, origin,
                feedback.loop_extra_latency, target_events,
                target.event_capacity, read, write, select_window, index,
                window_extent);
        } else {
            detail::iv_graph_jit_append_event_feedback(
                source_events, source_indices, filter, has_filter,
                source_begin, source_end, origin,
                feedback.loop_extra_latency, target_events,
                target.event_capacity, read, write, select_window, index,
                window_extent);
        }
        return;
    }
    auto* count = reinterpret_cast<std::size_t*>(
        target_base + target.count_relative_offset);
    if (source.persistent_ring) {
        detail::iv_graph_jit_append_event_feedback_sequence_ring_source(
            source_events, source_indices, filter, has_filter,
            source.event_capacity, source_begin, source_end, origin,
            feedback.loop_extra_latency, target_events,
            target.event_capacity, count, select_window, index,
            window_extent);
    } else {
        detail::iv_graph_jit_append_event_feedback_sequence(
            source_events, source_indices, filter, has_filter,
            source_begin, source_end, origin,
            feedback.loop_extra_latency, target_events,
            target.event_capacity, count, select_window, index,
            window_extent);
    }
}

struct ObservationCaptureTimer {
    LiveObservationFrame& frame;
    std::chrono::steady_clock::time_point started{};
    explicit ObservationCaptureTimer(LiveObservationFrame& frame) : frame(frame)
    { if (frame.measure_capture) started = std::chrono::steady_clock::now(); }
    ~ObservationCaptureTimer()
    {
        if (frame.measure_capture) frame.capture_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
    }
};

void capture_sample_observation(RootFrame& frame, std::size_t slot,
    ReflectedContinuousInputPortBinding const& binding, std::span<std::size_t const> selected,
    GlobalIndex index, std::size_t block_size) noexcept
{
    if (!frame.invocation || !frame.invocation->observations) return;
    auto& observations = *frame.invocation->observations;
    auto* observation = observations.capture_value(slot);
    if (!observation || !observation->operation || !observation->operation->capture_block) return;
    ObservationCaptureTimer timer{observations};
    std::array<ObservationChannelView, maximum_supported_channel_count> channels{};
    auto const count = selected.empty() ? channel_count(binding.storage.channel_layout) : selected.size();
    for (std::size_t channel = 0; channel < count; ++channel) {
        auto const& source = binding.storage.channels[selected.empty() ? channel : selected[channel]];
        channels[channel] = {source.storage, source.value_capacity,
            source.value_stride_bytes, source.value_delay};
    }
    auto const projection = project_lattice(index, block_size, binding.absolute_pace_exponent);
    observation->operation->capture_block(observation->value, {
        .channels = std::span{channels}.first(count),
        .sequential_begin_index = binding.sequential_begin_index - binding.read_latency,
        .value_count = projection.value_count,
    });
}

void capture_observation_output(RootFrame& frame, std::size_t primitive,
    GlobalIndex index, std::size_t block_size) noexcept
{
    auto const& node = frame.plan.primitives[primitive];
    auto const& binding = *reinterpret_cast<ReflectedContinuousInputPortBinding const*>(
        frame.scratch + node.sample_inputs_offset);
    capture_sample_observation(frame, *frame.plan.observation_slots[primitive],
        binding, frame.plan.observation_channels[primitive], index, block_size);
}

void capture_input_observations(RootFrame& frame, RootPrimitive const& primitive,
    ReflectedContinuousInputPortBinding const* bindings, GlobalIndex index, std::size_t count) noexcept
{
    for (auto const& probe : primitive.input_observations)
        capture_sample_observation(frame, probe.slot, bindings[probe.port], std::span{&probe.channel, 1}, index, count);
}

void capture_observation_state(RootFrame& frame, std::size_t probe) noexcept
{
    if (!frame.invocation || !frame.invocation->observations) return;
    auto const& selected = frame.plan.state_probes[probe];
    auto& observations = *frame.invocation->observations;
    auto* value = observations.capture_value(selected.slot);
    if (!value || !value->operation || !value->operation->capture_state) return;
    ObservationCaptureTimer timer{observations};
    value->operation->capture_state(value->value, frame.storage_base + selected.storage_offset);
}

void mark_sample_representation(RootFrame& frame, std::size_t representation) noexcept
{
    if (frame.plan.tracks_activity) {
        ++frame.sample_representation_epochs[representation];
    }
}

void mark_event_representation(RootFrame& frame, std::size_t representation) noexcept
{
    if (frame.plan.tracks_activity) {
        ++frame.event_representation_epochs[representation];
    }
}

void run_operations(
    RootFrame& frame,
    std::span<RootOperation const> operations,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& plan = frame.plan;
    auto const& events = plan.event_ports;
    auto const& storage = plan.sample_ports.storage;
    for (auto const operation : operations) {
        switch (operation.kind) {
        case RootOperationKind::observation_output:
            capture_observation_output(frame, operation.index, index, block_size);
            break;
        case RootOperationKind::observation_state:
            capture_observation_state(frame, operation.index);
            break;
        case RootOperationKind::event_sequence_reset:
            store_raw<std::size_t>(
                frame.event_representations[operation.index],
                events.ports[operation.index].count_relative_offset,
                0);
            mark_event_representation(frame, operation.index);
            break;
        case RootOperationKind::event_ring_prune: {
            auto const& selected = events.persistent_rings[operation.index];
            prune_event_ring(frame, selected, index, block_size);
            mark_event_representation(frame, selected.storage);
            break;
        }
        case RootOperationKind::event_carry_restore: {
            auto const& selected = events.carry_operations[operation.index];
            event_carry(frame, selected, index, block_size, true);
            mark_event_representation(frame, selected.working_representation);
            break;
        }
        case RootOperationKind::event_carry_commit: {
            auto const& selected = events.carry_operations[operation.index];
            event_carry(frame, selected, index, block_size, false);
            mark_event_representation(frame, selected.persistent_representation);
            break;
        }
        case RootOperationKind::event_merge: {
            auto const& selected = events.merges[operation.index];
            merge_events(frame, selected);
            mark_event_representation(frame, selected.target_representation);
            break;
        }
        case RootOperationKind::event_materialize: {
            auto const& selected = events.materializations[operation.index];
            materialize_events(frame, selected, index, block_size);
            mark_event_representation(frame, selected.target_representation);
            break;
        }
        case RootOperationKind::event_feedback_append: {
            auto const& selected = events.feedback_operations[operation.index];
            append_event_feedback(frame, selected, index, block_size);
            mark_event_representation(frame, selected.target_representation);
            break;
        }
        case RootOperationKind::sample_carry_restore: {
            auto const& selected = storage.carry_operations[operation.index];
            sample_carry(frame, selected, index, block_size, true);
            mark_sample_representation(frame, selected.representation_index);
            break;
        }
        case RootOperationKind::sample_materialization:
            materialize_sample(frame, operation.index, index, block_size);
            mark_sample_representation(
                frame,
                storage.materializations[operation.index].target_representation);
            break;
        case RootOperationKind::sample_composition: {
            auto const& composition = storage.compositions[operation.index];
            compose_samples(
                frame,
                composition.contributions,
                plan.compositions[operation.index],
                composition.target_representation,
                composition.target_history,
                false,
                index,
                block_size);
            mark_sample_representation(frame, composition.target_representation);
            break;
        }
        case RootOperationKind::sample_feedback_write:
            write_feedback_samples(frame, operation.index, index, block_size);
            mark_sample_representation(
                frame,
                storage.feedback_timelines[operation.index].timeline_representation);
            break;
        case RootOperationKind::sample_carry_commit:
            // A commit writes the retained carry, not the working
            // representation, so no activity summary is invalidated.
            sample_carry(
                frame,
                storage.carry_operations[operation.index],
                index,
                block_size,
                false);
            break;
        }
    }
}

void initialize_bindings(RootFrame& frame) noexcept
{
    auto const& plan = frame.plan;
    for (auto const& copy : plan.binding_template_copies) {
        std::memcpy(
            frame.scratch + copy.scratch_offset,
            plan.binding_template.data() + copy.template_offset,
            copy.size);
    }
    for (auto const& pointer : plan.binding_pointers) {
        std::byte* base = nullptr;
        switch (pointer.base) {
        case BindingPointerBase::sample_representation:
            base = frame.sample_representations[pointer.representation];
            break;
        case BindingPointerBase::event_representation:
            base = frame.event_representations[pointer.representation];
            break;
        case BindingPointerBase::scratch:
            base = frame.scratch;
            break;
        case BindingPointerBase::node_storage:
            base = frame.storage_base;
            break;
        }
        store_raw(frame.scratch, pointer.field_offset, base + pointer.byte_offset);
    }
}

std::byte* bound_channel_value(
    ReflectedContinuousChannelStorageBinding const& channel,
    std::uint64_t index) noexcept
{
    auto const slot = static_cast<std::size_t>(index - channel.value_delay)
        & (channel.value_capacity - 1);
    return channel.storage + slot * channel.value_stride_bytes;
}

// Writes the current window of every slot-fed input that owns a timeline. A
// pure input receives its slot window; a mixed input receives the node's
// combination of its realtime operand with that window. Earlier timeline
// positions are the input's node-owned history and are never rewritten.
void stage_sample_timelines(
    RootFrame& frame,
    std::size_t primitive_index,
    RootTickSlots const& tick,
    GlobalIndex callback_index,
    std::size_t callback_size) noexcept
{
    auto const& plan = frame.plan;
    auto const& inputs = plan.sample_ports.primitives[primitive_index].inputs;
    auto const& callbacks = plan.primitives[primitive_index].callbacks;
    auto const* supplied = frame.invocation->sequential_sample_inputs.data()
        + tick.invocation.sequential_sample_begin;
    for (auto const slot : tick.sample_timeline_slots) {
        auto const input_index = tick.sample_inputs[slot];
        auto const& input = inputs[input_index];
        auto const& source = supplied[slot];
        // The timeline and the realtime operand are addressed by the same
        // generation pace cursor as the callback binding; slot frame zero is
        // the root call's first position.
        auto const pace = input.absolute_pace_exponent;
        auto const timeline_begin = pace_begin_index(frame, pace, callback_index);
        auto const supplied_begin = source.sequential_begin_index
            + projected_prefix(frame.root_index, callback_index, pace)
            - source.read_latency;
        auto const count = project_lattice(callback_index, callback_size, pace)
            .value_count;
        auto const value_size = input.geometry.value_size_bytes;
        for (std::size_t channel = 0;
             channel < channel_count(input.channel_layout);
             ++channel) {
            auto const& timeline = input.channels[channel];
            auto const& supplied_channel = source.storage.channels[channel];
            for (std::size_t value = 0; value < count; ++value) {
                auto* destination = continuous_value(
                    frame,
                    timeline.storage,
                    timeline_begin + value - timeline.frame_delay,
                    timeline.representation_channel);
                auto const* supplied_value = bound_channel_value(
                    supplied_channel, supplied_begin + value);
                if (input.resolves_coverage) {
                    auto const position = GlobalIndex::from_bits(
                        project_lattice(callback_index, callback_size, pace).begin_index + value);
                    auto present = [&](auto const& projections) {
                        auto presence = live_input_presence(frame, projections,
                            timeline_begin + value, position, pace, channel);
                        IV_ASSERT(presence != SequentialCoveragePresence::unknown,
                            "mixed Sequential contribution coverage was not resolved before Tick");
                        return presence == SequentialCoveragePresence::covered;
                    };
                    auto const background = present(input.background_coverage_projections);
                    auto const realtime = input.combines_invocation_supplied
                        && present(input.realtime_coverage_projections);
                    if (realtime) {
                        auto const& operand = input.realtime_operand_channels[channel];
                        std::memcpy(destination, continuous_value(frame, operand.storage,
                            timeline_begin + value - operand.frame_delay, operand.representation_channel), value_size);
                        if (background) {
                            auto const accepted = callbacks.combine_sequential_input_values(input_index,
                                input.geometry.value_type, destination, supplied_value, value_size,
                                input.geometry.elements_per_value);
                            IV_ASSERT(accepted, "mixed Sequential combination was rejected");
                        }
                    } else if (background) {
                        std::memcpy(destination, supplied_value, value_size);
                    } else {
                        auto const aggregate = live_input_presence(frame, input.coverage_projections,
                            timeline_begin + value, position, pace);
                        auto const& absent = aggregate == SequentialCoveragePresence::absent
                            ? input.fallback_value : input.neutral_value;
                        std::memcpy(destination, absent.data(), value_size);
                    }
                    continue;
                }
                if (!input.combines_invocation_supplied) {
                    std::memcpy(destination, supplied_value, value_size);
                    continue;
                }
                auto const& operand = input.realtime_operand_channels[channel];
                std::memcpy(
                    destination,
                    continuous_value(
                        frame,
                        operand.storage,
                        timeline_begin + value - operand.frame_delay,
                        operand.representation_channel),
                    value_size);
                auto const accepted = callbacks.combine_sequential_input_values(
                    input_index,
                    input.geometry.value_type,
                    destination,
                    supplied_value,
                    value_size,
                    input.geometry.elements_per_value);
                IV_ASSERT(accepted,
                    "GraphJit Sequential sample combination was rejected");
            }
        }
    }
}

// Inserts the current window of every slot-fed event input that owns a
// timeline ring. A mixed input inserts the current window of the deterministic
// merge of its realtime operand with the slot window. The ring was pruned and
// rebased before this primitive, so earlier events are the input's node-owned
// history.
void stage_event_timelines(
    RootFrame& frame,
    std::size_t primitive_index,
    RootTickSlots const& tick,
    GlobalIndex callback_index,
    std::size_t callback_size) noexcept
{
    auto const& plan = frame.plan;
    auto const& inputs = plan.event_ports.primitives[primitive_index].inputs;
    auto const* supplied = frame.invocation->sequential_event_inputs.data()
        + tick.invocation.sequential_event_begin;
    for (auto const slot : tick.event_timeline_slots) {
        auto const& input = inputs[tick.event_inputs[slot]];
        auto const& source_binding = supplied[slot];
        auto const& source = source_binding.storage;
        auto const* source_events = source.storage + source.events_offset;
        auto source_capacity = source.event_capacity;
        auto source_read = source.persistent_ring
            ? load_raw<std::size_t>(source.storage, source.read_index_offset)
            : 0;
        auto source_write = source.persistent_ring
            ? load_raw<std::size_t>(source.storage, source.write_index_offset)
            : load_raw<std::size_t>(source.storage, source.count_offset);

        if (input.combines_invocation_supplied) {
            auto const realtime_index = *input.realtime_operand_storage;
            auto const& realtime = plan.event_ports.ports[realtime_index];
            auto* realtime_base = frame.event_representations[realtime_index];
            auto const& combined = plan.event_ports.ports[*input.combined_storage];
            auto* combined_events = frame.event_representations[
                *input.combined_storage] + combined.events_relative_offset;
            source_write = detail::iv_graph_jit_merge_mixed_event_sequences(
                combined_events,
                combined.event_capacity,
                realtime_base + realtime.events_relative_offset,
                realtime.has_source_indices
                    ? reinterpret_cast<std::size_t const*>(
                        realtime_base + realtime.source_indices_relative_offset)
                    : nullptr,
                input.realtime_semantic_source_index.value_or(no_index),
                realtime.event_capacity,
                realtime.persistent_ring
                    ? load_raw<std::size_t>(
                        realtime_base, realtime.read_index_relative_offset)
                    : 0,
                load_raw<std::size_t>(
                    realtime_base,
                    realtime.persistent_ring
                        ? realtime.write_index_relative_offset
                        : realtime.count_relative_offset),
                source_events,
                source_binding.semantic_source_indices,
                no_index,
                source.event_capacity,
                source_read,
                source_write,
                callback_index);
            IV_ASSERT(source_write != no_index,
                "GraphJit mixed Sequential event merge overflowed its storage");
            source_events = combined_events;
            source_capacity = combined.event_capacity;
            source_read = 0;
        }

        auto const& target = plan.event_ports.ports[*input.storage];
        auto* target_base = frame.event_representations[*input.storage];
        auto const written = detail::iv_graph_jit_merge_staged_event_window(
            target_base + target.events_relative_offset,
            target.event_capacity,
            load_raw<std::size_t>(target_base, target.read_index_relative_offset),
            load_raw<std::size_t>(target_base, target.write_index_relative_offset),
            source_events,
            source_capacity,
            source_read,
            source_write,
            callback_index,
            callback_size,
            input.timeline_history_samples);
        IV_ASSERT(written != no_index,
            "GraphJit Sequential event staging overflowed its timeline");
        store_raw(target_base, target.write_index_relative_offset, written);
    }
}

// Resolve missing values after connection composition and slot staging, before
// the manual-control operation. Coverage is semantic; storage contents alone
// never distinguish an uncovered position from an unavailable covered value.
void stage_sequential_sample_resolution(
    RootFrame& frame,
    std::size_t primitive_index,
    GlobalIndex index,
    std::size_t count,
    ReflectedContinuousInputPortBinding const* bindings) noexcept
{
    auto const& primitive = frame.plan.primitives[primitive_index];
    for (auto const port : primitive.coverage_inputs) {
        auto const& input = frame.plan.sample_ports.primitives[primitive_index].inputs[port];
        bindings[port].upstream_coverage = {
            .context = &frame,
            .source = &input,
            .query = +[](void const* context, void const* source,
                std::uint64_t value_index, std::uint64_t sequential_index) noexcept {
                auto const& root = *static_cast<RootFrame const*>(context);
                auto const& plan = *static_cast<detail::PrimitiveSampleInputBindingPlan const*>(source);
                auto const presence = live_input_presence(root, plan.coverage_projections,
                    sequential_index, GlobalIndex::from_bits(value_index), plan.absolute_pace_exponent);
                IV_ASSERT(presence != SequentialCoveragePresence::unknown,
                    "Sequential input coverage was not resolved before Tick");
                return presence == SequentialCoveragePresence::covered;
            },
        };
        if (input.invocation_supplied) continue; // Resolved while staging its slot.
        auto const lattice = project_lattice(index, count, input.absolute_pace_exponent);
        auto const begin = pace_begin_index(frame, input.absolute_pace_exponent, index);
        auto const& operands = !input.manual_operand_channels.empty()
            ? input.manual_operand_channels : input.coverage_operand_channels;
        for (std::size_t value = 0; value < lattice.value_count; ++value) {
            auto const presence = live_input_presence(frame, input.coverage_projections,
                begin + value, GlobalIndex::from_bits(lattice.begin_index + value),
                input.absolute_pace_exponent);
            IV_ASSERT(presence != SequentialCoveragePresence::unknown,
                "Sequential input coverage was not resolved before Tick");
            for (std::size_t channel = 0; channel < input.channels.size(); ++channel) {
                auto const& target = input.channels[channel];
                auto* destination = continuous_value(frame, target.storage,
                    begin + value - target.frame_delay, target.representation_channel);
                if (presence == SequentialCoveragePresence::absent) {
                    std::memcpy(destination, input.fallback_value.data(), input.geometry.value_size_bytes);
                } else if (!operands.empty()) {
                    auto const& operand = operands[channel];
                    std::memcpy(destination, continuous_value(frame, operand.storage,
                        begin + value - operand.frame_delay, operand.representation_channel),
                        input.geometry.value_size_bytes);
                }
            }
        }
    }
}

// Adds every participating manual control of a primitive to its input
// timeline. The destination is always target-owned storage: an ordinary
// producer alias is copied first, and the node's Sequential combine operation
// then receives the scalar manual contribution.
void stage_manual_control_sample_timelines(
    RootFrame& frame,
    std::size_t primitive_index,
    GlobalIndex slice_index,
    std::size_t slice_size,
    GlobalIndex callback_index,
    std::size_t callback_size) noexcept
{
    auto const& plan = frame.plan;
    auto const& primitive = plan.primitives[primitive_index];
    auto const& inputs = plan.sample_ports.primitives[primitive_index].inputs;
    for (auto const control_index : primitive.manual_controls) {
        auto const& control = plan.sample_ports.manual_controls[control_index];
        if (!control.stages_timeline) continue;
        auto const& input = inputs[control.input_index];
        auto* state = reinterpret_cast<ManualControlRuntimeState*>(
            frame.storage_base + control.state_storage_offset);
        auto const pace = input.absolute_pace_exponent;
        auto const values = project_lattice(slice_index, slice_size, pace).value_count;
        if (values == 0) continue;
        auto const target_bits = frame.manual_control_latches[control_index].target_bits;
        auto const effective_bits = frame.manual_control_latches[control_index].effective_bits;
        auto target_value = std::bit_cast<float>(target_bits);
        auto effective_value = std::bit_cast<float>(effective_bits);
        auto const* circular = control.configured.dynamic && control.configured.circular_range
            && (control.configured.enabled || control.configured.override_connections)
            ? &*control.configured.circular_range : nullptr;
        double circular_delta = 0;
        if (circular) {
            target_value = circular->canonical(Sample{target_value}).value;
            effective_value = circular->canonical(Sample{effective_value}).value;
            circular_delta = circular->shortest_delta(Sample{effective_value}, Sample{target_value});
        }
        auto const callback_values = project_lattice(
            callback_index, callback_size, pace).value_count;
        auto const callback_prefix = projected_prefix(callback_index, slice_index, pace);
        auto const timeline_begin = pace_begin_index(frame, pace, slice_index);
        auto const has_existing_value = input.invocation_supplied
            || !input.manual_operand_channels.empty() || !control.first_for_input;
        for (std::size_t channel = 0;
             channel < channel_count(input.channel_layout);
             ++channel) {
            auto const& target = input.manual_timeline_channels[channel];
            for (std::size_t value_index = 0; value_index < values; ++value_index) {
                auto* destination = continuous_value(
                    frame,
                    target.storage,
                    timeline_begin + value_index - target.frame_delay,
                    target.representation_channel);
                if (control.fallback_only) {
                    auto const position = project_lattice(slice_index, slice_size, pace)
                        .begin_index + value_index;
                    auto const present = live_input_presence(frame, input.coverage_projections,
                        timeline_begin + value_index, GlobalIndex::from_bits(position), pace)
                            != SequentialCoveragePresence::absent;
                    if (present)
                        continue;
                }
                if (control.first_for_input && !input.resolves_coverage
                    && !input.manual_operand_channels.empty()) {
                    auto const& operand = input.manual_operand_channels[channel];
                    std::memcpy(
                        destination,
                        continuous_value(
                            frame,
                            operand.storage,
                            timeline_begin + value_index - operand.frame_delay,
                            operand.representation_channel),
                        sizeof(Sample));
                }
                auto value = target_value;
                if (control.configured.dynamic) {
                    auto const progress = callback_prefix + value_index + 1;
                    if (circular) {
                        if (progress < callback_values) {
                            auto const ratio = static_cast<double>(progress) / static_cast<double>(callback_values);
                            value = circular->wrap_adjacent(effective_value + circular_delta * ratio).value;
                        }
                    } else {
                        auto const ratio = static_cast<float>(progress) / static_cast<float>(callback_values);
                        value = effective_value + (target_value - effective_value) * ratio;
                    }
                }
                if (control.boundary_source) {
                    value *= control.channel_coefficients[channel];
                    if (control.delay_capacity) {
                        auto* ring = reinterpret_cast<Sample*>(frame.storage_base + control.delay_storage_offset)
                            + channel * control.delay_capacity;
                        auto const write_index = timeline_begin + value_index;
                        ring[write_index & (control.delay_capacity - 1)] = Sample{value};
                        value = ring[(write_index - control.delay) & (control.delay_capacity - 1)].value;
                    }
                    if (has_existing_value) value += reinterpret_cast<Sample const*>(destination)->value;
                    Sample const manual{value};
                    std::memcpy(destination, &manual, sizeof(manual));
                    continue;
                }
                Sample const manual{value};
                if (has_existing_value && !control.fallback_only) {
                    auto const accepted = primitive.callbacks
                        .combine_sequential_input_values(
                            control.input_index,
                            ContinuousValueTypeId::sample,
                            destination,
                            reinterpret_cast<std::byte const*>(&manual),
                            sizeof(manual),
                            1);
                    IV_ASSERT(accepted,
                        "GraphJit manual input combination was rejected");
                } else {
                    std::memcpy(destination, &manual, sizeof(manual));
                }
            }
        }
        if (callback_prefix + values >= callback_values) {
            state->effective_bits = std::bit_cast<std::uint32_t>(target_value);
        } else {
            state->effective_bits = effective_bits;
        }
    }
}

template<class T>
ReflectedSpan<T const> invocation_span(
    ReflectedSpan<T const> supplied,
    std::size_t begin,
    std::size_t count) noexcept
{
    if (supplied.data() == nullptr || supplied.size() < begin + count) return {};
    return {supplied.data() + begin, count};
}

// Completes, once per root call, the parts of each Tick primitive's bindings
// and context that its TickInvocationCall supplies: the bindings of inputs a
// slot replaces, and the Random Access views.
void initialize_tick_slots(RootFrame& frame) noexcept
{
    auto const& plan = frame.plan;
    for (auto const primitive_index : plan.tick_primitives) {
        auto const& primitive = plan.primitives[primitive_index];
        auto const& tick = *primitive.tick;
        auto const& invocation = tick.invocation;
        auto sample_inputs = frame_array<ReflectedContinuousInputPortBinding>(
            frame.scratch,
            primitive.sample_inputs_offset,
            plan.sample_ports.primitives[primitive_index].inputs.size());
        for (auto const slot : tick.sample_overlay_slots) {
            auto const& supplied = frame.invocation->sequential_sample_inputs
                .data()[invocation.sequential_sample_begin + slot];
            // The slot replaces the binding except for its compiler-owned
            // contiguous scratch.
            auto& binding = sample_inputs[tick.sample_inputs[slot]];
            auto const capacity = binding.contiguous_value_capacity;
            auto const upstream_coverage = binding.upstream_coverage;
            std::array<std::byte*, maximum_supported_channel_count> scratch{};
            for (std::size_t channel = 0; channel < scratch.size(); ++channel) {
                scratch[channel] = binding.storage.channels[channel].contiguous_scratch;
            }
            binding = supplied;
            binding.contiguous_value_capacity = capacity;
            binding.upstream_coverage = upstream_coverage;
            for (std::size_t channel = 0; channel < scratch.size(); ++channel) {
                binding.storage.channels[channel].contiguous_scratch = scratch[channel];
            }
        }
        auto event_inputs = frame_array<ReflectedEventInputPortBinding>(
            frame.scratch,
            primitive.event_inputs_offset,
            plan.event_ports.primitives[primitive_index].inputs.size());
        for (auto const slot : tick.event_overlay_slots) {
            event_inputs[tick.event_inputs[slot]] = frame.invocation
                ->sequential_event_inputs.data()[invocation.sequential_event_begin + slot];
        }
        auto& context = *reinterpret_cast<ReflectedNodeTickContext*>(
            frame.scratch + primitive.context_offset);
        if (invocation.random_access_sample_count != 0) {
            context.random_access_inputs = invocation_span(
                frame.invocation->random_access_sample_inputs,
                invocation.random_access_sample_begin,
                invocation.random_access_sample_count);
        }
        if (invocation.random_access_event_count != 0) {
            context.random_access_event_inputs = invocation_span(
                frame.invocation->random_access_event_inputs,
                invocation.random_access_event_begin,
                invocation.random_access_event_count);
        }
    }
}

void stage_live_output_coverage(RootFrame& frame, std::size_t primitive_index,
    GlobalIndex index, std::size_t count) noexcept
{
    auto const& primitive = frame.plan.primitives[primitive_index];
    if (primitive.live_coverage_outputs.empty()) return;
    auto const& inputs = frame.plan.sample_ports.primitives[primitive_index].inputs;
    auto const window = FractionalGlobalIndexRange::from_global_range({index, count});
    for (auto const change : primitive.live_coverage_outputs) {
        auto const& history = *frame.plan.live_coverage_histories[change];
        auto const output = *window.try_project_onto_lattice(history.pace);
        auto const begin = pace_begin_index(frame, history.pace, index);
        auto* evaluation = frame.storage_base + history.evaluation_offset;
        for (std::size_t offset = 0; offset < output.count; ++offset) {
            auto present = [&](auto const& projections) {
                auto const presence = live_input_presence(frame, projections, begin + offset,
                    GlobalIndex::from_bits(output.begin_index_bits + offset), history.pace);
                IV_ASSERT(presence != SequentialCoveragePresence::unknown,
                    "Tick input coverage was not resolved before its output rule");
                return presence == SequentialCoveragePresence::covered;
            };
            auto covered = [evaluation](std::size_t term) { return evaluation[term] != std::byte{0}; };
            for (std::size_t index = 0; index < history.terms.size(); ++index) {
                auto const& term = history.terms[index];
                using Op = iv::details::TickCoverageOp;
                bool value = false;
                switch (term.op) {
                case Op::input:
                    if (term.kind == PortKind::sample) {
                        auto const& input = inputs[term.first];
                        value = primitive.live_sample_inputs_with_manual_source[term.first]
                            || present(input.coverage_projections);
                    } else {
                        value = present(primitive.live_event_inputs[term.first]);
                    }
                    break;
                case Op::union_: value = covered(term.first) || covered(term.second); break;
                case Op::intersection: value = covered(term.first) && covered(term.second); break;
                case Op::extend_right: {
                    auto const tail_offset = history.terms.size() + index * sizeof(std::uint64_t);
                    auto const remaining = load_raw<std::uint64_t>(evaluation, tail_offset);
                    auto const input = covered(term.first);
                    value = input || remaining != 0;
                    auto const next = input ? static_cast<std::uint64_t>(term.second)
                        : remaining - (remaining != 0);
                    store_raw(evaluation, tail_offset, next);
                    break;
                }
                }
                evaluation[index] = value ? std::byte{1} : std::byte{0};
            }
            live_coverage_value(frame, history, begin + offset) = evaluation[history.terms.size() - 1];
        }
    }
}

// Writes what one invocation of a primitive changes: the windows of its
// timeline-owning slot inputs and staged manual controls, and the Sequential
// begin index of each binding. Everything else in its context is complete for
// the root call.
ReflectedNodeTickContext const& prepare_invocation(
    RootFrame& frame,
    std::size_t primitive_index,
    GlobalIndex callback_index,
    std::size_t callback_size,
    GlobalIndex enclosing_index,
    std::size_t enclosing_size) noexcept
{
    auto const& primitive = frame.plan.primitives[primitive_index];
    if (primitive.tick) {
        if (!primitive.tick->sample_timeline_slots.empty()) {
            stage_sample_timelines(
                frame, primitive_index, *primitive.tick, callback_index, callback_size);
        }
        if (!primitive.tick->event_timeline_slots.empty()) {
            stage_event_timelines(
                frame, primitive_index, *primitive.tick, callback_index, callback_size);
        }
    }
    if (!primitive.coverage_inputs.empty()) {
        auto const* inputs = reinterpret_cast<ReflectedContinuousInputPortBinding const*>(
            frame.scratch + primitive.sample_inputs_offset);
        stage_sequential_sample_resolution(frame, primitive_index, callback_index, callback_size, inputs);
    }
    if (!primitive.manual_controls.empty()) {
        stage_manual_control_sample_timelines(
            frame,
            primitive_index,
            callback_index,
            callback_size,
            enclosing_index,
            enclosing_size);
    }
    stage_live_output_coverage(frame, primitive_index, callback_index, callback_size);
    if (!primitive.sparse_event_outputs.empty()) {
        auto bindings = frame_array<ReflectedEventOutputPortBinding>(frame.scratch,
            primitive.event_outputs_offset, primitive.event_output_count);
        auto begins = frame_array<std::size_t>(frame.scratch,
            primitive.sparse_event_begins_offset, primitive.sparse_event_outputs.size());
        for (std::size_t slot = 0; slot < begins.size(); ++slot) {
            auto const& binding = bindings[primitive.sparse_event_outputs[slot].first];
            auto const& storage = binding.storage;
            begins[slot] = binding.append_existing ? load_raw<std::size_t>(storage.storage,
                storage.persistent_ring ? storage.write_index_offset : storage.count_offset) : 0;
        }
    }
    for (auto const& update : primitive.begin_indices) {
        // A directly bound slot holds the root call's window from frame zero,
        // so a slice begins at its projected offset.
        auto const prefix = projected_prefix(
            frame.root_index, callback_index, update.pace_exponent);
        store_raw<std::uint64_t>(
            frame.scratch,
            update.field_offset,
            update.cursor == no_index
                ? prefix
                : frame.sequential_pace_cursors[update.cursor] + prefix);
    }
    auto const& context = *reinterpret_cast<ReflectedNodeTickContext const*>(
        frame.scratch + primitive.context_offset);
    capture_input_observations(frame, primitive, context.continuous_input_bindings.data(), callback_index, callback_size);
    return context;
}

PrimitiveActivityDecision decide_primitive_activity(
    RootFrame& frame,
    std::size_t primitive_index,
    ReflectedNodeTickContext const& context,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& plan = frame.plan;
    auto const& selected = plan.primitive_activity[primitive_index];
    if (!selected) return {};
    auto const& activity = *selected;
    auto* state = reinterpret_cast<detail::PrimitiveActivityState*>(
        frame.storage_base + activity.storage_offset);
    auto inputs_constant = true;
    auto inputs_unchanged = true;
    auto const& sample_plan = plan.sample_ports.primitives[primitive_index];
    for (std::size_t input = 0; input < activity.continuous_inputs.size(); ++input) {
        auto const& input_activity = activity.continuous_inputs[input];
        if (input_activity.changed_during_transition) {
            inputs_constant = false;
            inputs_unchanged = false;
            continue;
        }
        if (input_activity.static_constant) continue;
        auto const value_count = project_lattice(
            index,
            block_size,
            sample_plan.inputs[input].absolute_pace_exponent).value_count;
        for (std::size_t channel = 0;
             channel < input_activity.channels.size();
             ++channel) {
            auto const frontier_index = input_activity.channels[channel].frontier;
            auto const& frontier = plan.activity_frontiers[frontier_index];
            // A frontier without a realtime representation has no realtime
            // writer that could invalidate its summary.
            auto const epoch = frontier.representation
                    == detail::no_sample_representation
                ? std::size_t{0}
                : frame.sample_representation_epochs[frontier.representation];
            auto& summary = frame.continuous_activity_summaries[frontier_index];
            if (!summary.valid
                || summary.representation_epoch != epoch
                || summary.index.bits() != index.bits()
                || summary.block_size != block_size) {
                summary = {
                    .valid = true,
                    .representation_epoch = epoch,
                    .index = index,
                    .block_size = block_size,
                    .flags = detail::iv_graph_jit_continuous_input_activity(
                        &context.continuous_input_bindings.data()[input],
                        channel,
                        value_count,
                        reinterpret_cast<std::uint8_t*>(
                            frame.storage_base
                            + frontier.remembered_valid_storage_offset),
                        frame.storage_base
                            + frontier.remembered_value_storage_offset),
                };
            }
            inputs_constant = inputs_constant
                && (summary.flags & detail::continuous_input_constant) != 0;
            inputs_unchanged = inputs_unchanged
                && (summary.flags & detail::continuous_input_unchanged) != 0;
        }
    }

    auto const& primitive = plan.primitives[primitive_index];
    auto const& event_plan = plan.event_ports.primitives[primitive_index];
    for (std::size_t input = 0; input < event_plan.inputs.size(); ++input) {
        auto const* binding = &context.event_input_bindings.data()[input];
        auto const storage = event_plan.inputs[input].storage;
        bool empty = false;
        if (!primitive.slot_supplied_event_inputs[input] && storage) {
            auto const epoch = frame.event_representation_epochs[*storage];
            auto& summary = frame.event_activity_summaries[*storage];
            if (!summary.valid
                || summary.representation_epoch != epoch
                || summary.index.bits() != index.bits()
                || summary.block_size != block_size) {
                summary = {
                    .valid = true,
                    .representation_epoch = epoch,
                    .index = index,
                    .block_size = block_size,
                    .empty = detail::iv_graph_jit_event_input_is_empty(
                        binding, index, block_size),
                };
            }
            empty = summary.empty;
        } else {
            empty = detail::iv_graph_jit_event_input_is_empty(
                binding, index, block_size);
        }
        inputs_constant = inputs_constant && empty;
        inputs_unchanged = inputs_unchanged && empty;
    }
    return {
        .plan = &activity,
        .state = state,
        .inputs_constant = inputs_constant,
        .inputs_unchanged = inputs_unchanged,
        .should_skip = state->dormant != 0 && inputs_unchanged,
    };
}

void update_primitive_activity(
    RootFrame const& frame,
    std::size_t primitive_index,
    PrimitiveActivityDecision const& decision,
    ReflectedNodeTickContext const& context,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    if (!decision.plan) return;
    auto const& sample_plan = frame.plan.sample_ports.primitives[primitive_index];
    auto outputs_clear = !context.continuous_output_bindings.empty()
        || !context.event_output_bindings.empty();
    std::size_t compact = 0;
    for (auto const& output : sample_plan.outputs) {
        if (!output.realtime) continue;
        auto const value_count = project_lattice(
            index, block_size, output.absolute_pace_exponent).value_count;
        auto const channels = decision.plan->continuous_output_channel_counts[compact];
        for (std::size_t channel = 0; channel < channels; ++channel) {
            outputs_clear = outputs_clear
                && detail::iv_graph_jit_continuous_output_is_clear(
                    &context.continuous_output_bindings.data()[compact],
                    channel,
                    value_count);
        }
        ++compact;
    }
    for (std::size_t output = 0;
         output < context.event_output_bindings.size();
         ++output) {
        outputs_clear = outputs_clear
            && detail::iv_graph_jit_event_output_is_empty(
                &context.event_output_bindings.data()[output], index, block_size);
    }
    // A changed input starts a new quiet interval. In particular, a dormant
    // primitive must not reuse the interval that preceded its wake.
    auto const quiescent = decision.inputs_constant && outputs_clear;
    auto const base = decision.inputs_unchanged
        ? decision.state->silent_samples
        : std::size_t{0};
    auto const next = base > std::numeric_limits<std::size_t>::max() - block_size
        ? std::numeric_limits<std::size_t>::max()
        : base + block_size;
    decision.state->silent_samples = quiescent ? next : 0;
    decision.state->dormant = static_cast<std::uint8_t>(
        quiescent && decision.state->silent_samples >= decision.plan->ttl_samples);
}

// Events outside a Tick output's declared coverage are not contributions.
// Compact its callback-owned sequence before realization, fan-out and capture;
// every downstream event input then sees the same authoritative stream.
void resolve_event_output_coverage(RootFrame& frame, RootPrimitive const& primitive,
    GlobalIndex index, std::size_t count) noexcept
{
    if (primitive.sparse_event_outputs.empty()) return;
    auto bindings = frame_array<ReflectedEventOutputPortBinding>(frame.scratch,
        primitive.event_outputs_offset, primitive.event_output_count);
    auto const begins = frame_array<std::size_t>(frame.scratch,
        primitive.sparse_event_begins_offset, primitive.sparse_event_outputs.size());
    for (std::size_t slot = 0; slot < primitive.sparse_event_outputs.size(); ++slot) {
        auto const [binding_index, output_change] = primitive.sparse_event_outputs[slot];
        auto const& history = *frame.plan.live_coverage_histories[output_change];
        auto const begin = pace_begin_index(frame, 0, index);
        auto const& storage = bindings[binding_index].storage;
        if (storage.event_capacity == 0) continue;
        auto const first = storage.persistent_ring
            ? load_raw<std::size_t>(storage.storage, storage.read_index_offset) : 0;
        auto const count_offset = storage.persistent_ring ? storage.write_index_offset : storage.count_offset;
        auto const end = load_raw<std::size_t>(storage.storage, count_offset);
        auto* events = reinterpret_cast<TimedEvent*>(storage.storage + storage.events_offset);
        auto const authored_begin = begins[slot];
        IV_ASSERT(authored_begin - first <= end - first,
            "event output write start is outside its retained sequence");
        auto next = authored_begin;
        for (auto read = authored_begin; read != end; ++read) {
            auto const& event = events[read & (storage.event_capacity - 1)];
            auto const offset = index.forward_distance_to(event.time);
            IV_ASSERT(offset < count, "pointwise Tick authored an event outside its invocation");
            if (offset < count && live_coverage_value(frame, history, begin + offset) != std::byte{0})
                events[next++ & (storage.event_capacity - 1)] = event;
        }
        store_raw(storage.storage, count_offset, next);
    }
}

void mark_primitive_outputs(RootFrame& frame, RootPrimitive const& primitive) noexcept
{
    if (!frame.plan.tracks_activity) return;
    for (auto const representation : primitive.output_sample_representations) {
        ++frame.sample_representation_epochs[representation];
    }
    for (auto const representation : primitive.output_event_representations) {
        ++frame.event_representation_epochs[representation];
    }
}

void capture_outputs(RootFrame& frame, std::size_t primitive_index,
    GlobalIndex index, std::size_t block_size) noexcept;

void invoke_primitive(
    RootFrame& frame,
    std::size_t primitive_index,
    GlobalIndex callback_index,
    std::size_t callback_size,
    GlobalIndex enclosing_index,
    std::size_t enclosing_size,
    bool captures) noexcept
{
    auto const& primitive = frame.plan.primitives[primitive_index];
    auto const& context = prepare_invocation(
        frame,
        primitive_index,
        callback_index,
        callback_size,
        enclosing_index,
        enclosing_size);
    if (!frame.plan.tracks_activity) {
        primitive.callbacks.tick_block(
            primitive.node_data.data(), context, callback_index, callback_size);
    } else {
        auto const decision = decide_primitive_activity(
            frame, primitive_index, context, callback_index, callback_size);
        if (decision.should_skip) {
            primitive.callbacks.skip_block(
                primitive.node_data.data(), context, callback_index, callback_size);
        } else {
            if (decision.state) decision.state->dormant = 0;
            primitive.callbacks.tick_block(
                primitive.node_data.data(), context, callback_index, callback_size);
            update_primitive_activity(
                frame, primitive_index, decision, context, callback_index, callback_size);
        }
    }
    if (captures) capture_outputs(frame, primitive_index, callback_index, callback_size);
    resolve_event_output_coverage(frame, primitive, callback_index, callback_size);
    mark_primitive_outputs(frame, primitive);
}

// Capture each callback before the next one can replace its disposition.
void capture_outputs(
    RootFrame& frame,
    std::size_t primitive_index,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& primitive = frame.plan.primitives[primitive_index];
    auto const& tick = *primitive.tick;
    auto sample_outputs = frame_array<ReflectedContinuousOutputPortBinding>(
        frame.scratch, primitive.sample_outputs_offset, primitive.sample_output_count);
    auto event_outputs = frame_array<ReflectedEventOutputPortBinding>(
        frame.scratch, primitive.event_outputs_offset, primitive.event_output_count);
    auto const& sample_captures = frame.invocation->sample_captures;
    for (std::size_t offset = 0; offset < tick.sample_capture_outputs.size(); ++offset) {
        auto const slot = tick.invocation.sample_capture_begin + offset;
        if (sample_captures.data() == nullptr || slot >= sample_captures.size()) break;
        auto const& operation = sample_captures.data()[slot];
        if (!operation.context || !operation.capture) continue;
        operation.capture(
            operation.context,
            &sample_outputs[tick.sample_capture_outputs[offset]],
            index,
            block_size);
    }
    auto const& event_captures = frame.invocation->event_captures;
    for (std::size_t offset = 0; offset < tick.event_capture_outputs.size(); ++offset) {
        auto const slot = tick.invocation.event_capture_begin + offset;
        if (event_captures.data() == nullptr || slot >= event_captures.size()) break;
        auto const& operation = event_captures.data()[slot];
        if (!operation.context || !operation.capture) continue;
        operation.capture(
            operation.context,
            &event_outputs[tick.event_capture_outputs[offset]],
            index,
            block_size);
    }
}

void run_step(
    RootFrame& frame,
    std::size_t step_index,
    GlobalIndex index,
    std::size_t block_size,
    bool allow_primitive_slicing) noexcept
{
    auto const& step = frame.plan.steps[step_index];
    if (!step.before.empty()) run_operations(frame, step.before, index, block_size);
    if (allow_primitive_slicing && step.maximum_block_size < block_size) {
        for (std::size_t offset = 0; offset < block_size;
             offset += step.maximum_block_size) {
            auto const slice_index = index.advanced_by(offset);
            auto const slice_size = std::min(step.maximum_block_size, block_size - offset);
            invoke_primitive(
                frame,
                step.primitive,
                slice_index,
                slice_size,
                index,
                block_size, step.captures);
        }
    } else {
        invoke_primitive(frame, step.primitive, index, block_size, index, block_size, step.captures);
    }
    if (!step.after.empty()) run_operations(frame, step.after, index, block_size);
}

// Invokes one compatible set of primitive steps as native callback lanes.
// A cyclic phase keeps each step's operations and captures around the common
// callback. Its regions are independent, so no lane observes another lane's
// outputs. An acyclic ready group has no step-local work by construction.
void run_step_batch(
    RootFrame& frame,
    std::span<std::size_t const> steps,
    GlobalIndex index,
    std::size_t block_size,
    bool run_step_operations) noexcept
{
    auto const& plan = frame.plan;
    if (run_step_operations) {
        for (auto const step_index : steps) {
            auto const& step = plan.steps[step_index];
            if (!step.before.empty()) {
                run_operations(frame, step.before, index, block_size);
            }
        }
    }
    if (!plan.tracks_activity) {
        details::NodeCompilerOperations const* callbacks = nullptr;
        std::size_t lane_count = 0;
        for (auto const step_index : steps) {
            auto const primitive_index = plan.steps[step_index].primitive;
            auto const& primitive = plan.primitives[primitive_index];
            if (!callbacks) callbacks = &primitive.callbacks;
            frame.tick_lanes[lane_count++] = ReflectedNodeTickBatchLane{
                .node_data = primitive.node_data.data(),
                .context = prepare_invocation(
                    frame, primitive_index, index, block_size, index, block_size),
                .index = index,
                .block_size = block_size,
            };
        }
        callbacks->tick_block_batch(ReflectedNodeTickBatchContext{
            .lanes = {frame.tick_lanes.data(), lane_count},
        });
    } else {
        std::size_t tick_count = 0;
        std::size_t skip_count = 0;
        details::NodeCompilerOperations const* callbacks = nullptr;
        for (auto const step_index : steps) {
            auto const primitive_index = plan.steps[step_index].primitive;
            auto const& primitive = plan.primitives[primitive_index];
            if (!callbacks) callbacks = &primitive.callbacks;
            auto const& context = prepare_invocation(
                frame, primitive_index, index, block_size, index, block_size);
            auto const decision = decide_primitive_activity(
                frame, primitive_index, context, index, block_size);
            auto const lane = ReflectedNodeTickBatchLane{
                .node_data = primitive.node_data.data(),
                .context = context,
                .index = index,
                .block_size = block_size,
            };
            if (decision.should_skip) {
                frame.skip_lanes[skip_count++] = lane;
                continue;
            }
            if (decision.state) decision.state->dormant = 0;
            frame.tick_lanes[tick_count] = lane;
            frame.tick_decisions[tick_count] = decision;
            frame.tick_lane_primitives[tick_count] = primitive_index;
            ++tick_count;
        }
        if (tick_count != 0) {
            callbacks->tick_block_batch(ReflectedNodeTickBatchContext{
                .lanes = {frame.tick_lanes.data(), tick_count},
            });
            for (std::size_t lane = 0; lane < tick_count; ++lane) {
                update_primitive_activity(
                    frame,
                    frame.tick_lane_primitives[lane],
                    frame.tick_decisions[lane],
                    frame.tick_lanes[lane].context,
                    index,
                    block_size);
            }
        }
        if (skip_count != 0) {
            callbacks->skip_block_batch(ReflectedNodeTickBatchContext{
                .lanes = {frame.skip_lanes.data(), skip_count},
            });
        }
    }
    for (auto const step_index : steps) {
        auto const& step = plan.steps[step_index];
        auto const& primitive = plan.primitives[step.primitive];
        if (step.captures) capture_outputs(frame, step.primitive, index, block_size);
        resolve_event_output_coverage(frame, primitive, index, block_size);
        mark_primitive_outputs(frame, primitive);
    }
    if (run_step_operations) {
        for (auto const step_index : steps) {
            auto const& step = plan.steps[step_index];
            if (!step.after.empty()) {
                run_operations(frame, step.after, index, block_size);
            }
        }
    }
}

// One cyclic region's complete sliced invocation through the interpreter,
// without batching it with other regions.
void run_region_slices(
    RootFrame& frame,
    detail::ExecutionRegionPlan const& region,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    for (std::size_t offset = 0; offset < block_size;
         offset += region.maximum_block_size) {
        auto const slice = index.advanced_by(offset);
        auto const count = std::min(region.maximum_block_size, block_size - offset);
        for (auto const step : region.primitive_steps) {
            run_step(frame, step, slice, count, false);
        }
    }
}

// The RegionSpecializationOperationsFunction a specialization calls for a
// step's own storage operations.
void run_specialized_step_operations(
    void* frame,
    void const* operations,
    std::size_t operation_begin,
    std::size_t operation_count,
    std::uint64_t index_bits,
    std::size_t count) noexcept
{
    auto const& selected =
        *static_cast<std::vector<RootOperation> const*>(operations);
    run_operations(
        *static_cast<RootFrame*>(frame),
        std::span<RootOperation const>{selected}.subspan(
            operation_begin, operation_count),
        GlobalIndex::from_bits(index_bits),
        count);
}

void mark_specialized_sample_representation(
    void* frame,
    std::size_t representation_index) noexcept
{
    mark_sample_representation(
        *static_cast<RootFrame*>(frame), representation_index);
}

// The RegionSpecializationManualControlsFunction a specialization calls after
// one step's operations_before and before its callback.
void stage_specialized_manual_controls(
    void* frame,
    std::size_t primitive_index,
    std::uint64_t slice_index_bits,
    std::size_t slice_count,
    std::uint64_t enclosing_index_bits,
    std::size_t enclosing_count,
    ReflectedContinuousInputPortBinding const* inputs) noexcept
{
    auto& root_frame = *static_cast<RootFrame*>(frame);
    stage_sequential_sample_resolution(root_frame, primitive_index,
        GlobalIndex::from_bits(slice_index_bits), slice_count, inputs);
    stage_manual_control_sample_timelines(
        root_frame,
        primitive_index,
        GlobalIndex::from_bits(slice_index_bits),
        slice_count,
        GlobalIndex::from_bits(enclosing_index_bits),
        enclosing_count);
    stage_live_output_coverage(root_frame, primitive_index,
        GlobalIndex::from_bits(slice_index_bits), slice_count);
    capture_input_observations(root_frame, root_frame.plan.primitives[primitive_index],
        inputs, GlobalIndex::from_bits(slice_index_bits), slice_count);
}

// One region's complete invocation through its compiled specialization. The
// generated function preserves every step operation's position. It delegates
// operation spans that remain on the root and directly executes the sample
// compositions that it owns.
void run_region_specialization(
    RootFrame& frame,
    RootRegionSpecialization const& specialization,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& plan = frame.plan;
    for (std::size_t step = 0; step < specialization.steps.size(); ++step) {
        auto const& root_step = plan.steps[specialization.steps[step]];
        auto const& primitive = plan.primitives[root_step.primitive];
        frame.specialization_steps[step] = RegionSpecializationStep{
            .node_data = primitive.node_data.data(),
            .context = reinterpret_cast<ReflectedNodeTickContext const*>(
                frame.scratch + primitive.context_offset),
            .operations_before = !root_step.before.empty()
                ? &root_step.before
                : nullptr,
            .operations_after = !root_step.after.empty()
                ? &root_step.after
                : nullptr,
            .primitive_index = root_step.primitive,
        };
    }
    for (std::size_t update = 0;
         update < specialization.begin_cursors.size(); ++update) {
        frame.specialization_begin_bases[update] =
            frame.sequential_pace_cursors[specialization.begin_cursors[update]];
    }
    for (std::size_t representation = 0;
         representation < specialization.sample_representations.size();
         ++representation) {
        frame.specialization_sample_representations[representation] =
            frame.sample_representations[
                specialization.sample_representations[representation]];
    }
    auto const project_begin = specialization.project_cursor == no_index
        ? std::uint64_t{0}
        : frame.sequential_pace_cursors[specialization.project_cursor];
    specialization.function(
        frame.specialization_steps.data(),
        frame.specialization_begin_bases.data(),
        frame.specialization_sample_representations.data(),
        specialization.sample_representations.data(),
        project_begin,
        frame.specialization_workspace.data(),
        index.bits(),
        block_size,
        &frame,
        &run_specialized_step_operations,
        &stage_specialized_manual_controls,
        &mark_specialized_sample_representation);
    for (auto const step : specialization.steps) {
        mark_primitive_outputs(frame, plan.primitives[plan.steps[step].primitive]);
    }
}
} // namespace

bool has_early_tick(RealtimeRootPlan const* plan) noexcept
{
    return plan && plan->first_foreground_group != 0;
}

std::size_t tick_transfer_size(RealtimeRootPlan const* plan) noexcept
{
    return plan ? plan->root_stack.transfer_size_bytes : 0;
}

void execute_tick_root_part(
    RealtimeRootPlan const* root,
    std::byte* storage_base,
    TickInvocationCall const* invocation,
    GlobalIndex sample_index,
    std::size_t block_size,
    TickRootPart part,
    std::span<std::byte> transfer) noexcept
{
    auto const& plan = *root;
    auto const& layout = plan.frame;
    auto const& storage = plan.sample_ports.storage;
    auto const activity_sample_count = plan.tracks_activity
        ? storage.representations.size()
        : std::size_t{0};
    auto const activity_event_count = plan.tracks_activity
        ? plan.event_ports.ports.size()
        : std::size_t{0};
    auto const activity_frontier_count = plan.tracks_activity
        ? plan.activity_frontiers.size()
        : std::size_t{0};
    auto const activity_batch_count = plan.tracks_activity
        ? plan.maximum_batch_lanes
        : std::size_t{0};
    IV_ASSERT(invocation != nullptr || !plan.has_tick_slots,
        "GraphJit Tick runtime slots require an invocation record");
    auto* allocation = static_cast<std::byte*>(
        alloca(layout.size + plan.scratch_alignment - 1));
    auto* scratch = reinterpret_cast<std::byte*>(align_up(
        reinterpret_cast<std::uintptr_t>(allocation), plan.scratch_alignment));

    RootFrame frame{
        .plan = plan,
        .storage_base = storage_base,
        .scratch = scratch,
        .sample_representations = frame_array<std::byte*>(
            scratch, layout.sample_representations, storage.representations.size()),
        .manual_control_latches = frame_array<ManualControlLatch>(scratch, layout.manual_control_latches, plan.sample_ports.manual_controls.size()),
        .event_representations = frame_array<std::byte*>(
            scratch, layout.event_representations, plan.event_ports.ports.size()),
        .producer_authored_extents = frame_array<std::size_t>(
            scratch, layout.authored_extents, storage.producer_authored_extents.size()),
        .sequential_pace_cursors = frame_array<std::uint64_t>(
            scratch, layout.pace_cursors, storage.sequential_pace_cursors.size()),
        .sample_representation_epochs = frame_array<std::size_t>(
            scratch, layout.sample_epochs, activity_sample_count),
        .event_representation_epochs = frame_array<std::size_t>(
            scratch, layout.event_epochs, activity_event_count),
        .continuous_activity_summaries = frame_array<ContinuousActivitySummary>(
            scratch, layout.continuous_summaries, activity_frontier_count),
        .event_activity_summaries = frame_array<EventActivitySummary>(
            scratch, layout.event_summaries, activity_event_count),
        .tick_lanes = frame_array<ReflectedNodeTickBatchLane>(
            scratch, layout.tick_lanes, plan.maximum_batch_lanes),
        .skip_lanes = frame_array<ReflectedNodeTickBatchLane>(
            scratch, layout.skip_lanes, activity_batch_count),
        .tick_decisions = frame_array<PrimitiveActivityDecision>(
            scratch, layout.tick_decisions, activity_batch_count),
        .tick_lane_primitives = frame_array<std::size_t>(
            scratch, layout.tick_lane_primitives, activity_batch_count),
        .specialization_steps = frame_array<RegionSpecializationStep>(
            scratch, layout.specialization_steps, plan.maximum_specialization_steps),
        .specialization_begin_bases = frame_array<std::uint64_t>(
            scratch,
            layout.specialization_begin_bases,
            plan.maximum_specialization_begin_indices),
        .specialization_sample_representations = frame_array<std::byte*>(
            scratch,
            layout.specialization_sample_representations,
            plan.maximum_specialization_sample_representations),
        .specialization_workspace = frame_array<std::byte>(
            scratch,
            layout.specialization_workspace,
            plan.maximum_specialization_workspace_size),
        .invocation = invocation,
        .root_index = sample_index,
    };
    std::ranges::fill(frame.sample_representation_epochs, 0);
    std::ranges::fill(frame.event_representation_epochs, 0);
    std::ranges::fill(frame.continuous_activity_summaries, ContinuousActivitySummary{});
    std::ranges::fill(frame.event_activity_summaries, EventActivitySummary{});
    for (std::size_t index = 0; index < plan.sample_addresses.size(); ++index) {
        frame.sample_representations[index] = resolve_address(
            plan.sample_addresses[index], storage_base, scratch);
    }
    for (std::size_t index = 0; index < plan.event_addresses.size(); ++index) {
        frame.event_representations[index] = resolve_address(
            plan.event_addresses[index], storage_base, scratch);
    }
    for (std::size_t index = 0; index < storage.producer_authored_extents.size(); ++index) {
        frame.producer_authored_extents[index] = load_raw<std::size_t>(
            storage_base, storage.producer_authored_extents[index].storage_offset);
    }
    for (std::size_t index = 0; index < storage.sequential_pace_cursors.size(); ++index) {
        frame.sequential_pace_cursors[index] = load_raw<std::uint64_t>(
            storage_base, storage.sequential_pace_cursors[index].storage_offset);
    }
    for (std::size_t index = 0; index < plan.sample_ports.manual_controls.size(); ++index) {
        auto const& control = plan.sample_ports.manual_controls[index];
        auto* state = reinterpret_cast<ManualControlRuntimeState*>(storage_base + control.state_storage_offset);
        auto const settled = std::bit_cast<std::uint32_t>(control.configured.settled_value.value);
        if (!control.configured.dynamic || !control.stages_timeline) {
            // Specialized steps can read an immutable input without staging a
            // timeline. Record its effective value for the next live kernel,
            // while preserving any target already submitted by the UI.
            state->effective_bits = settled;
        }
        if (!control.stages_timeline) continue;
        auto const target_index = plan.manual_target_latches[index];
        frame.manual_control_latches[index] = {
            .target_bits = !control.configured.dynamic ? settled : target_index < index
                ? frame.manual_control_latches[target_index].target_bits
                : std::atomic_ref<std::uint32_t>{state->target_bits}.load(std::memory_order_relaxed),
            .effective_bits = state->effective_bits,
        };
    }
    initialize_bindings(frame);
    initialize_tick_slots(frame);
    if (part != TickRootPart::complete)
        IV_ASSERT(transfer.size() == plan.root_stack.transfer_size_bytes, "early Tick transfer size changed");
    if (part == TickRootPart::foreground) {
        for (auto const& value : plan.root_stack.early_to_foreground)
            std::memcpy(scratch + value.scratch_offset, transfer.data() + value.transfer_offset, value.size_bytes);
    }

    auto const& execution = plan.execution;
    auto const first_group = part == TickRootPart::foreground ? plan.first_foreground_group : 0;
    auto const end_group = part == TickRootPart::early ? plan.first_foreground_group : execution.ready_groups.size();
    for (auto group_index = first_group; group_index < end_group; ++group_index) {
        auto const& group = execution.ready_groups[group_index];
        if (group.cyclic_regions.empty()) {
            auto const& acyclic = plan.acyclic_groups[group_index];
            for (auto const region_index : acyclic.specialized_regions) {
                run_region_specialization(
                    frame,
                    *plan.regions[region_index].specialization,
                    sample_index,
                    block_size);
            }
            auto const& steps = acyclic.interpreted_steps;
            if (steps.size() > 1) {
                run_step_batch(frame, steps, sample_index, block_size, acyclic.has_step_operations);
                continue;
            }
            for (auto const step : steps) {
                run_step(frame, step, sample_index, block_size, true);
            }
            continue;
        }
        auto const& first_region = execution.regions[group.cyclic_regions.front()];
        for (auto const region_index : group.cyclic_regions) {
            auto const& operations = plan.regions[region_index];
            run_operations(frame, operations.before, sample_index, block_size);
        }
        // The group's regions are independent, so each specialized region
        // runs its complete invocation in turn. Without a specialization for
        // every region, the remaining regions run their slices alone.
        auto const specialized = static_cast<std::size_t>(std::ranges::count_if(
            group.cyclic_regions,
            [&](std::size_t region_index) {
                return plan.regions[region_index].specialization.has_value();
            }));
        if (specialized != 0) {
            for (auto const region_index : group.cyclic_regions) {
                if (auto const& specialization =
                        plan.regions[region_index].specialization) {
                    run_region_specialization(
                        frame, *specialization, sample_index, block_size);
                } else {
                    run_region_slices(
                        frame,
                        execution.regions[region_index],
                        sample_index,
                        block_size);
                }
            }
            for (auto const region_index : group.cyclic_regions) {
                auto const& operations = plan.regions[region_index];
                run_operations(frame, operations.after, sample_index, block_size);
            }
            continue;
        }
        for (std::size_t offset = 0; offset < block_size;
             offset += first_region.maximum_block_size) {
            auto const slice = sample_index.advanced_by(offset);
            auto const count = std::min(
                first_region.maximum_block_size, block_size - offset);
            if (group.cyclic_regions.size() == 1) {
                for (auto const step : first_region.primitive_steps) {
                    run_step(frame, step, slice, count, false);
                }
                continue;
            }
            auto const lane_count = group.cyclic_regions.size();
            for (std::size_t phase = 0;
                 phase < first_region.primitive_steps.size();
                 ++phase) {
                run_step_batch(
                    frame,
                    std::span<std::size_t const>{group.primitive_steps}
                        .subspan(phase * lane_count, lane_count),
                    slice,
                    count,
                    true);
            }
        }
        for (auto const region_index : group.cyclic_regions) {
            auto const& operations = plan.regions[region_index];
            run_operations(frame, operations.after, sample_index, block_size);
        }
    }

    if (part == TickRootPart::early) {
        for (auto const& value : plan.root_stack.early_to_foreground)
            std::memcpy(transfer.data() + value.transfer_offset, scratch + value.scratch_offset, value.size_bytes);
        return;
    }

    for (std::size_t index = 0; index < storage.producer_authored_extents.size(); ++index) {
        auto const& extent = storage.producer_authored_extents[index];
        auto const current = std::min(
            frame.producer_authored_extents[index], extent.maximum_revision_frames);
        auto const projected = project_lattice(
            sample_index, block_size, extent.absolute_pace_exponent).value_count;
        store_raw<std::size_t>(
            storage_base,
            extent.storage_offset,
            current + std::min(projected, extent.maximum_revision_frames - current));
    }
    for (std::size_t index = 0; index < storage.sequential_pace_cursors.size(); ++index) {
        auto const& cursor = storage.sequential_pace_cursors[index];
        store_raw<std::uint64_t>(
            storage_base,
            cursor.storage_offset,
            frame.sequential_pace_cursors[index]
                + project_lattice(
                    sample_index, block_size, cursor.absolute_pace_exponent)
                    .value_count);
    }
}

void execute_realtime_root(RealtimeRootPlan const* root, std::byte* storage_base,
    TickInvocationCall const* invocation, GlobalIndex sample_index, std::size_t block_size) noexcept
{
    execute_tick_root_part(root, storage_base, invocation, sample_index, block_size, TickRootPart::complete, {});
}
} // namespace iv::graph_jit
