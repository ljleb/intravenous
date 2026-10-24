#include <intravenous/runtime/tick_invocation_frame.h>
#include <intravenous/runtime/realtime_produced_record.h>

#include <intravenous/graph_jit/background_storage_plan.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iv {
namespace {

static_assert(std::atomic<bool>::is_always_lock_free);

IndexCoverage const empty_tick_coverage{};

[[nodiscard]] bool capture_event_matches_type(
    EventTypeId type, Event const& event) noexcept
{
    switch (type) {
    case EventTypeId::empty:
        return std::holds_alternative<EmptyEvent>(event);
    case EventTypeId::trigger:
        return std::holds_alternative<TriggerEvent>(event);
    case EventTypeId::boundary:
        return std::holds_alternative<BoundaryEvent>(event);
    case EventTypeId::midi:
        return std::holds_alternative<MidiEvent>(event);
    case EventTypeId::count:
        break;
    }
    return false;
}

bool target_subset_matches(
    graph_jit::BackgroundEvaluationPlan const& plan,
    graph_jit::PortSubsetIndex subset,
    NodeBundlePortId port)
{
    return subset < plan.sample_target_subsets.size()
        && plan.sample_target_subsets[subset].port == port;
}

bool target_subset_matches_event(
    graph_jit::BackgroundEvaluationPlan const& plan,
    graph_jit::PortSubsetIndex subset,
    NodeBundlePortId port)
{
    if (subset >= plan.event_target_subsets.size()) return false;
    auto const& target = plan.event_target_subsets[subset].port;
    return target.bundle == port.node_bundle_handle
        && target.port == port.port_index;
}

PersistedOutputId capture_output_id(
    graph_jit::BackgroundEvaluationPlan const& plan,
    graph_jit::BackgroundPortIndex port,
    std::uint64_t generation)
{
    if (port >= plan.ports.size()) {
        throw std::invalid_argument(
            "Tick capture binding references a missing logical port");
    }
    auto const& output = plan.ports[port];
    if (output.stable_identity) return *output.stable_identity;
    return GenerationLocalPersistedOutputId{
        .generation = generation,
        .port = port,
        .kind = output.kind,
    };
}

[[nodiscard]] std::size_t capture_block_count(
    std::size_t value_count,
    std::size_t value_size,
    std::size_t block_payload_capacity)
{
    if (value_size == 0 || block_payload_capacity == 0
        || value_count > std::numeric_limits<std::size_t>::max()
                / value_size) {
        throw std::length_error("Tick capture payload is too large");
    }
    auto const bytes = value_count * value_size;
    return std::max<std::size_t>(
        1, bytes / block_payload_capacity
            + static_cast<std::size_t>(bytes % block_payload_capacity != 0));
}

[[nodiscard]] std::size_t queued_record_block_count(
    std::size_t payload_size,
    std::size_t block_storage_size)
{
    if (block_storage_size < sizeof(RealtimeProducedRecordHeader)
        || payload_size > std::numeric_limits<std::size_t>::max()
                - sizeof(RealtimeProducedRecordHeader)) {
        throw std::length_error("Realtime-produced record is too large");
    }
    return capture_block_count(
        sizeof(RealtimeProducedRecordHeader) + payload_size,
        1,
        block_storage_size);
}

[[nodiscard]] std::size_t queued_record_block_count_noexcept(
    std::size_t payload_size,
    std::size_t block_storage_size) noexcept
{
    if (block_storage_size < sizeof(RealtimeProducedRecordHeader)
        || payload_size > std::numeric_limits<std::size_t>::max()
                - sizeof(RealtimeProducedRecordHeader)) {
        return 0;
    }
    auto const bytes = sizeof(RealtimeProducedRecordHeader) + payload_size;
    return bytes / block_storage_size
        + static_cast<std::size_t>(bytes % block_storage_size != 0);
}

template<class Binding>
std::optional<PersistedOutputHandle> direct_persisted_sample_output(
    graph_jit::BackgroundEvaluationPlan const& plan,
    Binding const& binding,
    graph_jit::BackgroundPortPlan const& port,
    std::uint64_t generation,
    PersistedDataStore& data)
{
    std::optional<graph_jit::PortStorageIndex> persisted;
    for (auto const storage : binding.storage) {
        if (storage < plan.storage.ports.size()
            && plan.storage.ports[storage].storage
                == graph_jit::PortStorageKind::persisted_data) {
            if (persisted) return std::nullopt;
            persisted = storage;
        }
    }
    if (!persisted || binding.storage.size() != 1) return std::nullopt;

    auto const& storage = plan.storage.ports[*persisted];
    auto const channels = channel_count(port.sample_layout);
    bool identity = storage.sample_layout == port.sample_layout;
    for (std::size_t channel = 0; identity && channel < channels; ++channel) {
        auto const direct_count = std::ranges::count_if(
            plan.storage.direct_samples,
            [&](auto const& direct) {
                return direct.target_channel == channel
                    && target_subset_matches(
                        plan, direct.target_subset, port.configured_port);
            });
        identity = direct_count == 1 && std::ranges::any_of(
            plan.storage.direct_samples,
            [&](auto const& direct) {
                return direct.storage == *persisted
                    && direct.target_channel == channel
                    && direct.source_channel == channel
                    && direct.read_latency == 0
                    && target_subset_matches(
                        plan, direct.target_subset, port.configured_port);
            });
    }
    return identity
        ? std::optional<PersistedOutputHandle>{data.resolve_output(
            persisted_output_id(plan, *persisted, generation))}
        : std::nullopt;
}

template<class Binding>
std::optional<PersistedOutputHandle> direct_persisted_event_output(
    graph_jit::BackgroundEvaluationPlan const& plan,
    Binding const& binding,
    graph_jit::BackgroundPortPlan const& port,
    std::uint64_t generation,
    PersistedDataStore& data)
{
    std::optional<graph_jit::PortStorageIndex> persisted;
    for (auto const storage : binding.storage) {
        if (storage < plan.storage.ports.size()
            && plan.storage.ports[storage].storage
                == graph_jit::PortStorageKind::persisted_data) {
            if (persisted) return std::nullopt;
            persisted = storage;
        }
    }
    if (!persisted || binding.storage.size() != 1
        || plan.storage.ports[*persisted].event_type != port.event_type) {
        return std::nullopt;
    }

    auto const direct = std::ranges::count_if(
            plan.storage.direct_events,
            [&](auto const& candidate) {
                return target_subset_matches_event(
                    plan, candidate.target_subset, port.configured_port);
            }) == 1
        && std::ranges::any_of(
            plan.storage.direct_events,
            [&](auto const& candidate) {
                return candidate.storage == *persisted
                    && target_subset_matches_event(
                        plan, candidate.target_subset, port.configured_port);
            });
    return direct
        ? std::optional<PersistedOutputHandle>{data.resolve_output(
            persisted_output_id(plan, *persisted, generation))}
        : std::nullopt;
}

} // namespace

