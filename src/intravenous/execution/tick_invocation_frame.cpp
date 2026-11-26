#include <intravenous/execution/tick_invocation_frame.h>
#include <intravenous/execution/deferred_tick_input_requests.h>
#include <intravenous/execution/storage/realtime_produced_record.h>

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

bool combine_default_samples(
    graph_jit::BackgroundPortIndex,
    std::byte* destination,
    std::byte const* source,
    std::size_t byte_count)
{
    if (byte_count % sizeof(Sample) != 0) return false;
    auto* destination_values = reinterpret_cast<Sample*>(destination);
    auto const* source_values = reinterpret_cast<Sample const*>(source);
    auto const count = byte_count / sizeof(Sample);
    for (std::size_t index = 0; index < count; ++index) {
        destination_values[index] += source_values[index];
    }
    return true;
}

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
    case EventTypeId::sample:
        return std::holds_alternative<SampleEvent>(event);
    case EventTypeId::curve_point:
        return std::holds_alternative<CurvePointEvent>(event);
    case EventTypeId::beat:
        return std::holds_alternative<BeatEvent>(event);
    case EventTypeId::midi:
        return std::holds_alternative<MidiEvent>(event);
    case EventTypeId::count:
        break;
    }
    return false;
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

std::optional<PersistedOutputHandle> direct_persisted_output(
    graph_jit::BackgroundEvaluationPlan const& plan,
    std::optional<graph_jit::PortStorageIndex> storage,
    std::uint64_t generation,
    PersistedDataStore& data)
{
    if (!storage) return std::nullopt;
    return data.resolve_output(persisted_output_id(plan, *storage, generation));
}

} // namespace

class TickInvocationWorkspace::Impl {
public:
    struct RecordingSpans {
        struct Span {
            std::uint64_t sequential_begin = 0;
            GlobalIndexRange addresses{};
            OutputDisposition disposition = OutputDisposition::untouched;
        };

        std::vector<Span> slots{};
        std::size_t head = 0;
        std::size_t count = 0;
        std::size_t extent = 0;
        std::uint64_t next_sequential_index = 0;

        void allocate(std::size_t latency)
        {
            if (latency == std::numeric_limits<std::size_t>::max()) {
                throw std::length_error("Tick recording latency is too large");
            }
            slots.resize(latency + 1);
        }

        void transfer_from(RecordingSpans const& previous) noexcept
        {
            assert(slots.size() == previous.slots.size());
            head = 0;
            count = previous.count;
            extent = previous.extent;
            next_sequential_index = previous.next_sequential_index;
            for (std::size_t index = 0; index < count; ++index) {
                slots[index] = previous.slots[(previous.head + index) % previous.slots.size()];
            }
        }

        void clear() noexcept
        {
            head = count = extent = next_sequential_index = 0;
        }

        template<typename Finalize>
        void capture(Span span, std::size_t latency, Finalize&& finalize) noexcept
        {
            if (span.addresses.extent == 0) return;
            auto const current_extent = static_cast<std::size_t>(span.addresses.extent);
            span.sequential_begin = next_sequential_index;
            next_sequential_index += current_extent;
            auto const merged = [&] {
                if (count == 0) return false;
                auto& tail = slots[(head + count - 1) % slots.size()];
                if (tail.disposition != span.disposition
                    || tail.sequential_begin + tail.addresses.extent != span.sequential_begin
                    || tail.addresses.begin.advanced_by(tail.addresses.extent) != span.addresses.begin) {
                    return false;
                }
                tail.addresses.extent += current_extent;
                return true;
            }();
            if (!merged) {
                assert(count < slots.size());
                slots[(head + count++) % slots.size()] = span;
            }
            extent += current_extent;
            while (extent > latency) {
                auto& first = slots[head];
                auto const size = std::min<std::size_t>(first.addresses.extent, extent - latency);
                auto finalized = first;
                finalized.addresses.extent = size;
                finalize(finalized);
                first.addresses.begin = first.addresses.begin.advanced_by(size);
                first.addresses.extent -= size;
                first.sequential_begin += size;
                extent -= size;
                if (first.addresses.extent == 0) {
                    head = (head + 1) % slots.size();
                    --count;
                }
            }
        }
    };

    struct RecordingProducer {
        ProducerReserve* reserve = nullptr;
        PendingQueue* pending = nullptr;
        std::atomic<bool>* reservation_failed = nullptr;
        ProducedBlockChain callback_chain{};
        ProducedBlockWriter writer{};
        bool callback_failed = false;

        void fail_queue_reservation() noexcept
        {
            callback_failed = true;
            if (reservation_failed) reservation_failed->store(true, std::memory_order_release);
        }

        void publish_callback_chain() noexcept
        {
            if (callback_chain && !callback_failed
                && !pending->publish(std::move(callback_chain))) fail_queue_reservation();
            clear_callback_chain();
        }

        void clear_callback_chain() noexcept
        {
            callback_chain = {};
            writer = {};
            callback_failed = false;
        }

        bool write_header(RealtimeProducedRecordHeader const& header) noexcept
        {
            return !callback_failed
                && header.payload_size <= std::numeric_limits<std::size_t>::max() - sizeof(header)
                && writer.ensure_capacity(callback_chain, *reserve, sizeof(header) + header.payload_size)
                && writer.append(std::as_bytes(std::span{&header, std::size_t{1}}));
        }
    };

    struct SampleCaptureSlot : RecordingProducer {
        PersistedOutputHandle output{};
        PersistedDataStore::Snapshot const* published = nullptr;
        std::optional<GlobalIndex> held_position{};
        ChannelLayout layout{};
        ContinuousValueStorageGeometry geometry{};
        std::int32_t absolute_pace_exponent = 0;
        std::size_t history = 0;
        std::size_t latency = 0;
        RecordingSpans spans{};

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

