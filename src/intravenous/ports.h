#pragma once

#include <intravenous/compat.h>
#include <intravenous/channel_layout.h>
#include <intravenous/continuous_value.h>
#include <intravenous/global_index.h>
#include <intravenous/sample.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <concepts>
#include <compare>
#include <cstdint>
#include <array>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace iv {
    template<typename A>
    requires std::unsigned_integral<A>
    IV_FORCEINLINE A next_power_of_2(A x)
    {
        x--;
        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        if constexpr (sizeof(A) >= 2) x |= x >> 8;
        if constexpr (sizeof(A) >= 4) x |= x >> 16;
        if constexpr (sizeof(A) >= 8) x |= x >> 32;
        if constexpr (sizeof(A) >= 16) x |= x >> 64;
        x++;

        return x;
    }

    struct MidiEvent {
        std::array<std::uint8_t, 3> bytes {};
        std::uint8_t size = 0;

        static constexpr size_t average_event_size_bytes()
        {
            return 4;
        }
    };

    struct TriggerEvent {
        static constexpr size_t average_event_size_bytes()
        {
            return 1;
        }
    };

    struct BoundaryEvent {
        bool is_begin = false;

        static constexpr size_t average_event_size_bytes()
        {
            return 1;
        }
    };

    struct EmptyEvent {
        static constexpr size_t average_event_size_bytes()
        {
            return 0;
        }
    };

    enum class EventTypeId : unsigned int {
        empty,
        trigger,
        boundary,
        midi,
        count,
    };

    enum class PortKind : std::uint8_t {
        sample,
        event,
    };

    [[nodiscard]] constexpr GlobalIndexRange realtime_port_window(
        GlobalIndex block_index,
        size_t block_size,
        size_t history,
        size_t latency) noexcept
    {
        return {
            .begin = block_index.retreated_by(history),
            .extent = static_cast<GlobalIndex::extent_type>(history)
                + static_cast<GlobalIndex::extent_type>(block_size)
                + static_cast<GlobalIndex::extent_type>(latency),
        };
    }

    using Event = std::variant<MidiEvent, TriggerEvent, BoundaryEvent, EmptyEvent>;

    struct TimedEvent {
        GlobalIndex time{};
        Event value {};
    };

    enum class EventConversionStepId : std::uint8_t {
        midi_to_trigger = 0,
        midi_to_boundary = 1,
        midi_to_empty = 2,
        // Values 3 and 4 were the former trigger-to-boundary/MIDI conversions.
        // They intentionally remain unused: a trigger has neither duration nor
        // MIDI note/channel information, so those conversions have no
        // objective semantics.
        trigger_to_empty = 5,
        boundary_to_trigger = 6,
        // Value 7 was the former boundary-to-MIDI conversion. Choosing a MIDI
        // note/channel would be arbitrary, so it is intentionally unavailable.
        boundary_to_empty = 8,
    };

    struct EventConversionPlan {
        static constexpr size_t max_steps = static_cast<size_t>(EventTypeId::count) - 1;

        EventTypeId source_type {};
        EventTypeId target_type {};
        std::array<EventConversionStepId, max_steps> steps {};
        size_t step_count = 0;

        constexpr size_t size() const
        {
            return step_count;
        }

        constexpr EventConversionStepId operator[](size_t index) const
        {
            return steps[index];
        }

        auto operator<=>(EventConversionPlan const&) const = default;
    };

    class EventConversionRegistry {
        struct Score {
            int loss = 0;
            int assumptions = 0;
            int hops = 0;

            constexpr bool operator<(Score const& other) const
            {
                if (loss != other.loss) {
                    return loss < other.loss;
                }
                if (assumptions != other.assumptions) {
                    return assumptions < other.assumptions;
                }
                return hops < other.hops;
            }
        };

        struct Edge {
            EventTypeId source {};
            EventTypeId target {};
            EventConversionStepId step {};
            Score score {};
        };

        static constexpr std::array<Edge, 6> edges() noexcept
        {
            // Only conversions with objective data semantics belong here.
            // Trigger and Empty are sink-like event types: information-rich
            // types may collapse into them, but neither may invent information
            // required by MIDI/Boundary.
            return {{
                { EventTypeId::midi, EventTypeId::trigger, EventConversionStepId::midi_to_trigger, { 1, 0, 1 } },
                { EventTypeId::midi, EventTypeId::boundary, EventConversionStepId::midi_to_boundary, { 1, 0, 1 } },
                { EventTypeId::midi, EventTypeId::empty, EventConversionStepId::midi_to_empty, { 1, 0, 1 } },
                { EventTypeId::trigger, EventTypeId::empty, EventConversionStepId::trigger_to_empty, { 1, 0, 1 } },
                { EventTypeId::boundary, EventTypeId::trigger, EventConversionStepId::boundary_to_trigger, { 1, 0, 1 } },
                { EventTypeId::boundary, EventTypeId::empty, EventConversionStepId::boundary_to_empty, { 1, 0, 1 } },
            }};
        }

        static constexpr size_t type_index(EventTypeId type) noexcept
        {
            return static_cast<size_t>(type);
        }

        static constexpr size_t type_count() noexcept
        {
            return type_index(EventTypeId::count);
        }

        static constexpr bool is_valid_type(EventTypeId type) noexcept
        {
            return type_index(type) < type_count();
        }

        static constexpr bool is_note_on(MidiEvent const& midi)
        {
            if (midi.size < 3) return false;
            std::uint8_t const status = midi.bytes[0] & 0xF0;
            return status == 0x90 && midi.bytes[2] != 0;
        }

        static constexpr bool is_note_off(MidiEvent const& midi)
        {
            if (midi.size < 3) return false;
            std::uint8_t const status = midi.bytes[0] & 0xF0;
            return status == 0x80 || (status == 0x90 && midi.bytes[2] == 0);
        }

        template<typename Emit>
        static constexpr void apply_step(EventConversionStepId step, TimedEvent const& event, Emit&& emit)
        {
            switch (step) {
            case EventConversionStepId::midi_to_trigger:
                if (auto midi = std::get_if<MidiEvent>(&event.value); midi && is_note_on(*midi)) {
                    emit(TimedEvent { .time = event.time, .value = TriggerEvent {} });
                }
                break;
            case EventConversionStepId::midi_to_boundary:
                if (auto midi = std::get_if<MidiEvent>(&event.value)) {
                    if (is_note_on(*midi)) {
                        emit(TimedEvent { .time = event.time, .value = BoundaryEvent { .is_begin = true } });
                    } else if (is_note_off(*midi)) {
                        emit(TimedEvent { .time = event.time, .value = BoundaryEvent { .is_begin = false } });
                    }
                }
                break;
            case EventConversionStepId::midi_to_empty:
                break;
            case EventConversionStepId::trigger_to_empty:
                break;
            case EventConversionStepId::boundary_to_trigger:
                if (auto boundary = std::get_if<BoundaryEvent>(&event.value); boundary && boundary->is_begin) {
                    emit(TimedEvent { .time = event.time, .value = TriggerEvent {} });
                }
                break;
            case EventConversionStepId::boundary_to_empty:
                break;
            }
        }

        static constexpr bool is_nonexpanding_step(
            EventConversionStepId step) noexcept
        {
            switch (step) {
            case EventConversionStepId::midi_to_trigger:
            case EventConversionStepId::midi_to_boundary:
            case EventConversionStepId::midi_to_empty:
            case EventConversionStepId::trigger_to_empty:
            case EventConversionStepId::boundary_to_trigger:
            case EventConversionStepId::boundary_to_empty:
                return true;
            }
            return false;
        }

        template<typename Emit>
        static constexpr void apply_steps_recursive(
            EventConversionPlan const& plan,
            size_t step_index,
            TimedEvent const& event,
            Emit&& emit
        )
        {
            if (step_index >= plan.step_count) {
                emit(event);
                return;
            }

            apply_step(plan.steps[step_index], event, [&](TimedEvent const& converted) {
                apply_steps_recursive(plan, step_index + 1, converted, emit);
            });
        }

    public:
        static constexpr EventConversionRegistry instance()
        {
            return {};
        }

        static constexpr bool is_nonexpanding(
            EventConversionPlan const& plan) noexcept
        {
            for (size_t i = 0; i < plan.step_count; ++i) {
                if (!is_nonexpanding_step(plan.steps[i])) return false;
            }
            return true;
        }

        static constexpr EventConversionPlan plan(EventTypeId source, EventTypeId target)
        {
            if (!is_valid_type(source) || !is_valid_type(target)) {
                throw std::logic_error("invalid event type");
            }

            if (source == target) {
                return EventConversionPlan {
                    .source_type = source,
                    .target_type = target,
                    .steps = {},
                    .step_count = 0,
                };
            }

            std::array<Score, type_count()> best_scores;
            std::array<bool, type_count()> visited {};
            std::array<EventTypeId, type_count()> previous_type;
            std::array<EventConversionStepId, type_count()> previous_step {};

            for (auto& score : best_scores) {
                score = { std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), std::numeric_limits<int>::max() };
            }

            best_scores[type_index(source)] = {};
            previous_type[type_index(source)] = source;

            for (size_t iteration = 0; iteration < type_count(); ++iteration) {
                size_t current = type_count();
                for (size_t i = 0; i < type_count(); ++i) {
                    if (visited[i]) continue;
                    if (current == type_count() || best_scores[i] < best_scores[current]) {
                        current = i;
                    }
                }
                if (current == type_count()
                    || best_scores[current].loss
                        == std::numeric_limits<int>::max()) {
                    break;
                }
                visited[current] = true;

                for (auto const& edge : edges()) {
                    if (type_index(edge.source) != current) continue;
                    size_t const next = type_index(edge.target);
                    Score candidate {
                        best_scores[current].loss + edge.score.loss,
                        best_scores[current].assumptions + edge.score.assumptions,
                        best_scores[current].hops + edge.score.hops,
                    };
                    if (candidate < best_scores[next]) {
                        best_scores[next] = candidate;
                        previous_type[next] = edge.source;
                        previous_step[next] = edge.step;
                    }
                }
            }

            if (best_scores[type_index(target)].loss == std::numeric_limits<int>::max()) {
                throw std::logic_error("no event conversion path is available");
            }

            std::array<EventConversionStepId, EventConversionPlan::max_steps> reversed_steps {};
            size_t reversed_step_count = 0;
            for (EventTypeId current = target; current != source; current = previous_type[type_index(current)]) {
                reversed_steps[reversed_step_count++] = previous_step[type_index(current)];
            }

            EventConversionPlan result {
                .source_type = source,
                .target_type = target,
            };
            for (size_t i = 0; i < reversed_step_count; ++i) {
                result.steps[i] = reversed_steps[reversed_step_count - 1 - i];
            }
            result.step_count = reversed_step_count;

            return result;
        }

        template<typename Emit>
        static constexpr void convert(EventConversionPlan const& plan, TimedEvent const& event, Emit&& emit)
        {
            apply_steps_recursive(plan, 0, event, std::forward<Emit>(emit));
        }
    };

    inline constexpr size_t DEFAULT_EVENT_PORT_BUFFER_BASE_MULTIPLIER = 256;

    inline constexpr size_t average_event_size_bytes(EventTypeId type)
    {
        switch (type) {
        case EventTypeId::midi:
            return MidiEvent::average_event_size_bytes();
        case EventTypeId::trigger:
            return TriggerEvent::average_event_size_bytes();
        case EventTypeId::boundary:
            return BoundaryEvent::average_event_size_bytes();
        case EventTypeId::empty:
            return EmptyEvent::average_event_size_bytes();
        case EventTypeId::count:
            break;
        }
        return 0;
    }

    // Legacy/value-size capacity heuristic. GraphJIT static event-buffer sizing
    // uses EventOutputProperties::max_events_per_index instead; keep this for
    // compatibility and other heuristic/default-policy decisions.
    inline constexpr size_t calculate_event_port_buffer_capacity(size_t base_multiplier, EventTypeId type)
    {
        size_t const average_size = average_event_size_bytes(type);
        if (average_size == 0 || base_multiplier == 0) {
            return 0;
        }

        size_t const budget_bytes = base_multiplier * average_size;
        return next_power_of_2(std::max<size_t>(1, budget_bytes / sizeof(TimedEvent)));
    }

    inline constexpr double DEFAULT_MAX_EVENTS_PER_SAMPLE = 1.0;

    [[nodiscard]] constexpr bool is_valid_event_buffer_rate(
        double max_events_per_index) noexcept
    {
        // Both comparisons are false for NaN; +infinity exceeds max().
        return max_events_per_index >= 0.0
            && max_events_per_index <= std::numeric_limits<double>::max();
    }

    // Static event-buffer sizing rule. For a representation whose relevant
    // temporal span is W samples, reserve ceil(max_events_per_index * W)
    // event entries before any storage power-of-two sequence rounding. This is
    // sizing metadata, not a runtime producer quota. Runtime writers are bounded
    // only by the storage representation capacity; overflow clipping is not a
    // semantic guarantee.
    [[nodiscard]] inline std::optional<size_t> event_count_for_sample_span(
        double max_events_per_index,
        size_t sample_count) noexcept
    {
        if (!is_valid_event_buffer_rate(max_events_per_index)) return std::nullopt;
        if (max_events_per_index == 0.0 || sample_count == 0) return size_t{0};

        long double const product = static_cast<long double>(max_events_per_index)
            * static_cast<long double>(sample_count);
        long double const limit = static_cast<long double>(
            std::numeric_limits<size_t>::max());
        if (!(product >= 0.0L) || product > limit) return std::nullopt;
        return static_cast<size_t>(std::ceil(product));
    }

    // EventSharedPortData uses a power-of-two ring mask. Storage bounded
    // sequences therefore round the requested event count upward while
    // preserving zero-capacity declarations exactly.
    [[nodiscard]] inline std::optional<size_t> event_sequence_capacity_for_sample_span(
        double max_events_per_index,
        size_t sample_count) noexcept
    {
        auto const required = event_count_for_sample_span(
            max_events_per_index, sample_count);
        if (!required) return std::nullopt;
        if (*required == 0) return size_t{0};
        constexpr size_t highest_power_of_two =
            size_t{1} << (std::numeric_limits<size_t>::digits - 1);
        if (*required > highest_power_of_two) return std::nullopt;
        return next_power_of_2(*required);
    }

    inline constexpr size_t MAX_BLOCK_SIZE = size_t(1) << (std::numeric_limits<size_t>::digits - 1);

    IV_FORCEINLINE bool is_power_of_2(size_t n)
    {
        return n && !(n & (n - 1));
    }

    IV_FORCEINLINE constexpr bool is_valid_block_size(size_t block_size)
    {
        return block_size != 0 && block_size <= MAX_BLOCK_SIZE && is_power_of_2(block_size);
    }

    IV_FORCEINLINE void validate_block_size(size_t block_size, char const* message = "block size must be a power of 2")
    {
        if (!is_valid_block_size(block_size)) {
            throw std::logic_error(message);
        }
    }

    IV_FORCEINLINE void validate_max_block_size(size_t block_size, char const* message = "max_block_size must be a power of 2")
    {
        if (block_size == MAX_BLOCK_SIZE) {
            return;
        }
        validate_block_size(block_size, message);
    }

    inline constexpr ChannelLayout mono_planar_channel_layout {
        .channel_type = ChannelTypeId::mono,
        .sample_layout = SampleStreamLayout::planar,
    };

    // The default preserves the scalar ring-buffer view. Multi-channel block
    // access is intentionally expressed through the layout-aware port views;
    // callers cannot accidentally treat interleaved storage as scalar samples.
    template<typename A, ChannelLayout Layout = mono_planar_channel_layout>
    struct BlockView {
        // first/second sizes are logical element counts. A stride greater than
        // one lets a scalar block view alias one channel of interleaved sample
        // storage without gathering it into a contiguous temporary.
        std::span<A> first {};
        std::span<A> second {};
        size_t first_stride = 1;
        size_t second_stride = 1;

        template<typename B = A>
            requires (!std::is_const_v<B>)
        constexpr operator BlockView<std::add_const_t<B>, Layout>() const
        {
            return {
                std::span<std::add_const_t<B>>(first),
                std::span<std::add_const_t<B>>(second),
                first_stride,
                second_stride,
            };
        }

        constexpr size_t size() const
        {
            return first.size() + second.size();
        }

        constexpr bool empty() const
        {
            return size() == 0;
        }

        constexpr A& operator[](size_t index) const
        {
            return index < first.size()
                ? first.data()[index * first_stride]
                : second.data()[(index - first.size()) * second_stride];
        }

        struct iterator {
            using value_type = A;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::forward_iterator_tag;
            using iterator_concept = std::forward_iterator_tag;
            using reference = A;
            using pointer = void;

            A const* first_ptr = nullptr;
            A const* second_ptr = nullptr;
            size_t split = 0;
            size_t first_stride = 1;
            size_t second_stride = 1;
            size_t index = 0;

            constexpr reference operator*() const
            {
                return index < split
                    ? first_ptr[index * first_stride]
                    : second_ptr[(index - split) * second_stride];
            }

            constexpr iterator& operator++()
            {
                ++index;
                return *this;
            }

            constexpr iterator operator++(int)
            {
                auto tmp = *this;
                ++*this;
                return tmp;
            }

            constexpr bool operator==(iterator const&) const = default;
        };

        constexpr iterator begin() const
        {
            return iterator{
                first.data(),
                second.data(),
                first.size(),
                first_stride,
                second_stride,
                0
            };
        }

        constexpr iterator end() const
        {
            return iterator{
                first.data(),
                second.data(),
                first.size(),
                first_stride,
                second_stride,
                size()
            };
        }

        template<typename Dst>
        IV_FORCEINLINE constexpr void copy_to(BlockView<Dst, Layout> dst) const
        {
            IV_ASSERT(size() == dst.size(), "BlockView::copy_to requires matching block sizes");
            if (first_stride == 1 && second_stride == 1
                && dst.first_stride == 1 && dst.second_stride == 1) {
                auto src_first = first;
                auto src_second = second;
                auto dst_first = dst.first;
                auto dst_second = dst.second;

                auto copy_partial = [](auto& source, auto& target) {
                    size_t const n = std::min(source.size(), target.size());
                    std::copy_n(source.data(), n, target.data());
                    source = source.subspan(n);
                    target = target.subspan(n);
                };

                copy_partial(src_first, dst_first);
                copy_partial(src_first, dst_second);
                copy_partial(src_second, dst_first);
                copy_partial(src_second, dst_second);
                return;
            }

            for (size_t i = 0; i < size(); ++i) dst[i] = (*this)[i];
        }
    };

    template<ChannelLayout Layout = mono_planar_channel_layout, typename A>
    IV_FORCEINLINE constexpr BlockView<A, Layout> make_block_view(
        std::span<A> buffer,
        size_t start,
        size_t count
    )
    {
        static_assert(channel_count(Layout) == 1, "multi-channel BlockView must be obtained from a layout-aware port accessor");
        if (count == 0) {
            return {};
        }

        size_t const first_size = std::min(count, buffer.size() - start);
        return {
            buffer.subspan(start, first_size),
            buffer.subspan(0, count - first_size),
        };
    }

    struct SharedPortData {
        std::span<Sample> buffer;
        size_t latency;
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        // Ring positions are frame positions. Storage is frame_capacity times
        // the layout's channel count.
        size_t frame_capacity = 0;

        constexpr explicit SharedPortData(
            std::span<Sample> buffer = {},
            size_t latency = 0,
            ChannelLayout channel_layout = {
                .channel_type = ChannelTypeId::mono,
                .sample_layout = SampleStreamLayout::planar,
            },
            size_t frame_capacity = 0
        ) :
            buffer(buffer),
            latency(latency),
            channel_layout(channel_layout),
            frame_capacity(frame_capacity == 0 ? buffer.size() / channel_count(channel_layout) : frame_capacity)
        {
            IV_ASSERT(buffer.size() == sample_storage_size(channel_layout, this->frame_capacity), "port buffer storage does not match channel layout");
            IV_ASSERT(this->frame_capacity == 0 || is_power_of_2(this->frame_capacity), "port buffer frame capacity should be a power of 2");
        }

        constexpr size_t sample_index(size_t frame, size_t channel) const
        {
            IV_ASSERT(frame < frame_capacity, "port frame index out of bounds");
            IV_ASSERT(channel < channel_count(channel_layout), "port channel index out of bounds");
            return channel_layout.sample_layout == SampleStreamLayout::planar
                ? channel * frame_capacity + frame
                : frame * channel_count(channel_layout) + channel;
        }
    };

    // Value-semantic sample-storage descriptor used by InputPort/OutputPort.
    // Compatibility Graph wiring may still own SharedPortData objects, but the
    // port facades only require this immutable view of the backing storage.
    // GraphJit therefore treats InputPort/OutputPort as invocation-local API
    // facades reconstructed from compiler-resolved bindings, never as
    // persistent connection state or NodeStorage-owned objects. A
    // Tick/persisted output binding may additionally retain invocation-local
    // authored disposition for its post-step recording operation; ordinary
    // ephemeral/Tock output writes do not observe it.
    struct SampleChannelStorageView {
        Sample* storage = nullptr;
        size_t frame_capacity = 0;
        size_t frame_stride = 1;
        size_t frame_delay = 0;

        constexpr Sample& sample(size_t frame) const
        {
            IV_ASSERT(storage != nullptr, "sample channel storage is null");
            IV_ASSERT(frame < frame_capacity, "sample channel frame index out of bounds");
            IV_ASSERT(frame_stride != 0, "sample channel frame stride must be non-zero");
            return storage[frame * frame_stride];
        }

        constexpr size_t frame_index_absolute(GlobalIndex logical_frame) const
        {
            IV_ASSERT(
                frame_capacity != 0 && is_power_of_2(frame_capacity),
                "sample channel capacity must be a power of two");
            auto const mask = frame_capacity - 1;
            auto const delayed = logical_frame.retreated_by(frame_delay);
            return static_cast<size_t>(delayed.bits() & mask);
        }

        constexpr Sample& sample_absolute(GlobalIndex logical_frame) const
        {
            return sample(frame_index_absolute(logical_frame));
        }
    };

    struct SamplePortStorageView {
        std::array<SampleChannelStorageView, maximum_supported_channel_count>
            channels{};
        size_t latency = 0;
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        size_t frame_capacity = 0;

        constexpr explicit SamplePortStorageView(
            std::span<Sample> buffer = {},
            size_t latency = 0,
            ChannelLayout channel_layout = {
                .channel_type = ChannelTypeId::mono,
                .sample_layout = SampleStreamLayout::planar,
            },
            size_t frame_capacity = 0
        ) :
            latency(latency),
            channel_layout(channel_layout),
            frame_capacity(frame_capacity == 0
                ? buffer.size() / channel_count(channel_layout)
                : frame_capacity)
        {
            IV_ASSERT(
                buffer.size() == sample_storage_size(channel_layout, this->frame_capacity),
                "port buffer storage does not match channel layout");
            IV_ASSERT(
                this->frame_capacity == 0 || is_power_of_2(this->frame_capacity),
                "port buffer frame capacity should be a power of 2");
            auto const count = channel_count(channel_layout);
            for (size_t channel = 0; channel < count; ++channel) {
                channels[channel] = SampleChannelStorageView{
                    .storage = buffer.empty()
                        ? nullptr
                        : buffer.data() + (channel_layout.sample_layout
                            == SampleStreamLayout::planar
                                ? channel * this->frame_capacity
                                : channel),
                    .frame_capacity = this->frame_capacity,
                    .frame_stride = channel_layout.sample_layout
                            == SampleStreamLayout::planar
                        ? 1
                        : count,
                };
            }
        }

        constexpr explicit SamplePortStorageView(
            std::array<
                SampleChannelStorageView,
                maximum_supported_channel_count> channels,
            size_t latency,
            ChannelLayout channel_layout,
            size_t frame_capacity
        ) :
            channels(channels),
            latency(latency),
            channel_layout(channel_layout),
            frame_capacity(frame_capacity)
        {
            IV_ASSERT(
                frame_capacity == 0 || is_power_of_2(frame_capacity),
                "port buffer frame capacity should be a power of 2");
            auto const count = channel_count(channel_layout);
            for (size_t channel = 0; channel < count; ++channel) {
                IV_ASSERT(
                    frame_capacity == 0 || channels[channel].storage != nullptr,
                    "active sample channel storage is null");
                IV_ASSERT(
                    channels[channel].frame_capacity != 0
                        && is_power_of_2(channels[channel].frame_capacity),
                    "sample channel capacity must be a power of two");
                IV_ASSERT(
                    channels[channel].frame_stride != 0,
                    "sample channel frame stride must be non-zero");
            }
        }

        constexpr explicit SamplePortStorageView(SharedPortData const& shared_data)
            : SamplePortStorageView(
                shared_data.buffer,
                shared_data.latency,
                shared_data.channel_layout,
                shared_data.frame_capacity)
        {}

        constexpr Sample& sample(size_t frame, size_t channel) const
        {
            IV_ASSERT(channel < channel_count(channel_layout), "port channel index out of bounds");
            return channels[channel].sample(frame);
        }

        // Compatibility helper for callers that retain the original contiguous
        // backing buffer. Channel-granular views should use sample() instead.
        constexpr size_t sample_index(size_t frame, size_t channel) const
        {
            IV_ASSERT(frame < frame_capacity, "port frame index out of bounds");
            IV_ASSERT(
                channel < channel_count(channel_layout),
                "port channel index out of bounds");
            return channel_layout.sample_layout == SampleStreamLayout::planar
                ? channel * frame_capacity + frame
                : frame * channel_count(channel_layout) + channel;
        }

        constexpr BlockView<Sample> channel_block(
            size_t channel, size_t start, size_t count) const
        {
            IV_ASSERT(channel < channel_count(channel_layout), "port channel index out of bounds");
            auto const& source = channels[channel];
            IV_ASSERT(start < source.frame_capacity || count == 0, "port channel block start is out of bounds");
            IV_ASSERT(count <= source.frame_capacity, "port channel block exceeds source capacity");
            if (count == 0) return {};

            auto const first_size = std::min(
                count, source.frame_capacity - start);
            auto const second_size = count - first_size;
            return BlockView<Sample>{
                .first = std::span<Sample>{
                    source.storage + start * source.frame_stride, first_size},
                .second = std::span<Sample>{
                    source.storage, second_size},
                .first_stride = source.frame_stride,
                .second_stride = source.frame_stride,
            };
        }

        constexpr BlockView<Sample> channel_block_absolute(
            size_t channel, GlobalIndex logical_start, size_t count) const
        {
            IV_ASSERT(channel < channel_count(channel_layout), "port channel index out of bounds");
            return channel_block(
                channel,
                channels[channel].frame_index_absolute(logical_start),
                count);
        }
    };

    class InputPort {
        SamplePortStorageView _storage;
        size_t _history;
        size_t _latency_samples = 0;
        GlobalIndex _read_index{};

        friend void advance_input(InputPort&, size_t);
        friend void advance_inputs(std::span<InputPort>, size_t);

    private:
        IV_FORCEINLINE constexpr void advance(size_t amount = 1)
        {
            _read_index = _read_index.advanced_by(amount);
        }

    public:
        explicit InputPort(
            SamplePortStorageView storage,
            size_t history,
            size_t latency_samples = 0,
            GlobalIndex index = {}
        ) :
            _storage(storage),
            _history(history),
            _latency_samples(latency_samples),
            _read_index(index.retreated_by(latency_samples))
        {
            auto const count = channel_count(_storage.channel_layout);
            for (size_t channel = 0; channel < count; ++channel) {
                IV_ASSERT(
                    _storage.channels[channel].frame_capacity != 0
                        && is_power_of_2(
                            _storage.channels[channel].frame_capacity),
                    "input channel frame capacity should be a power of 2");
                IV_ASSERT(
                    _storage.channels[channel].frame_delay
                        < _storage.channels[channel].frame_capacity,
                    "input channel delay must fit its source ring buffer");
                IV_ASSERT(
                    _latency_samples
                        < _storage.channels[channel].frame_capacity,
                    "input latency must fit its source ring buffer");
            }
        }

        explicit InputPort(
            SharedPortData& shared_data,
            size_t history,
            size_t latency_samples = 0,
            GlobalIndex index = {}
        ) : InputPort(
            SamplePortStorageView{shared_data}, history, latency_samples, index)
        {}

        IV_FORCEINLINE constexpr Sample get(size_t offset = 0, size_t channel = 0) const
        {
            if (offset > _history) return 0.0f;
            IV_ASSERT(channel < channel_count(_storage.channel_layout), "port channel index out of bounds");
            return _storage.channels[channel].sample_absolute(
                _read_index.retreated_by(offset));
        }

        IV_FORCEINLINE constexpr Sample get_frame(size_t sample_offset, size_t channel = 0) const
        {
            IV_ASSERT(channel < channel_count(_storage.channel_layout), "port channel index out of bounds");
            return _storage.channels[channel].sample_absolute(
                _read_index.advanced_by(sample_offset));
        }

        IV_FORCEINLINE constexpr BlockView<Sample> get_block(size_t block_size, size_t sample_offset = 0) const
        {
            if (sample_offset > block_size) {
                return {};
            }

            return _storage.channel_block_absolute(
                0,
                _read_index.advanced_by(sample_offset),
                block_size - sample_offset);
        }

        IV_FORCEINLINE constexpr size_t latency() const
        {
            return _latency_samples;
        }

        IV_FORCEINLINE constexpr size_t buffer_size() const
        {
            return _storage.channels[0].frame_capacity;
        }

        IV_FORCEINLINE constexpr ChannelLayout channel_layout() const
        {
            return _storage.channel_layout;
        }
    };

    // Invocation state for a statically known Tick/persisted output binding.
    // OutputPort and EventOutputPort deliberately neither store nor update it.
    enum class OutputDisposition : std::uint8_t {
        untouched,
        written,
        voided,
    };

    class OutputPort {
        SamplePortStorageView _storage;
        size_t _history;
        size_t _latency;
        size_t _position = 0;
        size_t _direct_write_extent = 0;
        ChannelLayout _source_layout;
        ChannelConversionPlan _conversion;

        IV_FORCEINLINE constexpr void write_target_frame_at(
            std::span<Sample const> values, size_t frame)
        {
            IV_ASSERT(values.size() == channel_count(_storage.channel_layout), "output frame does not match target channel layout");
            for (size_t channel = 0; channel < values.size(); ++channel) {
                _storage.sample(frame, channel) = values[channel];
            }
        }

        IV_FORCEINLINE constexpr void write_target_frame(
            std::span<Sample const> values, size_t frame_offset)
        {
            size_t const frame = (_position + _storage.latency + frame_offset) & (buffer_size() - 1);
            write_target_frame_at(values, frame);
        }

    public:
        explicit OutputPort(
            SamplePortStorageView storage,
            size_t history,
            GlobalIndex index = {}
        ) : OutputPort(
            storage,
            history,
            index,
            storage.latency)
        {}

        explicit OutputPort(
            SamplePortStorageView storage,
            size_t history,
            GlobalIndex index,
            size_t latency
        ) :
            _storage(storage),
            _history(history),
            _latency(latency),
            _position(static_cast<size_t>(
                index.bits() & (storage.frame_capacity - 1))),
            _source_layout(storage.channel_layout)
        {
            IV_ASSERT(is_power_of_2(_storage.frame_capacity), "buffer frame capacity should be a power of 2");
            IV_ASSERT(_latency < _storage.frame_capacity, "output latency must fit its shared ring buffer");
        }

        explicit OutputPort(
            SharedPortData& shared_data,
            size_t history,
            GlobalIndex index = {}
        ) :
            OutputPort(
                SamplePortStorageView{shared_data},
                history,
                index)
        {}

        explicit OutputPort(
            SharedPortData& shared_data,
            size_t history,
            GlobalIndex index,
            size_t latency
        ) :
            OutputPort(
                SamplePortStorageView{shared_data},
                history,
                index,
                latency)
        {}

        explicit OutputPort(
            SamplePortStorageView storage,
            size_t history,
            ChannelLayout source_layout,
            ChannelConversionPlan conversion,
            GlobalIndex index = {}
        ) : OutputPort(
            storage,
            history,
            source_layout,
            conversion,
            index,
            storage.latency)
        {}

        explicit OutputPort(
            SamplePortStorageView storage,
            size_t history,
            ChannelLayout source_layout,
            ChannelConversionPlan conversion,
            GlobalIndex index,
            size_t latency
        ) :
            _storage(storage),
            _history(history),
            _latency(latency),
            _position(static_cast<size_t>(
                index.bits() & (storage.frame_capacity - 1))),
            _source_layout(source_layout),
            _conversion(conversion)
        {
            IV_ASSERT(is_power_of_2(_storage.frame_capacity), "buffer frame capacity should be a power of 2");
            IV_ASSERT(_latency < _storage.frame_capacity, "output latency must fit its shared ring buffer");
            IV_ASSERT(_conversion && _conversion.source == _source_layout, "sample edge conversion source layout does not match output layout");
            IV_ASSERT(_conversion.target == _storage.channel_layout, "sample edge conversion target layout does not match output buffer layout");
        }

        explicit OutputPort(
            SharedPortData& shared_data,
            size_t history,
            ChannelLayout source_layout,
            ChannelConversionPlan conversion,
            GlobalIndex index = {}
        ) : OutputPort(
            SamplePortStorageView{shared_data},
                history,
                source_layout,
                conversion,
                index)
        {}

        explicit OutputPort(
            SharedPortData& shared_data,
            size_t history,
            ChannelLayout source_layout,
            ChannelConversionPlan conversion,
            GlobalIndex index,
            size_t latency
        ) : OutputPort(
            SamplePortStorageView{shared_data},
            history,
                source_layout,
                conversion,
                index,
                latency)
        {}

        IV_FORCEINLINE constexpr Sample get(size_t offset = 0, size_t channel = 0) const
        {
            if (offset > _latency + _history) return 0.0f;
            size_t const idx = (
                _position + _storage.latency + buffer_size() - 1 - offset
            ) & (buffer_size() - 1);
            return _storage.sample(idx, channel);
        }

        IV_FORCEINLINE constexpr void write_frame(size_t frame_offset, size_t channel, Sample value)
        {
            IV_ASSERT(_source_layout == _storage.channel_layout, "direct frame writes require matching source and target channel layouts");
            size_t const frame = (_position + _storage.latency + frame_offset) & (buffer_size() - 1);
            _storage.sample(frame, channel) = value;
            _direct_write_extent = std::max(_direct_write_extent, frame_offset + 1);
        }

        IV_FORCEINLINE constexpr void write_block(
            size_t frame_offset,
            size_t channel,
            BlockView<Sample const> const& source
        )
        {
            IV_ASSERT(_source_layout == _storage.channel_layout, "direct block writes require matching source and target channel layouts");
            IV_ASSERT(channel < channel_count(_storage.channel_layout), "output channel index out of bounds");
            IV_ASSERT(frame_offset + source.size() <= buffer_size(), "direct output block write exceeds buffer capacity");
            for (size_t frame = 0; frame < source.size(); ++frame) {
                write_frame(frame_offset + frame, channel, source[frame]);
            }
        }

        IV_FORCEINLINE constexpr void finish_direct_write(size_t frame_count)
        {
            if (_direct_write_extent == 0) {
                return;
            }
            IV_ASSERT(_direct_write_extent <= frame_count, "direct port write exceeded context block size");
            _position = (_position + frame_count) & (buffer_size() - 1);
            _direct_write_extent = 0;
        }

        IV_FORCEINLINE constexpr BlockView<Sample> get_block(size_t block_size, size_t sample_offset = 0) const
        {
            size_t const available = _latency + _history + 1;
            if (sample_offset >= available) {
                return {};
            }
            size_t const count = std::min(block_size, available - sample_offset);
            size_t const start = (
                _position + _storage.latency + buffer_size() - (sample_offset + count)
            ) & (buffer_size() - 1);

            return _storage.channel_block(0, start, count);
        }

        IV_FORCEINLINE constexpr void push(Sample value)
        {
            IV_ASSERT(channel_count(_source_layout) == 1, "push(Sample) requires a mono source output port");
            Sample source[] { value };
            push_frame(source);
        }

        IV_FORCEINLINE constexpr void push_frame(std::span<Sample const> source)
        {
            IV_ASSERT(source.size() == channel_count(_source_layout), "output frame does not match source channel layout");
            Sample converted[2] {};
            if (_conversion) {
                _conversion.convert(source.data(), converted, 1);
                write_target_frame(std::span<Sample const>(converted, channel_count(_storage.channel_layout)), 0);
            } else {
                IV_ASSERT(_source_layout == _storage.channel_layout, "sample output requires a channel conversion plan");
                write_target_frame(source, 0);
            }
            _position = (_position + 1) & (buffer_size() - 1);
        }

        IV_FORCEINLINE constexpr void push_block(std::span<Sample const> samples)
        {
            IV_ASSERT(channel_count(_source_layout) == 1, "push_block(samples) requires a mono source output port");
            for (Sample sample : samples) {
                push(sample);
            }
        }

        IV_FORCEINLINE constexpr void push_block(BlockView<Sample const> samples)
        {
            IV_ASSERT(channel_count(_source_layout) == 1, "push_block(samples) requires a mono source output port");
            for (Sample sample : samples) {
                push(sample);
            }
        }

        IV_FORCEINLINE constexpr void accumulate_block(std::span<Sample const> samples)
        {
            if (samples.empty()) return;
            size_t const start = (
                _position + _storage.latency + buffer_size() - samples.size()
            ) & (buffer_size() - 1);
            auto dst = _storage.channel_block(0, start, samples.size());
            auto src = make_block_view(samples, 0, samples.size());
            for (size_t i = 0; i < samples.size(); ++i) {
                dst[i] += src[i];
            }
        }

        IV_FORCEINLINE constexpr void accumulate_block(BlockView<Sample const> samples)
        {
            if (samples.empty()) return;
            size_t const start = (
                _position + _storage.latency + buffer_size() - samples.size()
            ) & (buffer_size() - 1);
            auto dst = _storage.channel_block(0, start, samples.size());
            for (size_t i = 0; i < samples.size(); ++i) {
                dst[i] += samples[i];
            }
        }

        IV_FORCEINLINE constexpr void push_silence(size_t block_size)
        {
            if (block_size == 0) return;
            size_t const mask = buffer_size() - 1;
            size_t const start = (_position + _storage.latency) & mask;
            auto const channels = channel_count(_storage.channel_layout);
            for (size_t frame_offset = 0; frame_offset < block_size; ++frame_offset) {
                size_t const frame = (start + frame_offset) & mask;
                for (size_t channel = 0; channel < channels; ++channel) {
                    _storage.sample(frame, channel) = 0.0f;
                }
            }
            _position = (_position + block_size) & mask;
        }

        IV_FORCEINLINE constexpr void update(Sample value, size_t offset = 0)
        {
            IV_ASSERT(channel_count(_source_layout) == 1, "update(Sample) requires a mono source output port");
            Sample source[] { value };
            update_frame(source, offset);
        }

        IV_FORCEINLINE constexpr void update_frame(
            std::span<Sample const> source, size_t offset = 0)
        {
            if (offset >= _latency) return;
            IV_ASSERT(source.size() == channel_count(_source_layout), "output frame does not match source channel layout");
            Sample converted[2] {};
            std::span<Sample const> target = source;
            if (_conversion) {
                _conversion.convert(source.data(), converted, 1);
                target = std::span<Sample const>(
                    converted, channel_count(_storage.channel_layout));
            } else {
                IV_ASSERT(_source_layout == _storage.channel_layout, "sample output requires a channel conversion plan");
            }
            size_t const frame = (
                _position + _storage.latency + buffer_size() - 1 - offset
            ) & (buffer_size() - 1);
            write_target_frame_at(target, frame);
        }

        IV_FORCEINLINE constexpr size_t position() const
        {
            return _position;
        }

        IV_FORCEINLINE constexpr BlockView<Sample const> current_block(size_t block_size) const
        {
            size_t const start = (_position + buffer_size() - block_size) & (buffer_size() - 1);
            return static_cast<BlockView<Sample const>>(_storage.channel_block(0, start, block_size));
        }

        // The primary output writes the source-layout ring once.  Converted
        // fan-out branches are completed afterward from that exact block;
        // identity branches bind their InputPorts directly to this ring.
        IV_FORCEINLINE void copy_completed_block_to(
            OutputPort& target, size_t block_size) const
        {
            IV_ASSERT(
                _source_layout == target._source_layout,
                "fan-out branches must preserve the producer source layout");
            IV_ASSERT(block_size <= buffer_size(),
                "fan-out block exceeds source ring capacity");

            Sample frame[2] {};
            size_t const start = (_position + buffer_size() - block_size)
                & (buffer_size() - 1);
            for (size_t frame_i = 0; frame_i < block_size; ++frame_i) {
                size_t const source_frame = (start + frame_i)
                    & (buffer_size() - 1);
                for (size_t channel = 0;
                     channel < channel_count(_source_layout); ++channel) {
                    frame[channel] = _storage.sample(source_frame, channel);
                }
                target.push_frame(std::span<Sample const>(
                    frame, channel_count(_source_layout)));
            }
        }

        IV_FORCEINLINE constexpr size_t buffer_size() const
        {
            return _storage.frame_capacity;
        }

        IV_FORCEINLINE constexpr ChannelLayout channel_layout() const
        {
            return _storage.channel_layout;
        }

        IV_FORCEINLINE constexpr ChannelLayout source_layout() const
        {
            return _source_layout;
        }
    };

    struct EventSharedPortData {
        std::span<TimedEvent> buffer;
        size_t read_index = 0;
        size_t write_index = 0;
        EventTypeId type {};

        constexpr EventSharedPortData(
            std::span<TimedEvent> buffer = {},
            size_t read_index = 0,
            size_t write_index = 0,
            EventTypeId type = {}
        ) :
            buffer(buffer),
            read_index(read_index),
            write_index(write_index),
            type(type)
        {}
    };

    class EventInputPort {
        EventSharedPortData* _shared_data = nullptr;

    public:
        EventInputPort() = default;

        explicit EventInputPort(EventSharedPortData& shared_data) :
            _shared_data(&shared_data)
        {}

        BlockView<TimedEvent const> get_block(
            GlobalIndex block_index, size_t block_size) const
        {
            if (!_shared_data || _shared_data->buffer.empty()) {
                return {};
            }

            auto& shared_data = *_shared_data;
            size_t const mask = shared_data.buffer.size() - 1;

            // EventOutputPort's producer contract keeps this sequence in
            // forward order relative to the owning callback window, so both
            // scans may stop at the first boundary.
            while (shared_data.read_index != shared_data.write_index) {
                TimedEvent const& oldest = shared_data.buffer[shared_data.read_index & mask];
                if (block_index.signed_distance_to(oldest.time) >= 0) {
                    break;
                }
                ++shared_data.read_index;
            }

            size_t const available = shared_data.write_index - shared_data.read_index;
            size_t end = 0;
            while (end < available
                && block_index.forward_distance_to(
                    shared_data.buffer[
                        (shared_data.read_index + end) & mask].time)
                    < block_size) {
                ++end;
            }

            return make_block_view(
                std::span<TimedEvent const>(shared_data.buffer.data(), shared_data.buffer.size()),
                shared_data.read_index & mask,
                end
            );
        }

        template<typename Fn>
        void for_each_in_block(
            GlobalIndex block_index, size_t block_size, Fn&& fn) const
        {
            auto block = get_block(block_index, block_size);
            size_t i = 0;
            for (TimedEvent const& event : block) {
                fn(event, i++);
            }
        }

        EventTypeId type() const
        {
            return _shared_data ? _shared_data->type : EventTypeId::empty;
        }
    };

    // Realtime event producers must append each logical output in
    // nondecreasing forward order from the invocation's legal window begin.
    // A history/latency window may overlap adjacent invocations;
    // GraphJit therefore gives such cyclic producers an invocation-local stream
    // and merges that sorted stream into the retained aggregate. EventOutputPort
    // itself does not sort or add an O(n) release-time validation pass.
    class EventOutputPort {
        EventSharedPortData* _shared_data = nullptr;
        EventTypeId _source_type {};
        std::uint64_t* _overflow_count = nullptr;
        size_t _history = 0;
        size_t _latency = 0;
        bool _has_conversion = false;
        EventConversionPlan _conversion {};
        std::optional<GlobalIndexRange> _active_window {};

        [[nodiscard]] GlobalIndexRange window_for(
            GlobalIndex block_index, size_t block_size) const noexcept
        {
            return realtime_port_window(
                block_index, block_size, _history, _latency);
        }

        static void validate_time(
            TimedEvent const& event, GlobalIndexRange window)
        {
            if (!window.contains(event.time)) {
                throw std::logic_error(
                    "realtime event output timestamp is outside the current "
                    "block + history + latency window");
            }
        }

        void record_overflow() const noexcept
        {
            if (_overflow_count
                && *_overflow_count != std::numeric_limits<std::uint64_t>::max()) {
                ++*_overflow_count;
            }
        }

        void push_in_window(
            TimedEvent const& timed_event, GlobalIndexRange window) const
        {
            // Validate the final converted event as well as the source event.
            // Current built-in conversions preserve timestamps, but keeping the
            // check here makes that a validated property rather than an
            // assumption of the compatibility runtime.
            validate_time(timed_event, window);

            if (!_shared_data) return;

            auto append = [&](TimedEvent const& appended) {
                validate_time(appended, window);
                size_t const available =
                    _shared_data->write_index - _shared_data->read_index;
                if (_shared_data->buffer.empty()
                    || available >= _shared_data->buffer.size()) {
                    record_overflow();
                    return;
                }
                _shared_data->buffer[
                    _shared_data->write_index
                    & (_shared_data->buffer.size() - 1)] = appended;
                ++_shared_data->write_index;
            };
            if (_has_conversion) {
                EventConversionRegistry::instance().convert(
                    _conversion, timed_event,
                    [&](TimedEvent const& converted) { append(converted); });
            } else {
                append(timed_event);
            }
        }

    public:
        EventOutputPort() = default;

        explicit EventOutputPort(
            EventSharedPortData& shared_data,
            EventTypeId source_type,
            size_t history = 0,
            size_t latency = 0,
            std::uint64_t* overflow_count = nullptr
        ) :
            _shared_data(&shared_data),
            _source_type(source_type),
            _overflow_count(overflow_count),
            _history(history),
            _latency(latency)
        {
            if (_shared_data->type != _source_type) {
                throw std::logic_error(
                    "event output source type does not match target storage type");
            }
        }

        explicit EventOutputPort(
            EventSharedPortData& shared_data,
            EventTypeId source_type,
            EventConversionPlan const& conversion,
            size_t history = 0,
            size_t latency = 0,
            std::uint64_t* overflow_count = nullptr
        ) :
            _shared_data(&shared_data),
            _source_type(source_type),
            _overflow_count(overflow_count),
            _history(history),
            _latency(latency),
            _has_conversion(true),
            _conversion(conversion)
        {
            if (_conversion.source_type != _source_type
                || _conversion.target_type != _shared_data->type) {
                throw std::logic_error(
                    "event conversion plan does not match output/storage types");
            }
        }

        void begin_block(GlobalIndex block_index, size_t block_size)
        {
            _active_window = window_for(block_index, block_size);
        }

        void end_block() noexcept
        {
            _active_window.reset();
        }

        void push(
            Event event,
            size_t sample_offset,
            GlobalIndex block_index,
            size_t block_size) const
        {
            auto const time = block_index.advanced_by(sample_offset);
            push_in_window(
                TimedEvent{
                    .time = time,
                    .value = std::move(event),
                },
                window_for(block_index, block_size));
        }

        void push(
            TimedEvent const& timed_event,
            GlobalIndex block_index,
            size_t block_size) const
        {
            push_in_window(
                timed_event,
                window_for(block_index, block_size));
        }

        void push(TimedEvent const& timed_event) const
        {
            if (!_active_window) {
                throw std::logic_error(
                    "realtime event output push requires an active block window");
            }
            push_in_window(timed_event, *_active_window);
        }

        void push_block(
            BlockView<TimedEvent const> events,
            GlobalIndex block_index,
            size_t block_size) const
        {
            auto const window = window_for(block_index, block_size);
            for (TimedEvent const& event : events) {
                push_in_window(event, window);
            }
        }

        void push_block(BlockView<TimedEvent const> events) const
        {
            for (TimedEvent const& event : events) {
                push(event);
            }
        }

        void append_block(BlockView<TimedEvent const> events) const
        {
            push_block(events);
        }

        EventTypeId source_type() const
        {
            return _source_type;
        }

        bool empty_in_block(
            GlobalIndex block_index, size_t block_size) const
        {
            if (!_shared_data) {
                return true;
            }
            return EventInputPort(*_shared_data)
                .get_block(block_index, block_size)
                .size() == 0;
        }
    };

    enum class InputAccessKind : std::uint8_t {
        sequential,
        region,
        coverage,
    };

    enum class OutputProductionKind : std::uint8_t {
        tick,
        region,
        coverage,
    };

    enum class OutputRetention : std::uint8_t {
        ephemeral,
        persisted,
    };

    using InputAccessConfig = InputAccessKind;
    using OutputProductionConfig = OutputProductionKind;

    struct SequentialContinuousInputProperties {
        std::vector<ContinuousSequentialInputMetadata> value_metadata {
            ContinuousSequentialInputMetadata{
                SampleContinuousValueExtension::SequentialInputMetadata{}}};

        constexpr bool operator==(
            SequentialContinuousInputProperties const&) const = default;
    };

    struct RegionContinuousInputProperties {
        std::vector<ContinuousRegionInputMetadata> value_metadata {
            ContinuousRegionInputMetadata{
                SampleContinuousValueExtension::RegionInputMetadata{}}};
        constexpr bool operator==(
            RegionContinuousInputProperties const&) const = default;
    };

    struct CoverageContinuousInputProperties {
        std::vector<ContinuousCoverageInputMetadata> value_metadata {
            ContinuousCoverageInputMetadata{
                SampleContinuousValueExtension::CoverageInputMetadata{}}};
        constexpr bool operator==(
            CoverageContinuousInputProperties const&) const = default;
    };

    using ContinuousInputAccessProperties = std::variant<
        SequentialContinuousInputProperties,
        RegionContinuousInputProperties,
        CoverageContinuousInputProperties>;

    struct ContinuousInputProperties {
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        ContinuousInputAccessProperties access {};

        constexpr bool operator==(
            ContinuousInputProperties const&) const = default;
    };

    struct TickContinuousOutputProperties {
        std::vector<ContinuousTickOutputMetadata> value_metadata {
            ContinuousTickOutputMetadata{
                SampleContinuousValueExtension::TickOutputMetadata{}}};
        constexpr bool operator==(
            TickContinuousOutputProperties const&) const = default;
    };

    struct RegionContinuousOutputProperties {
        std::vector<ContinuousRegionOutputMetadata> value_metadata {
            ContinuousRegionOutputMetadata{
                SampleContinuousValueExtension::RegionOutputMetadata{}}};
        constexpr bool operator==(
            RegionContinuousOutputProperties const&) const = default;
    };

    struct CoverageContinuousOutputProperties {
        std::vector<ContinuousCoverageOutputMetadata> value_metadata {
            ContinuousCoverageOutputMetadata{
                SampleContinuousValueExtension::CoverageOutputMetadata{}}};
        constexpr bool operator==(
            CoverageContinuousOutputProperties const&) const = default;
    };

    using ContinuousOutputProductionProperties = std::variant<
        TickContinuousOutputProperties,
        RegionContinuousOutputProperties,
        CoverageContinuousOutputProperties>;

    struct ContinuousOutputProperties {
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        ContinuousOutputProductionProperties production {};

        constexpr bool operator==(
            ContinuousOutputProperties const&) const = default;
    };

    template<class Metadata>
    [[nodiscard]] constexpr ContinuousValueTypeSet
    continuous_metadata_type_domain(std::vector<Metadata> const& metadata)
    {
        if (metadata.empty()) {
            throw std::invalid_argument(
                "a continuous port must declare at least one value type");
        }
        ContinuousValueTypeSet result;
        for (auto const& value : metadata) {
            auto const type = continuous_metadata_value_type(value);
            if (!is_registered_continuous_value_type(type)
                || result.contains(type)) {
                throw std::invalid_argument(
                    "a continuous port must declare each value type exactly once");
            }
            result |= ContinuousValueTypeSet{type};
        }
        return result;
    }

    template<class Properties>
    [[nodiscard]] constexpr ContinuousValueTypeSet
    continuous_value_type_domain(Properties const& properties)
        requires requires { properties.value_metadata; }
    {
        return continuous_metadata_type_domain(properties.value_metadata);
    }

    [[nodiscard]] constexpr ContinuousValueTypeSet
    continuous_value_type_domain(ContinuousInputProperties const& properties)
    {
        return std::visit([](auto const& access) {
            return continuous_value_type_domain(access);
        }, properties.access);
    }

    [[nodiscard]] constexpr ContinuousValueTypeSet
    continuous_value_type_domain(ContinuousOutputProperties const& properties)
    {
        return std::visit([](auto const& production) {
            return continuous_value_type_domain(production);
        }, properties.production);
    }

    constexpr ChannelLayout effective_channel_layout(ContinuousInputProperties const& config)
    {
        return config.channel_layout;
    }

    constexpr ChannelLayout effective_channel_layout(ContinuousOutputProperties const& config)
    {
        return config.channel_layout;
    }

    struct EventInputProperties {
        EventTypeId type {};
        InputAccessConfig access = InputAccessKind::sequential;
        constexpr bool operator==(EventInputProperties const&) const = default;
    };

    struct EventOutputProperties {
        EventTypeId type {};
        // Static storage-sizing rate. For a representation spanning W samples,
        // GraphJIT reserves ceil(max_events_per_index * W) event entries. This
        // does not constrain how events are distributed among sample timestamps.
        double max_events_per_index = DEFAULT_MAX_EVENTS_PER_SAMPLE;
        OutputProductionConfig production = OutputProductionKind::tick;
        constexpr bool operator==(EventOutputProperties const&) const = default;
    };

    [[nodiscard]] constexpr bool is_random_access(InputAccessConfig const& config)
    {
        return config != InputAccessKind::sequential;
    }

    [[nodiscard]] constexpr bool is_tock(OutputProductionConfig const& config)
    {
        return config != OutputProductionKind::tick;
    }

    [[nodiscard]] constexpr bool is_persisted(OutputRetention retention)
    {
        return retention == OutputRetention::persisted;
    }

    [[nodiscard]] constexpr bool is_sequential(InputAccessConfig const& config)
    {
        return config == InputAccessKind::sequential;
    }

    [[nodiscard]] constexpr bool is_tick(OutputProductionConfig const& config)
    {
        return config == OutputProductionKind::tick;
    }

    template<class Config>
    constexpr void set_continuous_channel_layout(
        Config& config, ChannelLayout layout)
        requires requires { config.channel_layout; config.access; }
    {
        config.channel_layout = layout;
    }

    template<class Config>
    constexpr void set_continuous_channel_layout(
        Config& config, ChannelLayout layout)
        requires requires { config.channel_layout; config.production; }
    {
        config.channel_layout = layout;
    }

    template<class Metadata, class Factory>
    constexpr std::vector<Metadata> default_continuous_metadata(
        ContinuousValueTypeSet value_types,
        Factory factory)
    {
        std::vector<Metadata> result;
        result.reserve(value_types.size());
        for (auto const& type : continuous_value_type_registry) {
            if (!value_types.contains(type.id)) continue;
            result.push_back(factory(type.id));
        }
        return result;
    }

    [[nodiscard]] inline constexpr SequentialContinuousInputProperties
    default_sequential_input_properties(ContinuousValueTypeSet value_types)
    {
        return {
            .value_metadata = default_continuous_metadata<
                ContinuousSequentialInputMetadata>(
                    value_types,
                    default_sequential_input_metadata),
        };
    }

    [[nodiscard]] inline constexpr RegionContinuousInputProperties
    default_region_input_properties(ContinuousValueTypeSet value_types)
    {
        return {
            .value_metadata = default_continuous_metadata<
                ContinuousRegionInputMetadata>(
                    value_types,
                    default_region_input_metadata),
        };
    }

    [[nodiscard]] inline constexpr CoverageContinuousInputProperties
    default_coverage_input_properties(ContinuousValueTypeSet value_types)
    {
        return {
            .value_metadata = default_continuous_metadata<
                ContinuousCoverageInputMetadata>(
                    value_types,
                    default_coverage_input_metadata),
        };
    }

    [[nodiscard]] inline constexpr TickContinuousOutputProperties
    default_tick_output_properties(ContinuousValueTypeSet value_types)
    {
        return {
            .value_metadata = default_continuous_metadata<
                ContinuousTickOutputMetadata>(
                    value_types,
                    default_tick_output_metadata),
        };
    }

    [[nodiscard]] inline constexpr RegionContinuousOutputProperties
    default_region_output_properties(ContinuousValueTypeSet value_types)
    {
        return {
            .value_metadata = default_continuous_metadata<
                ContinuousRegionOutputMetadata>(
                    value_types,
                    default_region_output_metadata),
        };
    }

    [[nodiscard]] inline constexpr CoverageContinuousOutputProperties
    default_coverage_output_properties(ContinuousValueTypeSet value_types)
    {
        return {
            .value_metadata = default_continuous_metadata<
                ContinuousCoverageOutputMetadata>(
                    value_types,
                    default_coverage_output_metadata),
        };
    }

    // Authored declarations keep data kind and temporal/access semantics
    // orthogonal. RandomAccess inputs also expose their ordinary current-block
    // typed wrappers in tick()/tick_block().
    struct InputConfig {
        std::string name {};
        std::variant<ContinuousInputProperties, EventInputProperties> kind {};

        constexpr InputConfig() = default;
        constexpr explicit InputConfig(std::string name)
            : name(std::move(name))
        {}
        constexpr InputConfig(
            std::string name,
            ContinuousInputProperties config)
            : name(std::move(name))
            , kind(std::move(config))
        {}
        constexpr InputConfig(
            std::string name,
            EventInputProperties config)
            : name(std::move(name))
            , kind(std::move(config))
        {}

        constexpr bool operator==(InputConfig const&) const = default;
    };

    struct OutputConfig {
        std::string name {};
        std::variant<ContinuousOutputProperties, EventOutputProperties> kind {};
        OutputRetention retention = OutputRetention::ephemeral;

        constexpr OutputConfig() = default;
        constexpr explicit OutputConfig(std::string name)
            : name(std::move(name))
        {}
        constexpr OutputConfig(
            std::string name,
            ContinuousOutputProperties config,
            OutputRetention retention = OutputRetention::ephemeral)
            : name(std::move(name))
            , kind(std::move(config))
            , retention(retention)
        {}
        constexpr OutputConfig(
            std::string name,
            EventOutputProperties config,
            OutputRetention retention = OutputRetention::ephemeral)
            : name(std::move(name))
            , kind(std::move(config))
            , retention(retention)
        {}


        constexpr bool operator==(OutputConfig const&) const = default;
    };

    // Leaf declarations are heterogeneous and fully typed. Access and
    // production tokens select the concrete declaration type; only reflection
    // converts one of these values into the configured-graph representation.
    struct InputAccess {
        struct Sequential {};
        struct Region {};
        struct Coverage {};

        inline static constexpr Sequential sequential {};
        inline static constexpr Region region {};
        inline static constexpr Coverage coverage {};
    };

    struct OutputProduction {
        struct Tick {};
        struct Region {};
        struct Coverage {};

        inline static constexpr Tick tick {};
        inline static constexpr Region region {};
        inline static constexpr Coverage coverage {};
    };

    template<RegisteredContinuousValue... Types>
    struct SequentialInputConfig {
        static_assert(sizeof...(Types) != 0);
        static_assert(
            ContinuousValueTypeSet::of<Types...>().size() == sizeof...(Types),
            "a continuous port must not repeat a value type");
        using value_types = std::tuple<Types...>;
        static constexpr auto access = InputAccessKind::sequential;
        std::string name {};
        std::tuple<typename ContinuousValueType<Types>::SequentialInputOptions...>
            options {};
        constexpr bool operator==(SequentialInputConfig const&) const = default;
    };

    template<RegisteredContinuousValue... Types>
    struct RegionInputConfig {
        static_assert(sizeof...(Types) != 0);
        static_assert(
            ContinuousValueTypeSet::of<Types...>().size() == sizeof...(Types),
            "a continuous port must not repeat a value type");
        using value_types = std::tuple<Types...>;
        static constexpr auto access = InputAccessKind::region;
        std::string name {};
        std::tuple<typename ContinuousValueType<Types>::RegionInputOptions...>
            options {};
        constexpr bool operator==(RegionInputConfig const&) const = default;
    };

    template<RegisteredContinuousValue... Types>
    struct CoverageInputConfig {
        static_assert(sizeof...(Types) != 0);
        static_assert(
            ContinuousValueTypeSet::of<Types...>().size() == sizeof...(Types),
            "a continuous port must not repeat a value type");
        using value_types = std::tuple<Types...>;
        static constexpr auto access = InputAccessKind::coverage;
        std::string name {};
        std::tuple<typename ContinuousValueType<Types>::CoverageInputOptions...>
            options {};
        constexpr bool operator==(CoverageInputConfig const&) const = default;
    };

    template<RegisteredContinuousValue... Types>
    struct TickOutputConfig {
        static_assert(sizeof...(Types) != 0);
        static_assert(
            ContinuousValueTypeSet::of<Types...>().size() == sizeof...(Types),
            "a continuous port must not repeat a value type");
        using value_types = std::tuple<Types...>;
        static constexpr auto production = OutputProductionKind::tick;
        std::string name {};
        OutputRetention retention = OutputRetention::ephemeral;
        std::tuple<typename ContinuousValueType<Types>::TickOutputOptions...>
            options {};
        constexpr bool operator==(TickOutputConfig const&) const = default;
    };

    template<RegisteredContinuousValue... Types>
    struct RegionOutputConfig {
        static_assert(sizeof...(Types) != 0);
        static_assert(
            ContinuousValueTypeSet::of<Types...>().size() == sizeof...(Types),
            "a continuous port must not repeat a value type");
        using value_types = std::tuple<Types...>;
        static constexpr auto production = OutputProductionKind::region;
        std::string name {};
        OutputRetention retention = OutputRetention::ephemeral;
        std::tuple<typename ContinuousValueType<Types>::RegionOutputOptions...>
            options {};
        constexpr bool operator==(RegionOutputConfig const&) const = default;
    };

    template<RegisteredContinuousValue... Types>
    struct CoverageOutputConfig {
        static_assert(sizeof...(Types) != 0);
        static_assert(
            ContinuousValueTypeSet::of<Types...>().size() == sizeof...(Types),
            "a continuous port must not repeat a value type");
        using value_types = std::tuple<Types...>;
        static constexpr auto production = OutputProductionKind::coverage;
        std::string name {};
        OutputRetention retention = OutputRetention::ephemeral;
        std::tuple<typename ContinuousValueType<Types>::CoverageOutputOptions...>
            options {};
        constexpr bool operator==(CoverageOutputConfig const&) const = default;
    };

    struct SequentialEventInputConfig {
        std::string name {};
        EventTypeId type {};
        static constexpr auto access = InputAccessKind::sequential;
        constexpr bool operator==(
            SequentialEventInputConfig const&) const = default;
    };

    struct RegionEventInputConfig {
        std::string name {};
        EventTypeId type {};
        static constexpr auto access = InputAccessKind::region;
        constexpr bool operator==(RegionEventInputConfig const&) const = default;
    };

    struct CoverageEventInputConfig {
        std::string name {};
        EventTypeId type {};
        static constexpr auto access = InputAccessKind::coverage;
        constexpr bool operator==(CoverageEventInputConfig const&) const = default;
    };

    struct EventOutputOptions {
        double max_events_per_index = DEFAULT_MAX_EVENTS_PER_SAMPLE;
        constexpr bool operator==(EventOutputOptions const&) const = default;
    };