class TickInvocationWorkspace::Impl {
public:
    struct SampleCaptureSlot {
        ProducerReserve* reserve = nullptr;
        PendingQueue* pending = nullptr;
        std::atomic<bool>* reservation_failed = nullptr;
        ProducedBlockChain callback_chain{};
        ChannelLayout layout{};
        std::size_t history = 0;
        std::size_t latency = 0;

        [[nodiscard]] graph_jit::TickSampleCaptureOperation operation() noexcept
        {
            return {
                .context = this,
                .capture = +[](
                    void* opaque,
                    ReflectedContinuousOutputPortBinding const* output,
                    GlobalIndex sample_index,
                    std::size_t block_size) noexcept {
                    if (!output) return;
                    static_cast<SampleCaptureSlot*>(opaque)->capture(
                        *output, sample_index, block_size);
                },
            };
        }

        void fail_queue_reservation() noexcept
        {
            if (reservation_failed) {
                reservation_failed->store(true, std::memory_order_release);
            }
        }

        void publish_callback_chain() noexcept
        {
            if (!callback_chain) return;
            auto const published = pending
                && pending->publish(std::move(callback_chain));
            assert(published);
            if (!published) fail_queue_reservation();
        }

        void capture(
            ReflectedContinuousOutputPortBinding const& binding,
            GlobalIndex sample_index,
            std::size_t block_size) noexcept
        {
            if (binding.disposition == OutputDisposition::untouched) return;
            if (!reserve || !pending
                || binding.storage.channel_layout != layout
                || binding.history != history || binding.latency != latency
                || binding.absolute_pace_exponent != 0
                || binding.storage.geometry.value_type
                    != ContinuousValueTypeId::sample
                || binding.storage.geometry.value_size_bytes != sizeof(Sample)
                || binding.storage.value_capacity == 0
                || !std::has_single_bit(binding.storage.value_capacity)
                || binding.storage.storage_latency
                    >= binding.storage.value_capacity) {
                return;
            }
            auto const channels = channel_count(layout);
            if (channels == 0 || channels > maximum_supported_channel_count
                || channels > std::numeric_limits<std::size_t>::max()
                        / sizeof(Sample)) {
                return;
            }
            auto const bytes_per_frame = channels * sizeof(Sample);
            for (std::size_t channel = 0; channel < channels; ++channel) {
                auto const& source = binding.storage.channels[channel];
                if (!source.storage || source.value_capacity == 0
                    || !std::has_single_bit(source.value_capacity)
                    || source.value_stride_bytes < sizeof(Sample)
                    || source.value_delay >= source.value_capacity
                    || binding.storage.storage_latency
                        >= source.value_capacity) {
                    return;
                }
            }

            auto const window = realtime_port_window(
                sample_index, block_size, 0, 0);
            auto const selected_count = static_cast<std::size_t>(window.extent);
            if (selected_count == 0
                || static_cast<GlobalIndex::extent_type>(selected_count)
                    != window.extent
                || selected_count > std::numeric_limits<std::size_t>::max()
                        / bytes_per_frame) {
                return;
            }
            if (binding.disposition == OutputDisposition::voided) {
                auto const block_count = queued_record_block_count_noexcept(
                    0, reserve->block_storage_size());
                if (block_count == 0) {
                    fail_queue_reservation();
                    return;
                }
                auto chain = reserve->acquire(block_count);
                if (!chain) {
                    fail_queue_reservation();
                    return;
                }
                auto writer = chain.writer();
                auto const header = RealtimeProducedRecordHeader{
                    .payload_kind = RealtimeProducedPayloadKind::void_value,
                    .record_block_count = block_count,
                    .payload_size = 0,
                    .begin = window.begin,
                    .sample_count = selected_count,
                    .sample_layout = layout,
                };
                if (!writer.append(std::as_bytes(
                        std::span{&header, std::size_t{1}}))) {
                    fail_queue_reservation();
                    return;
                }
                auto const appended = callback_chain.append(std::move(chain));
                assert(appended);
                if (!appended) fail_queue_reservation();
                return;
            }
            auto append_values = [&](auto& writer) noexcept {
                auto append_sample = [&](std::size_t frame,
                                         std::size_t channel) noexcept {
                    auto const& source = binding.storage.channels[channel];
                    auto const delayed = binding.sequential_begin_ordinal
                        + binding.storage.storage_latency + frame
                        - source.value_delay;
                    auto const source_frame = static_cast<std::size_t>(
                        delayed & (source.value_capacity - 1));
                    Sample value{};
                    std::memcpy(
                        &value,
                        source.storage
                            + source_frame * source.value_stride_bytes,
                        sizeof(value));
                    return writer.append(
                        std::as_bytes(std::span{&value, std::size_t{1}}));
                };
                if (layout.sample_layout == SampleStreamLayout::planar) {
                    for (std::size_t channel = 0;
                         channel < channels; ++channel) {
                        for (std::size_t frame = 0;
                             frame < selected_count; ++frame) {
                            if (!append_sample(frame, channel)) return false;
                        }
                    }
                } else {
                    for (std::size_t frame = 0;
                         frame < selected_count; ++frame) {
                        for (std::size_t channel = 0;
                             channel < channels; ++channel) {
                            if (!append_sample(frame, channel)) return false;
                        }
                    }
                }
                return true;
            };

            auto const payload_size = selected_count * bytes_per_frame;
            auto const block_count = queued_record_block_count_noexcept(
                payload_size, reserve->block_storage_size());
            if (block_count == 0) {
                fail_queue_reservation();
                return;
            }
            auto chain = reserve->acquire(block_count);
            if (!chain) {
                fail_queue_reservation();
                return;
            }
            auto writer = chain.writer();
            auto const header = RealtimeProducedRecordHeader{
                .payload_kind = RealtimeProducedPayloadKind::samples,
                .record_block_count = block_count,
                .payload_size = payload_size,
                .begin = window.begin,
                .sample_count = selected_count,
                .sample_layout = layout,
            };
            if (!writer.append(std::as_bytes(
                    std::span{&header, std::size_t{1}}))
                || !append_values(writer)) {
                fail_queue_reservation();
                return;
            }
            auto const appended = callback_chain.append(std::move(chain));
            assert(appended);
            if (!appended) fail_queue_reservation();
        }
    };