        void capture(
            ReflectedContinuousOutputPortBinding const& binding,
            GlobalIndex sample_index,
            std::size_t block_size) noexcept
        {
            if (!reserve || !pending
                || binding.disposition == OutputDisposition::append_events_in_gaps
                || binding.storage.channel_layout != layout
                || binding.storage.geometry != geometry
                || binding.history != history || binding.latency != latency
                || binding.absolute_pace_exponent != absolute_pace_exponent
                || geometry.value_size_bytes == 0
                || binding.storage.value_capacity == 0
                || !std::has_single_bit(binding.storage.value_capacity)) {
                return;
            }
            auto const channels = channel_count(layout);
            if (channels == 0 || channels > maximum_supported_channel_count
                || channels > std::numeric_limits<std::size_t>::max()
                        / geometry.value_size_bytes) {
                return;
            }
            auto const bytes_per_value_index =
                channels * geometry.value_size_bytes;
            for (std::size_t channel = 0; channel < channels; ++channel) {
                auto const& source = binding.storage.channels[channel];
                if (!source.storage || source.value_capacity == 0
                    || !std::has_single_bit(source.value_capacity)
                    || source.value_stride_bytes < geometry.value_size_bytes
                    || source.value_delay >= source.value_capacity) {
                    return;
                }
            }

            auto const window = realtime_port_window(
                sample_index, block_size, 0, 0);
            auto const projection = FractionalGlobalIndexRange::from_global_range(
                window).try_project_onto_lattice(absolute_pace_exponent);
            if (!projection || projection->count == 0) return;
            auto const addresses = GlobalIndexRange{
                GlobalIndex::from_bits(projection->begin_index_bits), projection->count};
            spans.capture({
                .addresses = addresses,
                .disposition = binding.disposition,
            }, latency, [&](RecordingSpans::Span const& finalized) {
                if (finalized.disposition != OutputDisposition::untouched) {
                    auto const physical_begin = binding.sequential_begin_index
                        + finalized.sequential_begin - (spans.next_sequential_index - projection->count);
                    publish(finalized, binding, physical_begin, channels, bytes_per_value_index);
                }
            });
            if (geometry.value_type == continuous_value_type_id_v<Sample>
                && (binding.disposition == OutputDisposition::untouched
                    || binding.disposition == OutputDisposition::voided)) {
                deliver_samples(binding, addresses, channels);
            }
        }

        static std::byte* value_storage(
            ContinuousChannelStorageBinding const& channel,
            std::uint64_t value_index) noexcept
        {
            auto const slot = (value_index - channel.value_delay) & (channel.value_capacity - 1);
            return channel.storage + slot * channel.value_stride_bytes;
        }

        void deliver_samples(
            ReflectedContinuousOutputPortBinding const& binding,
            GlobalIndexRange addresses, std::size_t channels) const noexcept
        {
            auto copy = [&](std::size_t first, std::size_t end,
                            PersistedSampleRegion const* region = nullptr,
                            GlobalIndex source = {}, bool repeat = false) noexcept {
                for (auto frame = first; frame < end; ++frame) {
                    for (std::size_t channel = 0; channel < channels; ++channel) {
                        auto const value = region ? region->at(source, channel) : Sample{};
                        std::memcpy(value_storage(binding.storage.channels[channel],
                            binding.sequential_begin_index + frame), &value, sizeof(value));
                    }
                    if (!repeat) source = source.advanced_by(1);
                }
            };
            if (!published || binding.disposition == OutputDisposition::voided) {
                copy(0, addresses.extent);
                return;
            }

            if (held_position) {
                auto const held = FractionalGlobalIndexRange::from_global_range(
                    {*held_position, 1}).try_project_onto_lattice(absolute_pace_exponent);
                PersistedSampleRegion const* region = nullptr;
                GlobalIndex source{};
                if (held && held->count != 0) {
                    source = GlobalIndex::from_bits(held->begin_index_bits);
                    published->for_each_sample_region(output, {source, 1},
                        [&](PersistedSampleRegion const& stored, GlobalIndexRange) {
                            if (stored.layout == layout && stored.geometry == geometry) region = &stored;
                        });
                }
                copy(0, addresses.extent, region, source, true);
                return;
            }

            copy(0, addresses.extent);
            published->for_each_sample_region(output, addresses,
                [&](PersistedSampleRegion const& stored, GlobalIndexRange part) {
                    if (stored.layout != layout || stored.geometry != geometry) return;
                    auto const first = static_cast<std::size_t>(addresses.begin.forward_distance_to(part.begin));
                    copy(first, first + part.extent, &stored, part.begin);
                });
        }