#define IV_EVENT_OUTPUT_CONFIG(Name, Kind)                               \
    struct Name {                                                        \
        std::string name {};                                             \
        EventTypeId type {};                                             \
        EventOutputOptions options {};                                   \
        OutputRetention retention = OutputRetention::ephemeral;          \
        static constexpr auto production = Kind;                         \
        constexpr bool operator==(Name const&) const = default;          \
    }
    IV_EVENT_OUTPUT_CONFIG(TickEventOutputConfig, OutputProductionKind::tick);
    IV_EVENT_OUTPUT_CONFIG(RegionEventOutputConfig, OutputProductionKind::region);
    IV_EVENT_OUTPUT_CONFIG(
        CoverageEventOutputConfig, OutputProductionKind::coverage);
#undef IV_EVENT_OUTPUT_CONFIG

    template<class Config>
    concept AuthoredContinuousInputConfig = requires {
        typename std::remove_cvref_t<Config>::value_types;
        std::remove_cvref_t<Config>::access;
    };

    template<class Config>
    concept AuthoredContinuousOutputConfig = requires {
        typename std::remove_cvref_t<Config>::value_types;
        std::remove_cvref_t<Config>::production;
    };

    template<class Config>
    concept AuthoredEventInputConfig =
        std::same_as<std::remove_cvref_t<Config>, SequentialEventInputConfig>
        || std::same_as<std::remove_cvref_t<Config>, RegionEventInputConfig>
        || std::same_as<std::remove_cvref_t<Config>, CoverageEventInputConfig>;

    template<class Config>
    concept AuthoredEventOutputConfig =
        std::same_as<std::remove_cvref_t<Config>, TickEventOutputConfig>
        || std::same_as<std::remove_cvref_t<Config>, RegionEventOutputConfig>
        || std::same_as<std::remove_cvref_t<Config>, CoverageEventOutputConfig>;

    template<class Config>
    concept AuthoredInputConfig =
        AuthoredContinuousInputConfig<Config> || AuthoredEventInputConfig<Config>;

    template<class Config>
    concept AuthoredOutputConfig =
        AuthoredContinuousOutputConfig<Config> || AuthoredEventOutputConfig<Config>;

    template<class Tuple>
    struct ContinuousValueTupleSet;

    template<RegisteredContinuousValue... Types>
    struct ContinuousValueTupleSet<std::tuple<Types...>> {
        inline static constexpr auto value =
            ContinuousValueTypeSet::of<Types...>();
    };

    template<class Config>
    requires (
        AuthoredContinuousInputConfig<Config>
        || AuthoredContinuousOutputConfig<Config>)
    inline constexpr auto authored_continuous_value_types_v =
        ContinuousValueTupleSet<
            typename std::remove_cvref_t<Config>::value_types>::value;