    struct EventCaptureSlot {
        ProducerReserve* reserve = nullptr;
        PendingQueue* pending = nullptr;
        std::atomic<bool>* reservation_failed = nullptr;
        ProducedBlockChain callback_chain{};
        EventTypeId type = EventTypeId::empty;
        std::size_t history = 0;
        std::size_t latency = 0;

        [[nodiscard]] graph_jit::TickEventCaptureOperation operation() noexcept
        {
            return {
                .context = this,
                .capture = +[](
                    void* opaque,
                    ReflectedEventOutputPortBinding const* output,
                    GlobalIndex sample_index,
                    std::size_t block_size) noexcept {
                    if (!output) return;
                    static_cast<EventCaptureSlot*>(opaque)->capture(
                        *output, sample_index, block_size);
                },
            };
        }

        void fail_queue_reservation() noexcept
        {
            if (reservation_failed) {
                reservation_failed->store(true, std::memory_order_release);
            }
        }

        void publish_callback_chain() noexcept
        {
            if (!callback_chain) return;
            auto const published = pending
                && pending->publish(std::move(callback_chain));
            assert(published);
            if (!published) fail_queue_reservation();
        }

        void capture(
            ReflectedEventOutputPortBinding const& binding,
            GlobalIndex sample_index,
            std::size_t block_size) noexcept
        {
            if (binding.disposition == OutputDisposition::untouched) return;
            auto const& storage = binding.storage;
            if (!reserve || !pending || storage.storage == nullptr
                || storage.type != type
                || binding.history != history || binding.latency != latency
                || storage.event_capacity == 0
                || !std::has_single_bit(storage.event_capacity)) {
                return;
            }
            auto const window = realtime_port_window(
                sample_index, block_size, 0, 0);
            auto const selected_window_size =
                static_cast<std::size_t>(window.extent);
            if (selected_window_size == 0
                || static_cast<GlobalIndex::extent_type>(selected_window_size)
                    != window.extent) {
                return;
            }
            if (binding.disposition == OutputDisposition::voided) {
                auto const block_count = queued_record_block_count_noexcept(
                    0, reserve->block_storage_size());
                if (block_count == 0) {
                    fail_queue_reservation();
                    return;
                }
                auto chain = reserve->acquire(block_count);
                if (!chain) {
                    fail_queue_reservation();
                    return;
                }
                auto writer = chain.writer();
                auto const header = RealtimeProducedRecordHeader{
                    .payload_kind = RealtimeProducedPayloadKind::void_value,
                    .record_block_count = block_count,
                    .payload_size = 0,
                    .begin = window.begin,
                    .sample_count = selected_window_size,
                    .event_type = type,
                    .event_count = 0,
                };
                if (!writer.append(std::as_bytes(
                        std::span{&header, std::size_t{1}}))) {
                    fail_queue_reservation();
                    return;
                }
                auto const appended = callback_chain.append(std::move(chain));
                assert(appended);
                if (!appended) fail_queue_reservation();
                return;
            }

            auto const* events = reinterpret_cast<TimedEvent const*>(
                storage.storage + storage.events_offset);
            std::size_t read = 0;
            std::size_t write = *reinterpret_cast<std::size_t const*>(
                storage.storage + storage.count_offset);
            if (storage.persistent_ring) {
                read = *reinterpret_cast<std::size_t const*>(
                    storage.storage + storage.read_index_offset);
                write = *reinterpret_cast<std::size_t const*>(
                    storage.storage + storage.write_index_offset);
            }
            if (write < read) return;
            auto const available = std::min(
                write - read, storage.event_capacity);

            std::size_t selected_begin = available;
            std::size_t selected_count = 0;
            GlobalIndex::extent_type previous_offset = 0;
            bool first_selected = true;
            for (std::size_t index = 0; index < available; ++index) {
                auto const& event = events[
                    (read + index) & (storage.event_capacity - 1)];
                auto const offset = window.offset_of(event.time);
                if (!offset) continue;
                if (!capture_event_matches_type(type, event.value)
                    || (!first_selected && *offset < previous_offset)) {
                    return;
                }
                if (first_selected) selected_begin = index;
                previous_offset = *offset;
                first_selected = false;
                ++selected_count;
            }
            if (selected_count > std::numeric_limits<std::size_t>::max()
                    / sizeof(TimedEvent)) {
                return;
            }
            auto append_values = [&](auto& writer) noexcept {
                for (std::size_t offset = 0;
                     offset < selected_count; ++offset) {
                    auto const index = selected_begin + offset;
                    auto const& event = events[
                        (read + index) & (storage.event_capacity - 1)];
                    if (!writer.append(std::as_bytes(
                            std::span{&event, std::size_t{1}}))) {
                        return false;
                    }
                }
                return true;
            };
            auto const payload_size = selected_count * sizeof(TimedEvent);
            auto const block_count = queued_record_block_count_noexcept(
                payload_size, reserve->block_storage_size());
            if (block_count == 0) {
                fail_queue_reservation();
                return;
            }
            auto chain = reserve->acquire(block_count);
            if (!chain) {
                fail_queue_reservation();
                return;
            }
            auto writer = chain.writer();
            auto const header = RealtimeProducedRecordHeader{
                .payload_kind = RealtimeProducedPayloadKind::events,
                .record_block_count = block_count,
                .payload_size = payload_size,
                .begin = window.begin,
                .sample_count = selected_window_size,
                .event_type = type,
                .event_count = selected_count,
            };
            if (!writer.append(std::as_bytes(
                    std::span{&header, std::size_t{1}}))
                || !append_values(writer)) {
                fail_queue_reservation();
                return;
            }
            auto const appended = callback_chain.append(std::move(chain));
            assert(appended);
            if (!appended) fail_queue_reservation();
        }
    };

    struct SequentialSampleSlot {
        graph_jit::BackgroundPortIndex port = 0;
        std::optional<PersistedOutputHandle> output{};
        bool accepts_materialization = false;
        ChannelLayout layout{};
        std::size_t capacity = 0;
        Sample neutral{};
        // Frame f holds the current window's position sample_index + f. The
        // consumer's history is node-owned realtime state, so the window never
        // reaches behind the current block and its ordinal stays zero.
        std::vector<Sample> values{};
        ReflectedContinuousInputPortBinding binding{};