        void publish(
            RecordingSpans::Span const& finalized,
            ReflectedContinuousOutputPortBinding const& binding,
            std::uint64_t physical_begin,
            std::size_t channels, std::size_t bytes_per_value_index) noexcept
        {
            auto const recorded_range = finalized.addresses;
            auto const selected_count = static_cast<std::size_t>(recorded_range.extent);
            auto const voided = finalized.disposition == OutputDisposition::voided;
            if (!write_header({
                    .payload_kind = voided ? RealtimeProducedPayloadKind::void_value
                        : RealtimeProducedPayloadKind::continuous_values,
                    .payload_size = voided ? 0 : selected_count * bytes_per_value_index,
                    .begin = recorded_range.begin,
                    .range_extent = recorded_range.extent,
                    .continuous_layout = layout,
                    .continuous_geometry = geometry,
                })) {
                fail_queue_reservation();
                return;
            }
            if (voided) return;
            for (std::size_t frame = 0; frame < selected_count; ++frame) {
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    auto const& source = binding.storage.channels[channel];
                    if (!writer.append(std::span<std::byte const>{
                        value_storage(source, physical_begin + frame),
                        geometry.value_size_bytes})) {
                        fail_queue_reservation();
                        return;
                    }
                }
            }
        }
    };

    struct EventCaptureSlot : RecordingProducer {
        EventTypeId type = EventTypeId::empty;
        std::size_t history = 0;
        std::size_t latency = 0;
        std::size_t acceptance_capacity = 0;
        std::size_t maximum_invocations = 0;
        std::size_t header_bytes = 0;

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

        struct PendingEvent {
            std::uint64_t sequential_index = 0;
            std::size_t write_order = 0;
            Event value{};
        };
        RecordingSpans spans{};
        std::vector<PendingEvent> pending_events{};
        std::size_t pending_count = 0;

        std::size_t provision(std::size_t capacity)
        {
            auto const limit = std::numeric_limits<std::size_t>::max();
            if (latency > limit - maximum_invocations
                || (capacity != 0 && latency + maximum_invocations > limit / capacity)) {
                throw std::length_error("Tick Event capture invocation bound is too large");
            }
            auto const payload_events = (latency + maximum_invocations) * capacity;
            if (payload_events > (limit - header_bytes) / sizeof(TimedEvent)) {
                throw std::length_error("Tick Event capture payload is too large");
            }
            acceptance_capacity = capacity;
            pending_events.resize((latency + 1) * capacity);
            return capture_block_count(1, header_bytes + payload_events * sizeof(TimedEvent),
                realtime_produced_block_storage_size);
        }

        bool publish(RecordingSpans::Span const& span,
                     std::span<PendingEvent const> events) noexcept
        {
            auto const voided = span.disposition == OutputDisposition::voided;
            auto const append_in_gaps = span.disposition == OutputDisposition::append_events_in_gaps;
            if (!write_header({
                .payload_kind = voided ? RealtimeProducedPayloadKind::void_value
                    : append_in_gaps ? RealtimeProducedPayloadKind::append_events_in_gaps
                                     : RealtimeProducedPayloadKind::events,
                .payload_size = voided ? 0 : events.size() * sizeof(TimedEvent),
                .begin = span.addresses.begin,
                .range_extent = span.addresses.extent,
                .event_type = type,
                .event_count = voided ? 0 : events.size(),
            })) return false;
            if (!voided) {
                for (auto const& selected : events) {
                    auto const event = TimedEvent{
                        .time = span.addresses.begin.advanced_by(selected.sequential_index - span.sequential_begin),
                        .value = selected.value,
                    };
                    if (!writer.append(std::as_bytes(std::span{&event, std::size_t{1}}))) return false;
                }
            }
            return true;
        }

        void capture(
            ReflectedEventOutputPortBinding const& binding,
            GlobalIndex sample_index,
            std::size_t block_size) noexcept
        {
            auto const& storage = binding.storage;
            if (!reserve || !pending
                || storage.type != type || binding.history != history || binding.latency != latency
                || (storage.event_capacity != 0 && (storage.storage == nullptr
                    || !std::has_single_bit(storage.event_capacity)))) return;

            auto const sequential_begin = spans.next_sequential_index;
            auto const open_begin = sequential_begin - spans.extent;
            auto const open_window = GlobalIndexRange{
                sample_index.retreated_by(spans.extent), spans.extent + block_size};
            if ((binding.disposition == OutputDisposition::written
                    || binding.disposition == OutputDisposition::append_events_in_gaps)
                && storage.event_capacity != 0) {
                auto const* events = reinterpret_cast<TimedEvent const*>(storage.storage + storage.events_offset);
                auto const write = *reinterpret_cast<std::size_t const*>(storage.storage
                    + (storage.persistent_ring ? storage.write_index_offset : storage.count_offset));
                auto const authored = write - binding.authored_begin_index;
                IV_ASSERT(authored <= storage.event_capacity && authored <= acceptance_capacity,
                    "Tick Event output exceeds its planned acceptance bound");
                auto const previous_count = pending_count;
                for (std::size_t offset = 0; offset < authored; ++offset) {
                    auto const& event = events[(binding.authored_begin_index + offset) & (storage.event_capacity - 1)];
                    auto const position = open_window.offset_of(event.time);
                    if (!position) continue;
                    IV_ASSERT(capture_event_matches_type(type, event.value),
                        "Tick Event output wrote an incompatible Event");
                    IV_ASSERT(pending_count < pending_events.size(),
                        "Tick Event output exceeded its planned pending capacity");
                    pending_events[pending_count] = {open_begin + *position, pending_count, event.value};
                    ++pending_count;
                }
                if (pending_count != previous_count) {
                    std::sort(pending_events.begin(), pending_events.begin() + pending_count,
                        [open_begin](PendingEvent const& left, PendingEvent const& right) {
                            if (left.sequential_index == right.sequential_index) return left.write_order < right.write_order;
                            return left.sequential_index - open_begin < right.sequential_index - open_begin;
                        });
                }
            }

            std::size_t consumed = 0;
            spans.capture({
                .addresses = {sample_index, block_size},
                .disposition = binding.disposition,
            }, latency, [&](RecordingSpans::Span const& finalized) {
                auto const first = consumed;
                while (consumed < pending_count && pending_events[consumed].sequential_index
                    - finalized.sequential_begin < finalized.addresses.extent) ++consumed;
                if (finalized.disposition != OutputDisposition::untouched
                    && !publish(finalized, std::span{pending_events}.subspan(first, consumed - first))) {
                    fail_queue_reservation();
                }
            });
            if (consumed != 0) {
                std::move(pending_events.begin() + consumed, pending_events.begin() + pending_count, pending_events.begin());
                pending_count -= consumed;
                for (std::size_t index = 0; index < pending_count; ++index) {
                    pending_events[index].write_order = index;
                }
            }
        }
    };

    struct SequentialSampleSlot {
        graph_jit::BackgroundPortIndex port = 0;
        std::optional<PersistedOutputHandle> output{};
        bool accepts_materialization = false;
        ChannelLayout layout{};
        ContinuousValueStorageGeometry geometry{};
        std::size_t capacity = 0;
        Sample neutral{};
        graph_jit::SequentialInputCombineFunction combine =
            &combine_default_samples;
        graph_jit::SequentialInputInitializeFunction initialize = nullptr;
        // Frame f holds the current window's position sample_index + f. The
        // consumer's history is node-owned realtime state, so the window never
        // reaches behind the current block and its sequential begin index stays
        // zero.
        PersistedContinuousStorage values{};
        ReflectedContinuousInputPortBinding binding{};

        [[nodiscard]] std::byte* value_storage(
            std::size_t frame, std::size_t channel) noexcept
        {
            auto const channels = channel_count(layout);
            auto const value = layout.sample_layout
                    == SampleStreamLayout::planar
                ? channel * capacity + frame
                : frame * channels + channel;
            return values.data() + value * geometry.value_size_bytes;
        }

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
            auto const value_count = capacity * channels;
            if (geometry.value_size_bytes == 0
                || value_count > std::numeric_limits<std::size_t>::max()
                    / geometry.value_size_bytes) {
                throw std::length_error(
                    "Tick Sequential continuous playback storage is too large");
            }
            values = PersistedContinuousStorage{
                value_count * geometry.value_size_bytes,
                geometry.element_alignment};
            binding = {
                .storage = {
                    .geometry = geometry,
                    .value_capacity = capacity,
                    .channel_layout = layout,
                },
                .history = 0,
                .read_latency = 0,
            };
            for (std::size_t channel = 0; channel < channels; ++channel) {
                auto const planar = layout.sample_layout
                    == SampleStreamLayout::planar;
                binding.storage.channels[channel] = {
                    .storage = value_storage(0, channel),
                    .value_capacity = capacity,
                    .value_stride_bytes =
                        (planar ? 1 : channels) * geometry.value_size_bytes,
                    .value_delay = 0,
                };
            }
            clear(capacity);
        }

        void clear(std::size_t frames) noexcept
        {
            auto const channels = channel_count(layout);
            auto initialize_range = [&](std::byte* destination,
                                        std::size_t byte_count) noexcept {
                auto accepted = initialize
                    && initialize(port, destination, byte_count);
                if (!accepted
                    && geometry.value_type
                        == continuous_value_type_id_v<Sample>
                    && geometry.value_size_bytes == sizeof(Sample)) {
                    auto output = std::span{
                        reinterpret_cast<Sample*>(destination),
                        byte_count / sizeof(Sample)};
                    std::ranges::fill(output, neutral);
                    accepted = true;
                }
                IV_ASSERT(accepted,
                    "Tick Sequential continuous initialization was rejected");
            };
            if (layout.sample_layout == SampleStreamLayout::planar) {
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    initialize_range(
                        value_storage(0, channel),
                        frames * geometry.value_size_bytes);
                }
            } else {
                initialize_range(
                    values.data(),
                    frames * channels * geometry.value_size_bytes);
            }
        }

        void copy(
            PersistedSampleRegion const& region,
            GlobalIndex window_begin,
            GlobalIndexRange intersection) noexcept
        {
            if (region.layout != layout
                || region.geometry != geometry
                || region.storage_data() == nullptr) {
                return;
            }
            auto const frames = static_cast<std::size_t>(intersection.extent);
            auto const source = static_cast<std::size_t>(
                region.range.begin.forward_distance_to(intersection.begin));
            auto const destination = static_cast<std::size_t>(
                window_begin.forward_distance_to(intersection.begin));
            auto const region_frames = static_cast<std::size_t>(
                region.range.extent);
            auto const channels = channel_count(layout);
            auto combine_range = [&](std::byte* destination_values,
                                     std::byte const* source_values,
                                     std::size_t byte_count) noexcept {
                auto const accepted = combine(
                    port,
                    destination_values,
                    source_values,
                    byte_count);
                IV_ASSERT(accepted,
                    "Tick Sequential continuous combination was rejected");
            };
            auto const* source_storage = region.storage_data();
            if (layout.sample_layout == SampleStreamLayout::planar) {
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    combine_range(
                        value_storage(destination, channel),
                        source_storage
                            + (channel * region_frames + source)
                                * geometry.value_size_bytes,
                        frames * geometry.value_size_bytes);
                }
            } else {
                combine_range(
                    value_storage(destination, 0),
                    source_storage
                        + source * channels * geometry.value_size_bytes,
                    frames * channels * geometry.value_size_bytes);
            }
        }

        void populate(
            PersistedDataStore::Snapshot const& published,
            TickMaterializedSampleInput const* materialized,
            GlobalIndex sample_index,
            std::size_t block_size, std::optional<GlobalIndex> held) noexcept
        {
            if (block_size == 0) return;
            auto const selected_index = held.value_or(sample_index);
            auto const requested = GlobalIndexRange{
                .begin = selected_index,
                .extent = held ? 1 : block_size,
            };
            clear(block_size);
            if (output) {
                published.for_each_sample_region(*output, requested,
                    [&](PersistedSampleRegion const& stored, GlobalIndexRange intersection) {
                        if (!materialized || !materialized->coverage.intersects(intersection)) {
                            copy(stored, selected_index, intersection);
                            return;
                        }
                        if (materialized->coverage.contains(intersection)) return;
                        auto cursor = intersection.begin;
                        auto remaining = intersection.extent;
                        while (remaining != 0) {
                            auto const covered = materialized->coverage.contains(cursor);
                            GlobalIndex::extent_type count = 1;
                            while (count < remaining
                                && materialized->coverage.contains(cursor.advanced_by(count)) == covered) ++count;
                            if (!covered) copy(stored, selected_index, {cursor, count});
                            cursor = cursor.advanced_by(count);
                            remaining -= count;
                        }
                    });
            }

            if (materialized) materialized->for_each_region(
                requested,
                [&](TickMaterializedSampleRegion const& region,
                    GlobalIndexRange intersection) {
                    copy(*materialized, region, selected_index, intersection);
                });
            if (held) {
                for (std::size_t channel = 0; channel < channel_count(layout); ++channel) {
                    for (std::size_t frame = 1; frame < block_size; ++frame) {
                        std::memcpy(value_storage(frame, channel), value_storage(0, channel),
                            geometry.value_size_bytes);
                    }
                }
            }
        }

        // The caller selected only complete materializations, so every
        // logical channel has a slot in region.
        void copy(
            TickMaterializedSampleInput const& materialized,
            TickMaterializedSampleRegion const& region,
            GlobalIndex window_begin,
            GlobalIndexRange intersection) noexcept
        {
            if (materialized.geometry != geometry
                || region.storage_data() == nullptr) {
                return;
            }
            auto const frames = static_cast<std::size_t>(intersection.extent);
            auto const source = static_cast<std::size_t>(
                region.range.begin.forward_distance_to(intersection.begin));
            auto const destination = static_cast<std::size_t>(
                window_begin.forward_distance_to(intersection.begin));
            auto const channels = channel_count(layout);
            auto combine_range = [&](std::byte* destination_values,
                                     std::byte const* source_values,
                                     std::size_t byte_count) noexcept {
                auto const accepted = combine(
                    port,
                    destination_values,
                    source_values,
                    byte_count);
                IV_ASSERT(accepted,
                    "Tick Sequential continuous combination was rejected");
            };
            auto const* source_storage = region.storage_data();
            if (layout.sample_layout == SampleStreamLayout::planar) {
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    auto const slot = materialized.channel_slot(channel);
                    combine_range(
                        value_storage(destination, channel),
                        source_storage
                            + (slot * region.storage_frames
                                + region.first_frame + source)
                                * geometry.value_size_bytes,
                        frames * geometry.value_size_bytes);
                }
            } else {
                combine_range(
                    value_storage(destination, 0),
                    source_storage
                        + (region.first_frame + source) * channels
                            * geometry.value_size_bytes,
                    frames * channels * geometry.value_size_bytes);
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
            events()[size].time = event.time.advanced_by(delivery_offset);
            semantic_source_indices[size] = semantic_source_index;
            ++size;
        }

        std::uint64_t delivery_offset = 0;

        void populate(
            PersistedDataStore::Snapshot const& published,
            TickMaterializedEventInput const* materialized,
            GlobalIndex sample_index,
            std::size_t block_size, TickPlaybackSelection selection) noexcept
        {
            count() = 0;
            if (block_size == 0 || (selection.held_position && !selection.audition)) return;
            auto const selected_index = selection.held_position.value_or(sample_index);
            delivery_offset = sample_index.bits() - selected_index.bits();
            // Only the current window: the consumer owns its event history.
            auto const range = GlobalIndexRange{
                .begin = selected_index,
                .extent = selection.held_position ? 1 : block_size,
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
            auto const timeline = published.event_timeline(*output, type);
            range.for_each_signed_order_segment(
                [&](GlobalIndexRange segment) {
                auto event = std::ranges::lower_bound(
                    timeline,
                    segment.begin,
                    SignedGlobalIndexOrder{},
                    &TimedEvent::time);
                for (; event != timeline.end()
                       && segment.contains(event->time); ++event) {
                    append(*event);
                }
            });
        }
    };

    struct SampleSlot {
        TickMaterializedSampleInput const* materialized = nullptr;
        std::optional<PersistedOutputHandle> output{};
        graph_jit::BackgroundPortIndex port = 0;
        bool accepts_materialization = false;
        ChannelLayout layout{};
        ContinuousValueStorageGeometry geometry{};
        IndexCoverage const* coverage = &empty_tick_coverage;
        std::span<ContinuousRegionStorageView const> regions{};
        DeferredTickInputRequests* requests = nullptr;
        std::size_t request_lead = 0;
        std::optional<graph_jit::BackgroundPortIndex> inherited_port{};
        GlobalIndex execution_begin{};
        GlobalIndex project_begin{};

        static TickInputRequestOutcome submit(
            void* opaque, GlobalIndexRange block, SamplePositionsView positions) noexcept
        {
            auto& slot = *static_cast<SampleSlot*>(opaque);
            if (!slot.requests) return {};
            auto const begin = slot.execution_begin.advanced_by(slot.project_begin.forward_distance_to(block.begin));
            GlobalIndexRange const execution{begin, block.extent};
            auto const submitted = slot.requests->submit({.port = slot.port, .execution = execution,
                .first_due = begin.advanced_by(slot.request_lead), .positions = positions});
            return {submitted, slot.requests->due(slot.port, execution)};
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
        std::span<GlobalIndexRange const> regions{};

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
            auto const timeline = snapshot->event_timeline(*output, type);
            range.for_each_signed_order_segment(
                [&](GlobalIndexRange segment) {
                auto event = std::ranges::lower_bound(
                    timeline,
                    segment.begin,
                    SignedGlobalIndexOrder{},
                    &TimedEvent::time);
                for (; event != timeline.end()
                       && segment.contains(event->time); ++event) {
                    visitor(visitor_data, *event);
                }
            });
        }

        void for_each_region(
            std::size_t region,
            GlobalIndexRange range,
            void* visitor_data,
            RandomAccessEventInputPort::VisitEvent visitor) const
        {
            if (materialized) {
                materialized->for_each_region(
                    region, range, visitor_data, visitor);
                return;
            }
            if (!snapshot || !output || region >= regions.size()
                || !regions[region].contains(range)) {
                return;
            }
            snapshot->for_each_event_region(
                *output,
                type,
                region,
                range,
                [&](TimedEvent const& event) { visitor(visitor_data, event); });
        }
        [[nodiscard]] EventNeighbors neighbors(std::size_t region, GlobalIndex position, EventNeighborMode mode) const {
            if (materialized) return materialized->neighbors(region, position, mode);
            if (!snapshot || !output) return {};
            return snapshot->event_neighbors(*output, type, region, position, mode);
        }

    };

    std::vector<SequentialSampleSlot> sequential_sample_slots{};
    std::vector<ReflectedContinuousInputPortBinding> sequential_sample_views{};
    std::vector<SequentialEventSlot> sequential_event_slots{};
    std::vector<ReflectedEventInputPortBinding> sequential_event_views{};
    std::vector<SampleSlot> sample_slots{};
    std::vector<EventSlot> event_slots{};
    std::vector<RandomAccessContinuousInputPort> sample_views{};
    std::vector<RandomAccessEventInputPort> event_views{};
    std::vector<SampleCaptureSlot> sample_capture_slots{};
    std::vector<EventCaptureSlot> event_capture_slots{};
    std::vector<std::pair<std::size_t, std::size_t>> sample_recording_transfers{};
    std::vector<std::pair<std::size_t, std::size_t>> event_recording_transfers{};
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
        std::size_t selected_maximum_block_size,
        graph_jit::SequentialInputCombineFunction combine_sequential_input,
        graph_jit::SequentialInputInitializeFunction initialize_sequential_input)
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
            selected.geometry = port.continuous_geometry;
            selected.capacity = std::bit_ceil(maximum_block_size);
            selected.neutral = port.sample_neutral_value;
            selected.combine = combine_sequential_input
                ? combine_sequential_input
                : selected.geometry.value_type
                        == continuous_value_type_id_v<Sample>
                    ? &combine_default_samples
                    : nullptr;
            selected.initialize = initialize_sequential_input;
            if (selected.geometry.value_type
                    != continuous_value_type_id_v<Sample>
                && (!selected.combine || !selected.initialize)) {
                throw std::invalid_argument(
                    "Tick Sequential continuous binding has no value-type algebra");
            }
            selected.accepts_materialization = std::ranges::any_of(
                planned.reads, [&](auto const& read) {
                    auto const storage = read.storage;
                    return storage < plan.storage.ports.size()
                        && (plan.storage.ports[storage].storage
                                == graph_jit::PortStorageKind::tick_sequential
                            || plan.storage.ports[storage].storage
                                == graph_jit::PortStorageKind::tick_random_access);
                });
            selected.output = direct_persisted_output(
                plan, planned.direct_persisted_storage, selected_generation, data);
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

            std::size_t maximum_count = 0;
            for (auto const& read : planned.reads) {
                auto const storage = read.storage;
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
                auto const bound = graph_jit::detail::background_event_count_bound(
                    plan.storage, storage, maximum_block_size);
                if (!bound) throw std::length_error("Tick event playback bound is not representable");
                maximum_count = std::max(maximum_count, bound->count);
            }
            auto const maximum_power = std::size_t{1}
                << (std::numeric_limits<std::size_t>::digits - 1);
            if (maximum_count > maximum_power) {
                throw std::length_error(
                    "Tick Sequential event playback capacity is too large");
            }

            auto& selected = sequential_event_slots[slot];
            selected.port = planned.port;
            selected.type = port.event_type;
            selected.capacity = std::bit_ceil(std::max<std::size_t>(maximum_count, 1));
            selected.accepts_materialization = std::ranges::any_of(
                planned.reads, [&](auto const& read) {
                    auto const storage = read.storage;
                    return storage < plan.storage.ports.size()
                        && (plan.storage.ports[storage].storage
                                == graph_jit::PortStorageKind::tick_sequential
                            || plan.storage.ports[storage].storage
                                == graph_jit::PortStorageKind::tick_random_access);
                });
            selected.output = direct_persisted_output(
                plan, planned.direct_persisted_storage, selected_generation, data);
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
            selected.request_lead = port.deferred_tick_lead;
            selected.port = binding.port;
            selected.layout = port.sample_layout;
            selected.geometry = port.continuous_geometry;
            selected.accepts_materialization = std::ranges::any_of(
                binding.reads, [&](auto const& read) {
                    auto const storage = read.storage;
                    return storage < plan.storage.ports.size()
                        && plan.storage.ports[storage].storage
                            != graph_jit::PortStorageKind::current_tick;
                });

            selected.output = direct_persisted_output(
                plan, binding.direct_persisted_storage, selected_generation, data);
            sample_views[slot] = RandomAccessContinuousInputPort{
                .regions = nullptr,
                .region_count = 0,
                .coverage_value = &empty_tick_coverage,
                .geometry = port.continuous_geometry,
                .absolute_pace_exponent = port.absolute_pace_exponent,
                .request_owner = &selected,
                .request_samples = selected.request_lead ? &SampleSlot::submit : nullptr,
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
                binding.reads, [&](auto const& read) {
                    auto const storage = read.storage;
                    return storage < plan.storage.ports.size()
                        && plan.storage.ports[storage].storage
                            != graph_jit::PortStorageKind::current_tick;
                });

            selected.output = direct_persisted_output(
                plan, binding.direct_persisted_storage, selected_generation, data);
            event_views[slot] = RandomAccessEventInputPort{
                .data = &selected,
                .region_ranges = nullptr,
                .region_count = 0,
                .coverage_value = &empty_tick_coverage,
                .for_each_event = +[](
                    void const* opaque,
                    std::size_t region,
                    GlobalIndexRange range,
                    void* visitor_data,
                    RandomAccessEventInputPort::VisitEvent visitor) {
                    static_cast<EventSlot const*>(opaque)->for_each_region(
                        region, range, visitor_data, visitor);
                },
                .find_neighbors = +[](void const* opaque, std::size_t region, GlobalIndex position, EventNeighborMode mode) {
                    return static_cast<EventSlot const*>(opaque)->neighbors(region, position, mode);
                },
            };
        }

        auto capture_requirement = [&](auto const& planned,
                                       std::size_t payload_size,
                                       std::size_t maximum_records) {
            if (planned.maximum_block_size == 0
                || planned.maximum_invocations_per_callback == 0) {
                throw std::invalid_argument(
                    "Tick capture has invalid callback invocation bounds");
            }
            if (maximum_records > (std::numeric_limits<std::size_t>::max() - payload_size)
                    / sizeof(RealtimeProducedRecordHeader)) {
                throw std::length_error("Tick capture fragmentation reserve is too large");
            }
            return capture_block_count(
                planned.maximum_invocations_per_callback,
                payload_size + maximum_records * sizeof(RealtimeProducedRecordHeader),
                realtime_produced_block_storage_size);
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
                || port.realization != OutputRealization::buffered
                || port.sample_layout.sample_layout
                    != SampleStreamLayout::interleaved) {
                throw std::invalid_argument(
                    "Tick continuous capture references an invalid persisted output");
            }
            auto const bounds_compatible =
                capture_bounds_match_workspace(planned);
            auto const output = data.resolve_output(capture_output_id(
                plan, planned.port, selected_generation));
            auto& selected = sample_capture_slots[slot];
            selected = SampleCaptureSlot{
                .output = output,
                .layout = port.sample_layout,
                .geometry = port.continuous_geometry,
                .absolute_pace_exponent = port.absolute_pace_exponent,
                .history = port.output_history,
                .latency = port.output_latency,
            };
            auto const maximum_value_count =
                FractionalGlobalIndexRange::maximum_projection_count(
                    planned.maximum_block_size,
                    port.absolute_pace_exponent);
            if (!maximum_value_count || *maximum_value_count == 0) {
                throw std::length_error(
                    "Tick continuous capture projection is too large");
            }
            auto const channels = channel_count(port.sample_layout);
            if (channels == 0 || port.continuous_geometry.value_size_bytes == 0
                || channels > std::numeric_limits<std::size_t>::max()
                        / port.continuous_geometry.value_size_bytes) {
                throw std::length_error(
                    "Tick continuous capture layout is too large");
            }
            auto const bytes_per_value_index =
                channels * port.continuous_geometry.value_size_bytes;
            if (*maximum_value_count > std::numeric_limits<std::size_t>::max()
                    / bytes_per_value_index) {
                throw std::length_error(
                    "Tick continuous capture payload is too large");
            }
            auto const payload_size =
                *maximum_value_count * bytes_per_value_index;
            selected.spans.allocate(port.output_latency);
            auto const required = capture_requirement(
                planned,
                payload_size, 1 + std::min(port.output_latency, *maximum_value_count - 1));
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
                || port.realization != OutputRealization::buffered) {
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
                .maximum_invocations = planned.maximum_invocations_per_callback,
            };
            auto const window = planned.maximum_block_size;
            auto const fragments = window == 0 ? 0 : 1 + std::min(port.output_latency, window - 1);
            if (!bounds_compatible || fragments > std::numeric_limits<std::size_t>::max()
                    / sizeof(RealtimeProducedRecordHeader) / selected.maximum_invocations) {
                throw std::length_error("Tick Event capture fragmentation bound is invalid");
            }
            selected.header_bytes = fragments * sizeof(RealtimeProducedRecordHeader) * selected.maximum_invocations;
            selected.spans.allocate(port.output_latency);
            auto const required = selected.provision(planned.event_acceptance_capacity);
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
        std::size_t block_size, TickPlaybackSelection selection,
        TickMaterializationSnapshot const* inherited_deferred) noexcept
    {
        auto const materialization_matches = materialized.generation() == generation
            && materialized.data_version() == published.version();
        auto const selected_block_size = std::min(block_size, maximum_block_size);
        for (auto& selected : sample_capture_slots) {
            selected.published = &published;
            selected.held_position = selection.held_position;
        }
        for (auto& selected : sequential_sample_slots) {
            auto const* materialized_sample = materialization_matches
                    && selected.accepts_materialization
                ? materialized.find_sample(selected.port)
                : nullptr;
            auto const materialized_sample_is_complete = materialized_sample
                && materialized_sample->layout == selected.layout
                && materialized_sample->geometry == selected.geometry
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
                selected_block_size, selection.held_position);
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
                selected_block_size, selection);
        }
        for (std::size_t slot = 0; slot < sample_slots.size(); ++slot) {
            auto& selected = sample_slots[slot];
            selected.project_begin = sample_index;
            selected.materialized = materialization_matches
                    && selected.accepts_materialization
                ? materialized.find_sample(selected.port)
                : nullptr;
            if (inherited_deferred && selected.inherited_port)
                selected.materialized = inherited_deferred->find_sample(*selected.inherited_port);
            auto const materialized_sample_is_complete = selected.materialized
                && selected.materialized->layout == selected.layout
                && selected.materialized->geometry == selected.geometry
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
                        *selected.output,
                        selected.layout,
                        selected.geometry)
                    : nullptr;
            if (!materialized_sample_is_complete) {
                selected.materialized = nullptr;
            }
            if (!selected.coverage) selected.coverage = &empty_tick_coverage;
            selected.regions = selected.materialized
                ? std::span<ContinuousRegionStorageView const>{
                    selected.materialized->region_views}
                : selected.output
                    ? published.sample_region_views(
                        *selected.output,
                        selected.layout,
                        selected.geometry)
                    : std::span<ContinuousRegionStorageView const>{};
            sample_views[slot].regions = selected.regions.data();
            sample_views[slot].region_count = selected.regions.size();
            sample_views[slot].coverage_value = selected.materialized
                ? &selected.materialized->logical_coverage
                : selected.output ? published.find_sample_logical_coverage(*selected.output)
                    : selected.coverage;
            if (!sample_views[slot].coverage_value) sample_views[slot].coverage_value = selected.coverage;
            sample_views[slot].prepared_coverage_value = selected.coverage;
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
            selected.regions = selected.materialized
                ? std::span<GlobalIndexRange const>{
                    selected.materialized->region_ranges}
                : selected.output
                    ? published.event_region_ranges(
                        *selected.output, selected.type)
                    : std::span<GlobalIndexRange const>{};
            event_views[slot].region_ranges = selected.regions.data();
            event_views[slot].region_count = selected.regions.size();
            event_views[slot].coverage_value = selected.materialized
                ? &selected.materialized->logical_coverage
                : selected.output ? published.find_event_logical_coverage(*selected.output)
                    : selected.coverage;
            if (!event_views[slot].coverage_value) event_views[slot].coverage_value = selected.coverage;
            event_views[slot].prepared_coverage_value = selected.coverage;
        }
        return {
            .sequential_sample_inputs = sequential_sample_views,
            .sequential_event_inputs = sequential_event_views,
            .random_access_sample_inputs = sample_views,
            .random_access_event_inputs = event_views,
            .sample_captures = sample_captures,
            .event_captures = event_captures,
            .output_coverages = materialization_matches
                ? materialized.output_coverages() : std::span<IndexCoverage const>{},
            .held_background_position = selection.held_position,
        };
    }
};