#define IV_DEFINE_CONTINUOUS_INPUT_HELPERS(Token, Config, Options)       \
    template<RegisteredContinuousValue... Types>                         \
    [[nodiscard]] constexpr auto input(                                  \
        std::string name, InputAccess::Token)                            \
    {                                                                    \
        return Config<Types...>{.name = std::move(name)};                \
    }                                                                    \
                                                                         \
    template<RegisteredContinuousValue... Types>                         \
    [[nodiscard]] constexpr auto input(                                  \
        std::string name,                                                \
        InputAccess::Token,                                              \
        typename ContinuousValueType<Types>::Options... options)         \
        requires(sizeof...(Types) != 0)                                  \
    {                                                                    \
        return Config<Types...>{                                         \
            .name = std::move(name),                                     \
            .options = {std::move(options)...},                          \
        };                                                               \
    }
    IV_DEFINE_CONTINUOUS_INPUT_HELPERS(
        Sequential, SequentialInputConfig, SequentialInputOptions)
    IV_DEFINE_CONTINUOUS_INPUT_HELPERS(
        Region, RegionInputConfig, RegionInputOptions)
    IV_DEFINE_CONTINUOUS_INPUT_HELPERS(
        Coverage, CoverageInputConfig, CoverageInputOptions)
#undef IV_DEFINE_CONTINUOUS_INPUT_HELPERS