        void initialize_storage()
        {
            auto const channels = channel_count(layout);
            if (channels == 0 || channels > maximum_supported_channel_count
                || capacity == 0 || !std::has_single_bit(capacity)) {
                throw std::invalid_argument(
                    "Tick Sequential sample binding has an invalid layout");
            }
            if (capacity > std::numeric_limits<std::size_t>::max() / channels) {
                throw std::length_error(
                    "Tick Sequential sample playback storage is too large");
            }
            values.assign(capacity * channels, neutral);
            binding = {
                .storage = {
                    .geometry = *continuous_value_storage_geometry(
                        ContinuousValueTypeId::sample, std::nullopt),
                    .value_capacity = capacity,
                    .storage_latency = 0,
                    .channel_layout = layout,
                },
                .history = 0,
                .read_latency = 0,
            };
            for (std::size_t channel = 0; channel < channels; ++channel) {
                auto const planar = layout.sample_layout
                    == SampleStreamLayout::planar;
                binding.storage.channels[channel] = {
                    .storage = reinterpret_cast<std::byte*>(
                        values.data() + (planar ? channel * capacity : channel)),
                    .value_capacity = capacity,
                    .value_stride_bytes =
                        (planar ? 1 : channels) * sizeof(Sample),
                    .value_delay = 0,
                };
            }
        }

        void clear(std::size_t frames) noexcept
        {
            auto const channels = channel_count(layout);
            if (layout.sample_layout == SampleStreamLayout::planar) {
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    std::fill_n(
                        values.begin() + channel * capacity, frames, neutral);
                }
            } else {
                std::fill_n(values.begin(), frames * channels, neutral);
            }
        }