TickInvocationWorkspace::TickInvocationWorkspace(
    graph_jit::BackgroundEvaluationPlan const& plan,
    std::uint64_t generation,
    PersistedDataStore& data,
    std::size_t maximum_block_size,
    graph_jit::SequentialInputCombineFunction combine_sequential_input,
    graph_jit::SequentialInputInitializeFunction initialize_sequential_input)
    : impl_(std::make_unique<Impl>(
        plan,
        generation,
        data,
        maximum_block_size,
        combine_sequential_input,
        initialize_sequential_input))
{}

TickInvocationWorkspace::~TickInvocationWorkspace() = default;

TickInvocationWorkspace::CaptureScope::CaptureScope(Impl& impl, bool publish) noexcept
    : impl_(publish ? &impl : nullptr)
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

void TickInvocationWorkspace::prepare_recording_transfer_from(
    TickInvocationWorkspace const& previous,
    graph_jit::BackgroundEvaluationPlan const& plan,
    std::span<NodeBundlePortId const> inherited_outputs)
{
    auto& target = *impl_;
    auto const& source = *previous.impl_;
    for (std::size_t index = 0; index < target.producer_requirements.size(); ++index) {
        auto& requirement = target.producer_requirements[index];
        if (!std::ranges::contains(inherited_outputs, plan.ports[requirement.port].configured_port)) continue;
        auto const found = std::ranges::find(source.producer_requirements,
            requirement.output, &RealtimeProducerRequirement::output);
        if (found == source.producer_requirements.end()) continue;
        auto const old_index = static_cast<std::size_t>(found - source.producer_requirements.begin());
        if (requirement.kind == PortKind::sample) {
            auto const& before = source.sample_capture_slots[old_index];
            auto const& after = target.sample_capture_slots[index];
            if (after.latency != 0 && after.latency == before.latency
                && after.layout == before.layout && after.geometry == before.geometry
                && after.absolute_pace_exponent == before.absolute_pace_exponent) {
                target.sample_recording_transfers.emplace_back(old_index, index);
            }
        } else {
            auto const previous_slot = old_index - source.sample_capture_slots.size();
            auto const target_slot = index - target.sample_capture_slots.size();
            auto const& before = source.event_capture_slots[previous_slot];
            auto& after = target.event_capture_slots[target_slot];
            if (after.latency != 0 && after.latency == before.latency && after.type == before.type) {
                if (after.acceptance_capacity < before.acceptance_capacity) {
                    requirement.maximum_blocks_per_callback = after.provision(before.acceptance_capacity);
                }
                target.event_recording_transfers.emplace_back(previous_slot, target_slot);
            }
        }
    }
}