#define IV_DEFINE_CONTINUOUS_OUTPUT_HELPERS(Token, Config, Options)      \
    template<RegisteredContinuousValue... Types>                         \
    [[nodiscard]] constexpr auto output(                                 \
        std::string name,                                                \
        OutputProduction::Token,                                         \
        OutputRetention retention = OutputRetention::ephemeral)          \
    {                                                                    \
        return Config<Types...>{                                         \
            .name = std::move(name),                                     \
            .retention = retention,                                      \
        };                                                               \
    }                                                                    \
                                                                         \
    template<RegisteredContinuousValue... Types>                         \
    [[nodiscard]] constexpr auto output(                                 \
        std::string name,                                                \
        OutputProduction::Token,                                         \
        typename ContinuousValueType<Types>::Options... options)         \
        requires(sizeof...(Types) != 0)                                  \
    {                                                                    \
        return Config<Types...>{                                         \
            .name = std::move(name),                                     \
            .options = {std::move(options)...},                          \
        };                                                               \
    }                                                                    \
                                                                         \
    template<RegisteredContinuousValue... Types>                         \
    [[nodiscard]] constexpr auto output(                                 \
        std::string name,                                                \
        OutputProduction::Token,                                         \
        OutputRetention retention,                                       \
        typename ContinuousValueType<Types>::Options... options)         \
        requires(sizeof...(Types) != 0)                                  \
    {                                                                    \
        return Config<Types...>{                                         \
            .name = std::move(name),                                     \
            .retention = retention,                                      \
            .options = {std::move(options)...},                          \
        };                                                               \
    }
    IV_DEFINE_CONTINUOUS_OUTPUT_HELPERS(
        Tick, TickOutputConfig, TickOutputOptions)
    IV_DEFINE_CONTINUOUS_OUTPUT_HELPERS(
        Region, RegionOutputConfig, RegionOutputOptions)
    IV_DEFINE_CONTINUOUS_OUTPUT_HELPERS(
        Coverage, CoverageOutputConfig, CoverageOutputOptions)