        void copy(
            PersistedSampleRegion const& region,
            GlobalIndex window_begin,
            GlobalIndexRange intersection) noexcept
        {
            if (region.layout != layout) return;
            auto const frames = static_cast<std::size_t>(intersection.extent);
            auto const source = static_cast<std::size_t>(
                region.range.begin.forward_distance_to(intersection.begin));
            auto const destination = static_cast<std::size_t>(
                window_begin.forward_distance_to(intersection.begin));
            auto const region_frames = static_cast<std::size_t>(
                region.range.extent);
            auto const channels = channel_count(layout);
            if (layout.sample_layout == SampleStreamLayout::planar) {
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    std::copy_n(
                        region.values.begin()
                            + channel * region_frames + source,
                        frames,
                        values.begin() + channel * capacity + destination);
                }
            } else {
                std::copy_n(
                    region.values.begin() + source * channels,
                    frames * channels,
                    values.begin() + destination * channels);
            }
        }

        void populate(
            PersistedDataStore::Snapshot const& published,
            TickMaterializedSampleInput const* materialized,
            GlobalIndex sample_index,
            std::size_t block_size) noexcept
        {
            auto const requested = GlobalIndexRange{
                .begin = sample_index,
                .extent = block_size,
            };
            clear(block_size);
            if (output) {
                published.for_each_sample_region(
                    *output,
                    requested,
                    [&](PersistedSampleRegion const& region,
                        GlobalIndexRange intersection) {
                        copy(region, sample_index, intersection);
                    });
            }

            if (!materialized) return;
            materialized->for_each_region(
                requested,
                [&](TickMaterializedSampleRegion const& region,
                    GlobalIndexRange intersection) {
                    copy(*materialized, region, sample_index, intersection);
                });
        }

        // The caller selected only complete materializations, so every
        // logical channel has a slot in region.
        void copy(
            TickMaterializedSampleInput const& materialized,
            TickMaterializedSampleRegion const& region,
            GlobalIndex window_begin,
            GlobalIndexRange intersection) noexcept
        {
            auto const frames = static_cast<std::size_t>(intersection.extent);
            auto const source = static_cast<std::size_t>(
                region.range.begin.forward_distance_to(intersection.begin));
            auto const destination = static_cast<std::size_t>(
                window_begin.forward_distance_to(intersection.begin));
            auto const channels = channel_count(layout);
            for (std::size_t channel = 0; channel < channels; ++channel) {
                auto const* frames_begin = region.channel_frames(
                    materialized.channel_slot(channel)) + source;
                if (layout.sample_layout == SampleStreamLayout::planar) {
                    std::copy_n(
                        frames_begin,
                        frames,
                        values.begin() + channel * capacity + destination);
                } else {
                    for (std::size_t frame = 0; frame < frames; ++frame) {
                        values[(destination + frame) * channels + channel] =
                            frames_begin[frame];
                    }
                }
            }
        }
    };

    struct SequentialEventSlot {
        graph_jit::BackgroundPortIndex port = 0;
        std::optional<PersistedOutputHandle> output{};
        bool accepts_materialization = false;
        EventTypeId type = EventTypeId::empty;
        std::size_t events_offset = 0;
        std::size_t capacity = 0;
        std::vector<std::max_align_t> words{};
        std::vector<std::size_t> semantic_source_indices{};
        ReflectedEventInputPortBinding binding{};

        void initialize_storage()
        {
            static_assert(alignof(TimedEvent) <= alignof(std::max_align_t));
            if (capacity == 0 || !std::has_single_bit(capacity)
                || type >= EventTypeId::count) {
                throw std::invalid_argument(
                    "Tick Sequential event binding has an invalid layout");
            }
            auto const alignment = alignof(TimedEvent);
            auto const remainder = sizeof(std::size_t) % alignment;
            events_offset = remainder == 0
                ? sizeof(std::size_t)
                : sizeof(std::size_t) + alignment - remainder;
            if (capacity > (std::numeric_limits<std::size_t>::max()
                    - events_offset) / sizeof(TimedEvent)) {
                throw std::length_error(
                    "Tick Sequential event playback storage is too large");
            }
            auto const bytes = events_offset + capacity * sizeof(TimedEvent);
            if (bytes > std::numeric_limits<std::size_t>::max()
                    - (sizeof(std::max_align_t) - 1)) {
                throw std::length_error(
                    "Tick Sequential event playback storage is too large");
            }
            words.resize((bytes + sizeof(std::max_align_t) - 1)
                         / sizeof(std::max_align_t));
            semantic_source_indices.resize(
                capacity, std::numeric_limits<std::size_t>::max());
            count() = 0;
            binding = {
                .storage = {
                    .storage = data(),
                    .count_offset = 0,
                    .events_offset = events_offset,
                    .event_capacity = capacity,
                    .type = type,
                },
                .semantic_source_indices = semantic_source_indices.data(),
            };
        }

        [[nodiscard]] std::byte* data() noexcept
        {
            return reinterpret_cast<std::byte*>(words.data());
        }

        [[nodiscard]] std::size_t& count() noexcept
        {
            return *reinterpret_cast<std::size_t*>(data());
        }

        [[nodiscard]] TimedEvent* events() noexcept
        {
            return reinterpret_cast<TimedEvent*>(data() + events_offset);
        }

        void append(
            TimedEvent const& event,
            std::size_t semantic_source_index =
                std::numeric_limits<std::size_t>::max()) noexcept
        {
            auto& size = count();
            if (size == capacity) return;
            events()[size] = event;
            semantic_source_indices[size] = semantic_source_index;
            ++size;
        }

        void populate(
            PersistedDataStore::Snapshot const& published,
            TickMaterializedEventInput const* materialized,
            GlobalIndex sample_index,
            std::size_t block_size) noexcept
        {
            count() = 0;
            // Only the current window: the consumer owns its event history.
            auto const range = GlobalIndexRange{
                .begin = sample_index,
                .extent = block_size,
            };

            if (materialized) {
                materialized->for_each_indexed(
                    range,
                    this,
                    +[](void* opaque,
                        TimedEvent const& event,
                        std::size_t source_index) {
                        static_cast<SequentialEventSlot*>(opaque)->append(
                            event, source_index);
                    });
                return;
            }
            if (!output) return;
            published.for_each_event_chunk(
                *output,
                range,
                [&](PersistedEventChunk const& chunk,
                    GlobalIndexRange intersection) {
                    if (chunk.type != type) return;
                    auto event = std::ranges::lower_bound(
                        chunk.events,
                        intersection.begin,
                        SignedGlobalIndexOrder{},
                        &TimedEvent::time);
                    for (; event != chunk.events.end()
                           && intersection.contains(event->time); ++event) {
                        append(*event);
                    }
                });
        }
    };

    struct SampleSlot {
        PersistedDataStore::Snapshot const* snapshot = nullptr;
        TickMaterializedSampleInput const* materialized = nullptr;
        std::optional<PersistedOutputHandle> output{};
        graph_jit::BackgroundPortIndex port = 0;
        bool accepts_materialization = false;
        ChannelLayout layout{};
        IndexCoverage const* coverage = &empty_tick_coverage;

        [[nodiscard]] Sample read(
            GlobalIndex index, std::size_t channel) const noexcept
        {
            if (materialized && materialized->coverage.contains(index)
                && materialized->has_channel(channel)) {
                return materialized->at(index, channel);
            }
            if (!snapshot || !output) return {};
            auto const* region = snapshot->find_sample_region(*output, index);
            if (!region || region->layout != layout) {
                return {};
            }
            return region->at(index, channel);
        }
    };

    struct EventSlot {
        PersistedDataStore::Snapshot const* snapshot = nullptr;
        TickMaterializedEventInput const* materialized = nullptr;
        std::optional<PersistedOutputHandle> output{};
        graph_jit::BackgroundPortIndex port = 0;
        bool accepts_materialization = false;
        EventTypeId type = EventTypeId::empty;
        IndexCoverage const* coverage = &empty_tick_coverage;

        void for_each(
            GlobalIndexRange range,
            void* visitor_data,
            RandomAccessEventInputPort::VisitEvent visitor) const
        {
            if (materialized) {
                materialized->for_each(
                    range, visitor_data, visitor);
                return;
            }
            if (!snapshot || !output || range.empty()) return;
            snapshot->for_each_event_chunk(
                *output,
                range,
                [&](PersistedEventChunk const& chunk,
                    GlobalIndexRange intersection) {
                    if (chunk.type != type) return;
                    auto event = std::ranges::lower_bound(
                        chunk.events,
                        intersection.begin,
                        SignedGlobalIndexOrder{},
                        &TimedEvent::time);
                    for (; event != chunk.events.end()
                           && intersection.contains(event->time); ++event) {
                        visitor(visitor_data, *event);
                    }
                });
        }
    };

    std::vector<SequentialSampleSlot> sequential_sample_slots{};
    std::vector<ReflectedContinuousInputPortBinding> sequential_sample_views{};
    std::vector<SequentialEventSlot> sequential_event_slots{};
    std::vector<ReflectedEventInputPortBinding> sequential_event_views{};
    std::vector<SampleSlot> sample_slots{};
    std::vector<EventSlot> event_slots{};
    std::vector<RandomAccessSampleInputPort> sample_views{};
    std::vector<RandomAccessEventInputPort> event_views{};
    std::vector<SampleCaptureSlot> sample_capture_slots{};
    std::vector<EventCaptureSlot> event_capture_slots{};
    std::vector<graph_jit::TickSampleCaptureOperation> sample_captures{};
    std::vector<graph_jit::TickEventCaptureOperation> event_captures{};
    std::vector<RealtimeProducerRequirement> producer_requirements{};
    std::vector<std::uint8_t> producer_bounds_compatible{};
    std::uint64_t generation = 0;
    std::size_t maximum_block_size = 0;

    Impl(
        graph_jit::BackgroundEvaluationPlan const& plan,
        std::uint64_t selected_generation,
        PersistedDataStore& data,
        std::size_t selected_maximum_block_size)
        : generation(selected_generation)
        , maximum_block_size(selected_maximum_block_size)
    {
        if (maximum_block_size == 0) {
            throw std::invalid_argument(
                "Tick invocation maximum block size must be non-zero");
        }
        auto valid_runtime =
            graph_jit::detail::validate_tick_runtime_plan(plan);
        if (!valid_runtime) {
            throw std::invalid_argument(
                std::string{"invalid Tick invocation runtime plan: "}
                + valid_runtime.error());
        }
        auto const& runtime = plan.tick_runtime;
        sequential_sample_slots.resize(runtime.sequential_sample_inputs.size());
        sequential_sample_views.resize(sequential_sample_slots.size());
        sequential_event_slots.resize(runtime.sequential_event_inputs.size());
        sequential_event_views.resize(sequential_event_slots.size());
        sample_slots.resize(runtime.random_access_sample_inputs.size());
        event_slots.resize(runtime.random_access_event_inputs.size());
        sample_views.resize(sample_slots.size());
        event_views.resize(event_slots.size());
        sample_capture_slots.resize(runtime.sample_captures.size());
        event_capture_slots.resize(runtime.event_captures.size());
        sample_captures.resize(runtime.sample_captures.size());
        event_captures.resize(runtime.event_captures.size());
        producer_requirements.reserve(
            runtime.sample_captures.size() + runtime.event_captures.size());
        producer_bounds_compatible.reserve(
            runtime.sample_captures.size() + runtime.event_captures.size());

        for (std::size_t slot = 0;
             slot < sequential_sample_slots.size(); ++slot) {
            auto const& planned = runtime.sequential_sample_inputs[slot];
            if (planned.port >= plan.ports.size()) {
                throw std::invalid_argument(
                    "Tick Sequential sample binding references a missing "
                    "logical port");
            }
            auto const& port = plan.ports[planned.port];
            if (port.kind != PortKind::sample
                || port.direction != graph_jit::PortDirection::input) {
                throw std::invalid_argument(
                    "Tick Sequential sample binding references a non-sample "
                    "input port");
            }
            auto const largest_power_of_two = std::size_t{1}
                << (std::numeric_limits<std::size_t>::digits - 1);
            if (maximum_block_size > largest_power_of_two) {
                throw std::length_error(
                    "Tick Sequential sample playback capacity is too large");
            }

            auto& selected = sequential_sample_slots[slot];
            selected.port = planned.port;
            selected.layout = port.sample_layout;
            selected.capacity = std::bit_ceil(maximum_block_size);
            selected.neutral = port.sample_neutral_value;
            selected.accepts_materialization = std::ranges::any_of(
                planned.storage, [&](auto const storage) {
                    return storage < plan.storage.ports.size()
                        && (plan.storage.ports[storage].storage
                                == graph_jit::PortStorageKind::tick_sequential
                            || plan.storage.ports[storage].storage
                                == graph_jit::PortStorageKind::tick_random_access);
                });
            selected.output = direct_persisted_sample_output(
                plan, planned, port, selected_generation, data);
            selected.initialize_storage();
            sequential_sample_views[slot] = selected.binding;
        }

        for (std::size_t slot = 0;
             slot < sequential_event_slots.size(); ++slot) {
            auto const& planned = runtime.sequential_event_inputs[slot];
            if (planned.port >= plan.ports.size()) {
                throw std::invalid_argument(
                    "Tick Sequential event binding references a missing "
                    "logical port");
            }
            auto const& port = plan.ports[planned.port];
            if (port.kind != PortKind::event
                || port.direction != graph_jit::PortDirection::input) {
                throw std::invalid_argument(
                    "Tick Sequential event binding references a non-event "
                    "input port");
            }

            double maximum_rate = 0.0;
            for (auto const storage : planned.storage) {
                if (storage >= plan.storage.ports.size()) continue;
                auto const& candidate = plan.storage.ports[storage];
                if (candidate.kind != PortKind::event
                    || candidate.event_type != port.event_type
                    || !is_valid_event_buffer_rate(
                        candidate.max_events_per_index)) {
                    throw std::invalid_argument(
                        "Tick Sequential event binding has incompatible "
                        "storage");
                }
                maximum_rate = std::max(
                    maximum_rate, candidate.max_events_per_index);
            }
            auto const selected_capacity =
                event_sequence_capacity_for_sample_span(
                    maximum_rate, maximum_block_size);
            if (!selected_capacity) {
                throw std::length_error(
                    "Tick Sequential event playback capacity is too large");
            }

            auto& selected = sequential_event_slots[slot];
            selected.port = planned.port;
            selected.type = port.event_type;
            selected.capacity = std::max<std::size_t>(*selected_capacity, 1);
            selected.accepts_materialization = std::ranges::any_of(
                planned.storage, [&](auto const storage) {
                    return storage < plan.storage.ports.size()
                        && (plan.storage.ports[storage].storage
                                == graph_jit::PortStorageKind::tick_sequential
                            || plan.storage.ports[storage].storage
                                == graph_jit::PortStorageKind::tick_random_access);
                });
            selected.output = direct_persisted_event_output(
                plan, planned, port, selected_generation, data);
            selected.initialize_storage();
            sequential_event_views[slot] = selected.binding;
        }

        for (std::size_t slot = 0; slot < sample_slots.size(); ++slot) {
            auto const& binding = runtime.random_access_sample_inputs[slot];
            if (binding.port >= plan.ports.size()) {
                throw std::invalid_argument(
                    "Tick sample binding references a missing logical port");
            }
            auto const& port = plan.ports[binding.port];
            auto& selected = sample_slots[slot];
            selected.port = binding.port;
            selected.layout = port.sample_layout;
            selected.accepts_materialization = std::ranges::any_of(
                binding.storage, [&](auto const storage) {
                    return storage < plan.storage.ports.size()
                        && plan.storage.ports[storage].storage
                            == graph_jit::PortStorageKind::tick_random_access;
                });

            selected.output = direct_persisted_sample_output(
                plan, binding, port, selected_generation, data);
            sample_views[slot] = RandomAccessSampleInputPort{
                .data = &selected,
                .coverage_value = &empty_tick_coverage,
                .read_sample = +[](void const* opaque,
                                   GlobalIndex index,
                                   std::size_t channel) noexcept {
                    return static_cast<SampleSlot const*>(opaque)->read(
                        index, channel);
                },
            };
        }

        for (std::size_t slot = 0; slot < event_slots.size(); ++slot) {
            auto const& binding = runtime.random_access_event_inputs[slot];
            if (binding.port >= plan.ports.size()) {
                throw std::invalid_argument(
                    "Tick event binding references a missing logical port");
            }
            auto const& port = plan.ports[binding.port];
            auto& selected = event_slots[slot];
            selected.port = binding.port;
            selected.type = port.event_type;
            selected.accepts_materialization = std::ranges::any_of(
                binding.storage, [&](auto const storage) {
                    return storage < plan.storage.ports.size()
                        && plan.storage.ports[storage].storage
                            == graph_jit::PortStorageKind::tick_random_access;
                });

            selected.output = direct_persisted_event_output(
                plan, binding, port, selected_generation, data);
            event_views[slot] = RandomAccessEventInputPort{
                .data = &selected,
                .coverage_value = &empty_tick_coverage,
                .for_each_event = +[](
                    void const* opaque,
                    GlobalIndexRange range,
                    void* visitor_data,
                    RandomAccessEventInputPort::VisitEvent visitor) {
                    static_cast<EventSlot const*>(opaque)->for_each(
                        range, visitor_data, visitor);
                },
            };
        }

        auto capture_requirement = [&](auto const& planned,
                                       std::size_t blocks_per_invocation) {
            if (planned.maximum_block_size == 0
                || planned.maximum_invocations_per_callback == 0) {
                throw std::invalid_argument(
                    "Tick capture has invalid callback invocation bounds");
            }
            if (blocks_per_invocation
                > std::numeric_limits<std::size_t>::max()
                    / planned.maximum_invocations_per_callback) {
                throw std::length_error(
                    "Tick capture callback reserve is too large");
            }
            auto const required = blocks_per_invocation
                * planned.maximum_invocations_per_callback;
            return required;
        };
        auto capture_bounds_match_workspace = [&](auto const& planned) {
            return planned.maximum_block_size != 0
                && planned.maximum_block_size <= maximum_block_size
                && planned.maximum_invocations_per_callback != 0
                && planned.maximum_invocations_per_callback
                    == std::size_t{1} + (maximum_block_size - 1)
                        / planned.maximum_block_size;
        };
        for (std::size_t slot = 0;
             slot < sample_capture_slots.size(); ++slot) {
            auto const& planned = runtime.sample_captures[slot];
            if (planned.port >= plan.ports.size()) {
                throw std::invalid_argument(
                    "Tick sample capture references a missing logical port");
            }
            auto const& port = plan.ports[planned.port];
            if (port.kind != PortKind::sample
                || port.direction != graph_jit::PortDirection::output
                || !port.persisted_tick_output
                || port.retention != OutputRetention::persisted) {
                throw std::invalid_argument(
                    "Tick sample capture references a non-persisted output");
            }
            auto const bounds_compatible =
                capture_bounds_match_workspace(planned);
            auto const output = data.resolve_output(capture_output_id(
                plan, planned.port, selected_generation));
            auto& selected = sample_capture_slots[slot];
            selected = SampleCaptureSlot{
                .layout = port.sample_layout,
                .history = port.output_history,
                .latency = port.output_latency,
            };
            auto const window = planned.maximum_block_size;
            auto const channels = channel_count(port.sample_layout);
            if (channels == 0
                || channels > std::numeric_limits<std::size_t>::max()
                        / sizeof(Sample)) {
                throw std::length_error(
                    "Tick sample capture layout is too large");
            }
            auto const bytes_per_frame = channels * sizeof(Sample);
            if (window > std::numeric_limits<std::size_t>::max()
                    / bytes_per_frame) {
                throw std::length_error(
                    "Tick sample capture payload is too large");
            }
            auto const payload_size = window * bytes_per_frame;
            auto const required = capture_requirement(
                planned,
                queued_record_block_count(
                    payload_size, realtime_produced_block_storage_size));
            producer_requirements.push_back({
                .output = output,
                .port = planned.port,
                .kind = PortKind::sample,
                .maximum_blocks_per_callback = required,
            });
            producer_bounds_compatible.push_back(bounds_compatible);
        }
        for (std::size_t slot = 0;
             slot < event_capture_slots.size(); ++slot) {
            auto const& planned = runtime.event_captures[slot];
            if (planned.port >= plan.ports.size()) {
                throw std::invalid_argument(
                    "Tick event capture references a missing logical port");
            }
            auto const& port = plan.ports[planned.port];
            if (port.kind != PortKind::event
                || port.direction != graph_jit::PortDirection::output
                || !port.persisted_tick_output
                || port.retention != OutputRetention::persisted) {
                throw std::invalid_argument(
                    "Tick event capture references a non-persisted output");
            }
            auto const bounds_compatible =
                capture_bounds_match_workspace(planned);
            auto const output = data.resolve_output(capture_output_id(
                plan, planned.port, selected_generation));
            auto& selected = event_capture_slots[slot];
            selected = EventCaptureSlot{
                .type = port.event_type,
                .history = port.output_history,
                .latency = port.output_latency,
            };
            auto const window = planned.maximum_block_size;
            auto const event_capacity = event_sequence_capacity_for_sample_span(
                port.max_events_per_index, window);
            if (!event_capacity) {
                throw std::length_error(
                    "Tick event capture capacity is too large");
            }
            if (*event_capacity > std::numeric_limits<std::size_t>::max()
                    / sizeof(TimedEvent)) {
                throw std::length_error(
                    "Tick event capture payload is too large");
            }
            auto const payload_size = *event_capacity * sizeof(TimedEvent);
            auto const required = capture_requirement(
                planned,
                queued_record_block_count(
                    payload_size, realtime_produced_block_storage_size));
            producer_requirements.push_back({
                .output = output,
                .port = planned.port,
                .kind = PortKind::event,
                .maximum_blocks_per_callback = required,
            });
            producer_bounds_compatible.push_back(bounds_compatible);
        }
    }

    void bind_producer_endpoint(
        std::size_t producer,
        ProducerReserve& reserve,
        PendingQueue& pending,
        std::atomic<bool>& reservation_failed)
    {
        if (producer >= producer_requirements.size()) {
            throw std::out_of_range("Tick capture producer index is invalid");
        }
        if (producer_bounds_compatible[producer] == 0) {
            throw std::invalid_argument(
                "Tick capture has inconsistent callback invocation bounds");
        }
        if (reserve.block_storage_size()
            != realtime_produced_block_storage_size) {
            throw std::invalid_argument(
                "Tick capture producer has an incompatible block size");
        }
        if (producer < sample_capture_slots.size()) {
            auto& selected = sample_capture_slots[producer];
            selected.reserve = &reserve;
            selected.pending = &pending;
            selected.reservation_failed = &reservation_failed;
            sample_captures[producer] = selected.operation();
            return;
        }
        auto const event = producer - sample_capture_slots.size();
        auto& selected = event_capture_slots[event];
        selected.reserve = &reserve;
        selected.pending = &pending;
        selected.reservation_failed = &reservation_failed;
        event_captures[event] = selected.operation();
    }

    void finish_capture() noexcept
    {
        for (auto& selected : sample_capture_slots) {
            selected.publish_callback_chain();
        }
        for (auto& selected : event_capture_slots) {
            selected.publish_callback_chain();
        }
    }

    graph_jit::TickInvocationCall bind(
        PersistedDataStore::Snapshot const& published,
        TickMaterializationSnapshot const& materialized,
        GlobalIndex sample_index,
        std::size_t block_size) noexcept
    {
        auto const materialization_matches = materialized.generation() == generation
            && materialized.data_version() == published.version();
        auto const selected_block_size = std::min(block_size, maximum_block_size);
        for (auto& selected : sequential_sample_slots) {
            auto const* materialized_sample = materialization_matches
                    && selected.accepts_materialization
                ? materialized.find_sample(selected.port)
                : nullptr;
            auto const materialized_sample_is_complete = materialized_sample
                && materialized_sample->layout == selected.layout
                && [&] {
                    for (std::size_t channel = 0;
                         channel < channel_count(selected.layout); ++channel) {
                        if (!materialized_sample->has_channel(channel)) {
                            return false;
                        }
                    }
                    return true;
                }();
            selected.populate(
                published,
                materialized_sample_is_complete ? materialized_sample : nullptr,
                sample_index,
                selected_block_size);
        }
        for (auto& selected : sequential_event_slots) {
            auto const* materialized_event = materialization_matches
                    && selected.accepts_materialization
                ? materialized.find_event(selected.port)
                : nullptr;
            selected.populate(
                published,
                materialized_event && materialized_event->type == selected.type
                    ? materialized_event
                    : nullptr,
                sample_index,
                selected_block_size);
        }
        for (std::size_t slot = 0; slot < sample_slots.size(); ++slot) {
            auto& selected = sample_slots[slot];
            selected.snapshot = &published;
            selected.materialized = materialization_matches
                    && selected.accepts_materialization
                ? materialized.find_sample(selected.port)
                : nullptr;
            auto const materialized_sample_is_complete = selected.materialized
                && selected.materialized->layout == selected.layout
                && [&] {
                    for (std::size_t channel = 0;
                         channel < channel_count(selected.layout); ++channel) {
                        if (!selected.materialized->has_channel(channel)) {
                            return false;
                        }
                    }
                    return true;
                }();
            selected.coverage = materialized_sample_is_complete
                ? &selected.materialized->coverage
                : selected.output
                    ? published.find_sample_coverage(
                        *selected.output, selected.layout)
                    : nullptr;
            if (!materialized_sample_is_complete) {
                selected.materialized = nullptr;
            }
            if (!selected.coverage) selected.coverage = &empty_tick_coverage;
            sample_views[slot].coverage_value = selected.coverage;
        }
        for (std::size_t slot = 0; slot < event_slots.size(); ++slot) {
            auto& selected = event_slots[slot];
            selected.snapshot = &published;
            selected.materialized = materialization_matches
                    && selected.accepts_materialization
                ? materialized.find_event(selected.port)
                : nullptr;
            selected.coverage = selected.materialized
                && selected.materialized->type == selected.type
                ? &selected.materialized->coverage
                : selected.output
                    ? published.find_event_coverage(
                        *selected.output, selected.type)
                    : nullptr;
            if (selected.materialized
                && selected.materialized->type != selected.type) {
                selected.materialized = nullptr;
            }
            if (!selected.coverage) selected.coverage = &empty_tick_coverage;
            event_views[slot].coverage_value = selected.coverage;
        }
        return {
            .sequential_sample_inputs = sequential_sample_views,
            .sequential_event_inputs = sequential_event_views,
            .random_access_sample_inputs = sample_views,
            .random_access_event_inputs = event_views,
            .sample_captures = sample_captures,
            .event_captures = event_captures,
        };
    }
};