void TickInvocationWorkspace::transfer_recording_from(TickInvocationWorkspace const& previous) noexcept
{
    auto& target = *impl_;
    auto const& source = *previous.impl_;
    for (auto const [from, to] : target.sample_recording_transfers) {
        target.sample_capture_slots[to].spans.transfer_from(source.sample_capture_slots[from].spans);
    }
    for (auto const [from, to] : target.event_recording_transfers) {
        auto const& before = source.event_capture_slots[from];
        auto& after = target.event_capture_slots[to];
        after.spans.transfer_from(before.spans);
        assert(before.pending_count <= after.pending_events.size());
        std::copy_n(before.pending_events.begin(), before.pending_count, after.pending_events.begin());
        after.pending_count = before.pending_count;
    }
}

void TickInvocationWorkspace::discard_unpublished_startup() noexcept
{
    for (auto& slot : impl_->sample_capture_slots) {
        slot.clear_callback_chain();
        slot.spans.clear();
    }
    for (auto& slot : impl_->event_capture_slots) {
        slot.clear_callback_chain();
        slot.spans.clear();
        slot.pending_count = 0;
    }
}

TickInvocationWorkspace::CaptureScope
TickInvocationWorkspace::begin_capture(bool publish) noexcept
{
    return CaptureScope{*impl_, publish};
}