#undef IV_DEFINE_CONTINUOUS_OUTPUT_HELPERS

    [[nodiscard]] constexpr SequentialEventInputConfig event_input(
        std::string name, EventTypeId type, InputAccess::Sequential)
    {
        return {.name = std::move(name), .type = type};
    }

    [[nodiscard]] constexpr RegionEventInputConfig event_input(
        std::string name, EventTypeId type, InputAccess::Region)
    {
        return {.name = std::move(name), .type = type};
    }

    [[nodiscard]] constexpr CoverageEventInputConfig event_input(
        std::string name, EventTypeId type, InputAccess::Coverage)
    {
        return {.name = std::move(name), .type = type};
    }

#define IV_DEFINE_EVENT_OUTPUT_HELPER(Token, Config)                     \
    [[nodiscard]] constexpr Config event_output(                         \
        std::string name,                                                \
        EventTypeId type,                                                \
        OutputProduction::Token,                                         \
        EventOutputOptions options)                                      \
    {                                                                    \
        return {                                                         \
            .name = std::move(name),                                     \
            .type = type,                                                \
            .options = options,                                          \
        };                                                               \
    }                                                                    \
                                                                         \
    [[nodiscard]] constexpr Config event_output(                         \
        std::string name,                                                \
        EventTypeId type,                                                \
        OutputProduction::Token,                                         \
        OutputRetention retention = OutputRetention::ephemeral,          \
        EventOutputOptions options = {})                                 \
    {                                                                    \
        return {                                                         \
            .name = std::move(name),                                     \
            .type = type,                                                \
            .options = options,                                          \
            .retention = retention,                                      \
        };                                                               \
    }
    IV_DEFINE_EVENT_OUTPUT_HELPER(Tick, TickEventOutputConfig)
    IV_DEFINE_EVENT_OUTPUT_HELPER(Region, RegionEventOutputConfig)
    IV_DEFINE_EVENT_OUTPUT_HELPER(Coverage, CoverageEventOutputConfig)