TickInvocationWorkspace::TickInvocationWorkspace(
    graph_jit::BackgroundEvaluationPlan const& plan,
    std::uint64_t generation,
    PersistedDataStore& data,
    std::size_t maximum_block_size)
    : impl_(std::make_unique<Impl>(
        plan, generation, data, maximum_block_size))
{}

TickInvocationWorkspace::~TickInvocationWorkspace() = default;

TickInvocationWorkspace::CaptureScope::CaptureScope(Impl& impl) noexcept
    : impl_(&impl)
{}

TickInvocationWorkspace::CaptureScope::~CaptureScope()
{
    if (impl_) impl_->finish_capture();
}

std::size_t TickInvocationWorkspace::sequential_sample_count() const noexcept
{
    return impl_->sequential_sample_views.size();
}

std::size_t TickInvocationWorkspace::sequential_event_count() const noexcept
{
    return impl_->sequential_event_views.size();
}

std::size_t TickInvocationWorkspace::random_access_sample_count() const noexcept
{
    return impl_->sample_views.size();
}

std::size_t TickInvocationWorkspace::random_access_event_count() const noexcept
{
    return impl_->event_views.size();
}

std::size_t TickInvocationWorkspace::sample_capture_count() const noexcept
{
    return impl_->sample_captures.size();
}