void TickInvocationWorkspace::bind_deferred_input_requests(DeferredTickInputRequests& requests,
    GlobalIndex execution_begin) noexcept
{
    for (auto& slot : impl_->sample_slots) {
        if (slot.request_lead) {
            slot.requests = &requests;
            slot.execution_begin = execution_begin;
        }
    }
}

void TickInvocationWorkspace::prepare_deferred_input_transfer(
    std::span<std::optional<graph_jit::BackgroundPortIndex> const> successor_ports) noexcept
{
    for (auto& slot : impl_->sample_slots) {
        auto const previous = std::ranges::find(successor_ports, std::optional{slot.port});
        if (previous != successor_ports.end()) slot.inherited_port = previous - successor_ports.begin();
    }
}

graph_jit::TickInvocationCall TickInvocationWorkspace::bind(
    PersistedDataStore::Snapshot const& published,
    TickMaterializationSnapshot const& materialized,
    GlobalIndex sample_index,
    std::size_t block_size, TickPlaybackSelection selection,
    TickMaterializationSnapshot const* inherited_deferred) noexcept
{
    return impl_->bind(published, materialized, sample_index, block_size, selection, inherited_deferred);
}

TickInvocationFrame::TickInvocationFrame(
    PersistedDataStore::ReaderSlot& data_reader,
    TickMaterializationStore::ReaderSlot& materialization_reader,
    TickInvocationWorkspace& workspace,
    GlobalIndex sample_index,
    std::size_t block_size, TickPlaybackSelection selection) noexcept
    : published_data_(data_reader.pin())
    , materialized_storage_(materialization_reader.pin())
    , published_data_view_(&published_data_.snapshot())
    , materialized_storage_view_(&materialized_storage_.snapshot())
    , capture_scope_(workspace.begin_capture())
    , call_(workspace.bind(
        *published_data_view_,
        *materialized_storage_view_,
        sample_index,
        block_size, selection))
{}

TickInvocationFrame::TickInvocationFrame(
    RealtimePersistedState const& persisted,
    TickInvocationWorkspace& workspace,
    GlobalIndex sample_index,
    std::size_t block_size, TickPlaybackSelection selection, bool publish_captures) noexcept
    : published_data_view_(&persisted.data())
    , materialized_storage_view_(&persisted.materialization())
    , capture_scope_(workspace.begin_capture(publish_captures))
    , call_(workspace.bind(
        *published_data_view_,
        *materialized_storage_view_,
        sample_index,
        block_size, selection, persisted.inherited_deferred_materialization()))
{}

} // namespace iv