#undef IV_DEFINE_EVENT_OUTPUT_HELPER

    template<class Tuple>
    concept PortConfigTuple = requires {
        typename std::tuple_size<std::remove_cvref_t<Tuple>>::type;
    };

    template<PortConfigTuple Tuple>
    inline constexpr std::size_t port_config_count_v =
        std::tuple_size_v<std::remove_cvref_t<Tuple>>;

    template<PortConfigTuple Tuple, class Function>
    constexpr void for_each_port_config(Tuple&& configs, Function&& function)
    {
        std::apply(
            [&function](auto&&... config) {
                (std::invoke(function, decltype(config)(config)), ...);
            },
            std::forward<Tuple>(configs));
    }

    template<PortConfigTuple Tuple, class Predicate>
    [[nodiscard]] constexpr bool any_port_config(
        Tuple&& configs, Predicate&& predicate)
    {
        bool result = false;
        for_each_port_config(
            std::forward<Tuple>(configs),
            [&result, &predicate](auto&& config) {
                result = result || std::invoke(
                    predicate, decltype(config)(config));
            });
        return result;
    }

    template<PortConfigTuple Tuple, class Predicate>
    [[nodiscard]] constexpr std::size_t count_port_configs(
        Tuple&& configs, Predicate&& predicate)
    {
        std::size_t result = 0;
        for_each_port_config(
            std::forward<Tuple>(configs),
            [&result, &predicate](auto&& config) {
                result += static_cast<std::size_t>(std::invoke(
                    predicate, decltype(config)(config)));
            });
        return result;
    }

    template<PortConfigTuple Tuple>
    [[nodiscard]] constexpr std::size_t count_continuous_ports(Tuple const& configs)
    {
        return count_port_configs(
            configs, [](auto const& config) { return is_continuous(config); });
    }

    template<PortConfigTuple Tuple>
    [[nodiscard]] constexpr std::size_t count_event_ports(Tuple const& configs)
    {
        return port_config_count_v<Tuple> - count_continuous_ports(configs);
    }

    template<std::size_t Index, PortConfigTuple Tuple>
    constexpr decltype(auto) port_config_at(Tuple&& configs)
    {
        return std::get<Index>(std::forward<Tuple>(configs));
    }

    template<AuthoredContinuousInputConfig Config>
    [[nodiscard]] constexpr bool is_continuous(Config const&) { return true; }

    template<AuthoredContinuousOutputConfig Config>
    [[nodiscard]] constexpr bool is_continuous(Config const&) { return true; }

    template<AuthoredEventInputConfig Config>
    [[nodiscard]] constexpr bool is_continuous(Config const&) { return false; }

    template<AuthoredEventOutputConfig Config>
    [[nodiscard]] constexpr bool is_continuous(Config const&) { return false; }

    template<AuthoredInputConfig Config>
    [[nodiscard]] constexpr bool is_random_access(Config const&)
    {
        return Config::access != InputAccessKind::sequential;
    }

    template<AuthoredInputConfig Config>
    [[nodiscard]] constexpr bool is_sequential(Config const&)
    {
        return Config::access == InputAccessKind::sequential;
    }

    template<AuthoredOutputConfig Config>
    [[nodiscard]] constexpr bool is_tock(Config const&)
    {
        return Config::production != OutputProductionKind::tick;
    }

    template<AuthoredOutputConfig Config>
    [[nodiscard]] constexpr bool is_tick(Config const&)
    {
        return Config::production == OutputProductionKind::tick;
    }

    template<AuthoredOutputConfig Config>
    [[nodiscard]] constexpr bool is_persisted(Config const& config)
    {
        return is_persisted(config.retention);
    }

    namespace port_config_details {
        template<class Options>
        constexpr void include_channel_layout(
            std::optional<ChannelLayout>& result,
            Options const& options)
        {
            if constexpr (requires { options.channel_layout; }) {
                if (result && *result != options.channel_layout) {
                    throw "continuous alternatives must use one channel layout";
                }
                result = options.channel_layout;
            }
        }

        template<RegisteredContinuousValue... Types, class Config, std::size_t... I>
        constexpr ChannelLayout common_channel_layout(
            Config const& config, std::index_sequence<I...>)
        {
            std::optional<ChannelLayout> result;
            (include_channel_layout(result, std::get<I>(config.options)), ...);
            return result.value_or(
                continuous_value_details::default_audio_channel_layout);
        }

        template<RegisteredContinuousValue... Types, class Config, std::size_t... I>
        InputConfig erase_continuous_input(
            Config const& config, std::index_sequence<I...> indices)
        {
            ContinuousInputProperties properties {
                .channel_layout = common_channel_layout<Types...>(config, indices),
            };

            if constexpr (Config::access == InputAccessKind::sequential) {
                std::vector<ContinuousSequentialInputMetadata> metadata;
                metadata.reserve(sizeof...(Types));
                (metadata.emplace_back(ContinuousValueType<Types>::metadata(
                    std::get<I>(config.options))), ...);
                SequentialContinuousInputProperties access {
                    .value_metadata = std::move(metadata),
                };
                properties.access = std::move(access);
            } else if constexpr (Config::access == InputAccessKind::region) {
                std::vector<ContinuousRegionInputMetadata> metadata;
                metadata.reserve(sizeof...(Types));
                (metadata.emplace_back(ContinuousValueType<Types>::metadata(
                    std::get<I>(config.options))), ...);
                properties.access = RegionContinuousInputProperties{
                    .value_metadata = std::move(metadata),
                };
            } else {
                std::vector<ContinuousCoverageInputMetadata> metadata;
                metadata.reserve(sizeof...(Types));
                (metadata.emplace_back(ContinuousValueType<Types>::metadata(
                    std::get<I>(config.options))), ...);
                properties.access = CoverageContinuousInputProperties{
                    .value_metadata = std::move(metadata),
                };
            }

            return {
                config.name,
                std::move(properties),
            };
        }

        template<RegisteredContinuousValue... Types, class Config, std::size_t... I>
        OutputConfig erase_continuous_output(
            Config const& config, std::index_sequence<I...> indices)
        {
            ContinuousOutputProperties properties {
                .channel_layout = common_channel_layout<Types...>(config, indices),
            };

            if constexpr (Config::production == OutputProductionKind::tick) {
                std::vector<ContinuousTickOutputMetadata> metadata;
                metadata.reserve(sizeof...(Types));
                (metadata.emplace_back(ContinuousValueType<Types>::metadata(
                    std::get<I>(config.options))), ...);
                properties.production = TickContinuousOutputProperties{
                    .value_metadata = std::move(metadata),
                };
            } else if constexpr (
                Config::production == OutputProductionKind::region) {
                std::vector<ContinuousRegionOutputMetadata> metadata;
                metadata.reserve(sizeof...(Types));
                (metadata.emplace_back(ContinuousValueType<Types>::metadata(
                    std::get<I>(config.options))), ...);
                properties.production = RegionContinuousOutputProperties{
                    .value_metadata = std::move(metadata),
                };
            } else {
                std::vector<ContinuousCoverageOutputMetadata> metadata;
                metadata.reserve(sizeof...(Types));
                (metadata.emplace_back(ContinuousValueType<Types>::metadata(
                    std::get<I>(config.options))), ...);
                properties.production = CoverageContinuousOutputProperties{
                    .value_metadata = std::move(metadata),
                };
            }

            return {
                config.name,
                std::move(properties),
                config.retention,
            };
        }
    }

    template<AuthoredContinuousInputConfig Config>
    [[nodiscard]] constexpr ChannelLayout effective_channel_layout(
        Config const& config)
    {
        std::optional<ChannelLayout> result;
        std::apply([&](auto const&... options) {
            (port_config_details::include_channel_layout(result, options), ...);
        }, config.options);
        return result.value_or(
            continuous_value_details::default_audio_channel_layout);
    }

    template<AuthoredContinuousOutputConfig Config>
    [[nodiscard]] constexpr ChannelLayout effective_channel_layout(
        Config const& config)
    {
        std::optional<ChannelLayout> result;
        std::apply([&](auto const&... options) {
            (port_config_details::include_channel_layout(result, options), ...);
        }, config.options);
        return result.value_or(
            continuous_value_details::default_audio_channel_layout);
    }

    template<RegisteredContinuousValue... Types>
    [[nodiscard]] InputConfig erase_port_config(
        SequentialInputConfig<Types...> const& config)
    {
        return port_config_details::erase_continuous_input<Types...>(
            config, std::index_sequence_for<Types...>{});
    }

    template<RegisteredContinuousValue... Types>
    [[nodiscard]] InputConfig erase_port_config(
        RegionInputConfig<Types...> const& config)
    {
        return port_config_details::erase_continuous_input<Types...>(
            config, std::index_sequence_for<Types...>{});
    }

    template<RegisteredContinuousValue... Types>
    [[nodiscard]] InputConfig erase_port_config(
        CoverageInputConfig<Types...> const& config)
    {
        return port_config_details::erase_continuous_input<Types...>(
            config, std::index_sequence_for<Types...>{});
    }

    template<RegisteredContinuousValue... Types>
    [[nodiscard]] OutputConfig erase_port_config(
        TickOutputConfig<Types...> const& config)
    {
        return port_config_details::erase_continuous_output<Types...>(
            config, std::index_sequence_for<Types...>{});
    }

    template<RegisteredContinuousValue... Types>
    [[nodiscard]] OutputConfig erase_port_config(
        RegionOutputConfig<Types...> const& config)
    {
        return port_config_details::erase_continuous_output<Types...>(
            config, std::index_sequence_for<Types...>{});
    }

    template<RegisteredContinuousValue... Types>
    [[nodiscard]] OutputConfig erase_port_config(
        CoverageOutputConfig<Types...> const& config)
    {
        return port_config_details::erase_continuous_output<Types...>(
            config, std::index_sequence_for<Types...>{});
    }

    template<AuthoredEventInputConfig Config>
    [[nodiscard]] InputConfig erase_port_config(Config const& config)
    {
        return {
            config.name,
            EventInputProperties{
                .type = config.type,
                .access = static_cast<InputAccessConfig>(Config::access),
            },
        };
    }

    template<AuthoredEventOutputConfig Config>
    [[nodiscard]] OutputConfig erase_port_config(Config const& config)
    {
        return {
            config.name,
            EventOutputProperties{
                .type = config.type,
                .max_events_per_index = config.options.max_events_per_index,
                .production = static_cast<OutputProductionConfig>(
                    Config::production),
            },
            config.retention,
        };
    }

    // The configured graph keeps storage sample/event lists because lowering
    // uses separate sample and event collections. Production and retention
    // remain independent; Tock output configs have no Tick history/latency.
    struct EventInputConfig {
        std::string name {};
        EventTypeId type {};
        InputAccessConfig access = InputAccessKind::sequential;
        constexpr bool operator==(EventInputConfig const&) const = default;
    };

    struct EventOutputConfig {
        std::string name {};
        EventTypeId type {};
        double max_events_per_index = DEFAULT_MAX_EVENTS_PER_SAMPLE;
        OutputProductionConfig production = OutputProductionKind::tick;
        OutputRetention retention = OutputRetention::ephemeral;
        constexpr bool operator==(EventOutputConfig const&) const = default;
    };

    struct ContinuousInputConfig {
        std::string name {};
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        ContinuousInputAccessProperties access {};
        constexpr bool operator==(ContinuousInputConfig const&) const = default;
    };

    struct ContinuousOutputConfig {
        std::string name {};
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        ContinuousOutputProductionProperties production {};
        OutputRetention retention = OutputRetention::ephemeral;
        constexpr bool operator==(ContinuousOutputConfig const&) const = default;
    };

    [[nodiscard]] constexpr ContinuousValueTypeSet
    continuous_value_type_domain(ContinuousInputConfig const& config)
    {
        return std::visit([](auto const& access) {
            return continuous_value_type_domain(access);
        }, config.access);
    }

    [[nodiscard]] constexpr ContinuousValueTypeSet
    continuous_value_type_domain(ContinuousOutputConfig const& config)
    {
        return std::visit([](auto const& production) {
            return continuous_value_type_domain(production);
        }, config.production);
    }

    constexpr ChannelLayout effective_channel_layout(ContinuousInputConfig const& config)
    {
        return config.channel_layout;
    }

    constexpr ChannelLayout effective_channel_layout(ContinuousOutputConfig const& config)
    {
        return config.channel_layout;
    }

    [[nodiscard]] constexpr SequentialContinuousInputProperties const&
    sequential_input_properties(ContinuousInputProperties const& config)
    {
        return std::get<SequentialContinuousInputProperties>(config.access);
    }

    [[nodiscard]] constexpr SequentialContinuousInputProperties const&
    sequential_input_properties(ContinuousInputConfig const& config)
    {
        return std::get<SequentialContinuousInputProperties>(config.access);
    }

    template<RegisteredContinuousValue Type>
    [[nodiscard]] constexpr auto const& sequential_input_metadata(
        SequentialContinuousInputProperties const& properties)
    {
        using Metadata =
            typename ContinuousValueType<Type>::SequentialInputMetadata;
        for (auto const& metadata : properties.value_metadata) {
            if (auto const* value = std::get_if<Metadata>(&metadata)) {
                return *value;
            }
        }
        throw std::invalid_argument(
            "Sequential input does not declare the requested continuous value type");
    }

    [[nodiscard]] constexpr InputAccessKind input_access(
        ContinuousInputAccessProperties const& access)
    {
        return std::visit([](auto const& properties) {
            using Properties = std::remove_cvref_t<decltype(properties)>;
            if constexpr (std::same_as<
                              Properties,
                              SequentialContinuousInputProperties>) {
                return InputAccessKind::sequential;
            } else if constexpr (std::same_as<
                                     Properties,
                                     RegionContinuousInputProperties>) {
                return InputAccessKind::region;
            } else {
                static_assert(std::same_as<
                    Properties, CoverageContinuousInputProperties>);
                return InputAccessKind::coverage;
            }
        }, access);
    }

    [[nodiscard]] constexpr InputAccessKind input_access(
        ContinuousInputProperties const& config)
    {
        return input_access(config.access);
    }

    [[nodiscard]] constexpr InputAccessKind input_access(
        ContinuousInputConfig const& config)
    {
        return input_access(config.access);
    }

    [[nodiscard]] constexpr InputAccessKind input_access(
        EventInputProperties const& config)
    {
        return config.access;
    }

    [[nodiscard]] constexpr InputAccessKind input_access(
        EventInputConfig const& config)
    {
        return config.access;
    }

    [[nodiscard]] constexpr InputAccessKind input_access(InputConfig const& config)
    {
        return std::visit([](auto const& properties) {
            return input_access(properties);
        }, config.kind);
    }

    [[nodiscard]] constexpr OutputProductionKind output_production(
        ContinuousOutputProductionProperties const& production)
    {
        return std::visit([](auto const& properties) {
            using Properties = std::remove_cvref_t<decltype(properties)>;
            if constexpr (std::same_as<
                              Properties,
                              TickContinuousOutputProperties>) {
                return OutputProductionKind::tick;
            } else if constexpr (std::same_as<
                                     Properties,
                                     RegionContinuousOutputProperties>) {
                return OutputProductionKind::region;
            } else {
                static_assert(std::same_as<
                    Properties, CoverageContinuousOutputProperties>);
                return OutputProductionKind::coverage;
            }
        }, production);
    }

    [[nodiscard]] constexpr OutputProductionKind output_production(
        ContinuousOutputProperties const& config)
    {
        return output_production(config.production);
    }

    [[nodiscard]] constexpr OutputProductionKind output_production(
        ContinuousOutputConfig const& config)
    {
        return output_production(config.production);
    }

    [[nodiscard]] constexpr OutputProductionKind output_production(
        EventOutputProperties const& config)
    {
        return config.production;
    }

    [[nodiscard]] constexpr OutputProductionKind output_production(
        EventOutputConfig const& config)
    {
        return config.production;
    }

    [[nodiscard]] constexpr OutputProductionKind output_production(
        OutputConfig const& config)
    {
        return std::visit([](auto const& properties) {
            return output_production(properties);
        }, config.kind);
    }

    template<class Config>
    [[nodiscard]] constexpr bool is_random_access(Config const& config)
        requires requires { input_access(config); }
    {
        return is_random_access(input_access(config));
    }

    template<class Config>
    [[nodiscard]] constexpr bool is_tock(Config const& config)
        requires requires { output_production(config); }
    {
        return is_tock(output_production(config));
    }

    template<class Config>
    [[nodiscard]] constexpr bool is_sequential(Config const& config)
        requires requires { input_access(config); }
    {
        return is_sequential(input_access(config));
    }

    template<class Config>
    [[nodiscard]] constexpr bool is_tick(Config const& config)
        requires requires { output_production(config); }
    {
        return is_tick(output_production(config));
    }

    [[nodiscard]] constexpr bool is_persisted(OutputConfig const& config)
    {
        return is_persisted(config.retention);
    }

    [[nodiscard]] constexpr bool is_persisted(ContinuousOutputConfig const& config)
    {
        return is_persisted(config.retention);
    }

    [[nodiscard]] constexpr bool is_persisted(EventOutputConfig const& config)
    {
        return is_persisted(config.retention);
    }

    [[nodiscard]] constexpr ContinuousInputConfig materialize_continuous_config(
        InputConfig const& config)
    {
        auto const& properties = std::get<ContinuousInputProperties>(config.kind);
        return {
            .name = config.name,
            .channel_layout = properties.channel_layout,
            .access = properties.access,
        };
    }

    [[nodiscard]] constexpr ContinuousOutputConfig materialize_continuous_config(
        OutputConfig const& config)
    {
        auto const& properties = std::get<ContinuousOutputProperties>(config.kind);
        return {
            .name = config.name,
            .channel_layout = properties.channel_layout,
            .production = properties.production,
            .retention = config.retention,
        };
    }

    [[nodiscard]] constexpr EventInputConfig materialize_event_config(
        InputConfig const& config)
    {
        return {
            .name = config.name,
            .type = std::get<EventInputProperties>(config.kind).type,
            .access = input_access(config),
        };
    }

    [[nodiscard]] constexpr EventOutputConfig materialize_event_config(
        OutputConfig const& config)
    {
        auto const& properties = std::get<EventOutputProperties>(config.kind);
        return {
            .name = config.name,
            .type = properties.type,
            .max_events_per_index = properties.max_events_per_index,
            .production = output_production(config),
            .retention = config.retention,
        };
    }

    [[nodiscard]] constexpr InputConfig make_input_config(
        ContinuousInputConfig const& config)
    {
        return {
            config.name,
            ContinuousInputProperties{
                .channel_layout = config.channel_layout,
                .access = config.access,
            },
        };
    }

    [[nodiscard]] constexpr InputConfig make_input_config(
        EventInputConfig const& config)
    {
        return {
            config.name,
            EventInputProperties{.type = config.type, .access = config.access},
        };
    }

    [[nodiscard]] constexpr OutputConfig make_output_config(
        ContinuousOutputConfig const& config)
    {
        return {
            config.name,
            ContinuousOutputProperties{
                .channel_layout = config.channel_layout,
                .production = config.production,
            },
            config.retention,
        };
    }

    [[nodiscard]] constexpr OutputConfig make_output_config(
        EventOutputConfig const& config)
    {
        return {
            config.name,
            EventOutputProperties{
                .type = config.type,
                .max_events_per_index = config.max_events_per_index,
                .production = config.production,
            },
            config.retention,
        };
    }

    [[nodiscard]] constexpr bool is_continuous(InputConfig const& config)
    {
        return std::holds_alternative<ContinuousInputProperties>(config.kind);
    }

    [[nodiscard]] constexpr bool is_continuous(OutputConfig const& config)
    {
        return std::holds_alternative<ContinuousOutputProperties>(config.kind);
    }

    template<typename Ports>
    constexpr size_t count_continuous_ports(Ports const& ports)
    {
        size_t result = 0;
        for (auto const& port : ports) {
            result += is_continuous(port);
        }
        return result;
    }

    template<typename Ports>
    constexpr size_t count_event_ports(Ports const& ports)
    {
        return ports.size() - count_continuous_ports(ports);
    }

    [[nodiscard]] constexpr ContinuousInputProperties const& continuous_properties(
        InputConfig const& config)
    {
        return std::get<ContinuousInputProperties>(config.kind);
    }

    [[nodiscard]] constexpr ContinuousOutputProperties const& continuous_properties(
        OutputConfig const& config)
    {
        return std::get<ContinuousOutputProperties>(config.kind);
    }

    [[nodiscard]] constexpr EventInputProperties const& event_properties(
        InputConfig const& config)
    {
        return std::get<EventInputProperties>(config.kind);
    }

    [[nodiscard]] constexpr EventOutputProperties const& event_properties(
        OutputConfig const& config)
    {
        return std::get<EventOutputProperties>(config.kind);
    }

    IV_FORCEINLINE void advance_input(InputPort& input, size_t amount = 1)
    {
        input.advance(amount);
    }

    IV_FORCEINLINE void advance_inputs(std::span<InputPort> inputs, size_t amount)
    {
        for (InputPort& input : inputs) {
            advance_input(input, amount);
        }
    }

}