std::size_t TickInvocationWorkspace::event_capture_count() const noexcept
{
    return impl_->event_captures.size();
}

std::span<RealtimeProducerRequirement const>
TickInvocationWorkspace::producer_requirements() const noexcept
{
    return impl_->producer_requirements;
}

void TickInvocationWorkspace::bind_producer_endpoint(
    std::size_t producer,
    ProducerReserve& reserve,
    PendingQueue& pending,
    std::atomic<bool>& reservation_failed)
{
    impl_->bind_producer_endpoint(
        producer, reserve, pending, reservation_failed);
}

TickInvocationWorkspace::CaptureScope
TickInvocationWorkspace::begin_capture() noexcept
{
    return CaptureScope{*impl_};
}

graph_jit::TickInvocationCall TickInvocationWorkspace::bind(
    PersistedDataStore::Snapshot const& published,
    TickMaterializationSnapshot const& materialized,
    GlobalIndex sample_index,
    std::size_t block_size) noexcept
{
    return impl_->bind(published, materialized, sample_index, block_size);
}

TickInvocationFrame::TickInvocationFrame(
    PersistedDataStore::ReaderSlot& data_reader,
    TickMaterializationStore::ReaderSlot& materialization_reader,
    TickInvocationWorkspace& workspace,
    GlobalIndex sample_index,
    std::size_t block_size) noexcept
    : published_data_(data_reader.pin())
    , materialized_storage_(materialization_reader.pin())
    , published_data_view_(&published_data_.snapshot())
    , materialized_storage_view_(&materialized_storage_.snapshot())
    , capture_scope_(workspace.begin_capture())
    , call_(workspace.bind(
        *published_data_view_,
        *materialized_storage_view_,
        sample_index,
        block_size))
{}

TickInvocationFrame::TickInvocationFrame(
    RealtimePersistedState const& persisted,
    TickInvocationWorkspace& workspace,
    GlobalIndex sample_index,
    std::size_t block_size) noexcept
    : published_data_view_(&persisted.data())
    , materialized_storage_view_(&persisted.materialization())
    , capture_scope_(workspace.begin_capture())
    , call_(workspace.bind(
        *published_data_view_,
        *materialized_storage_view_,
        sample_index,
        block_size))
{}

} // namespace iv
