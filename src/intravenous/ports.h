#pragma once

#include <intravenous/compat.h>
#include <intravenous/channel_layout.h>
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

    // A global DSP timeline position. Unlike a buffer offset or event offset,
    // this remains meaningful across sequential and arbitrary-access work.
    using SampleIndex = std::uint64_t;

    using EventTime = size_t;

    struct RealtimePortWindow {
        SampleIndex begin = 0;
        SampleIndex end = 0; // exclusive

        [[nodiscard]] constexpr bool contains(SampleIndex index) const noexcept
        {
            return index >= begin && index < end;
        }

        constexpr bool operator==(RealtimePortWindow const&) const = default;
    };

    [[nodiscard]] constexpr SampleIndex saturating_sample_index_add(
        SampleIndex base, size_t delta) noexcept
    {
        auto const max = std::numeric_limits<SampleIndex>::max();
        if (delta > max - base) {
            return max;
        }
        return base + static_cast<SampleIndex>(delta);
    }

    [[nodiscard]] constexpr RealtimePortWindow realtime_port_window(
        SampleIndex block_index,
        size_t block_size,
        size_t history,
        size_t latency) noexcept
    {
        SampleIndex const history_samples = history > block_index
            ? block_index
            : static_cast<SampleIndex>(history);
        auto const block_end = saturating_sample_index_add(block_index, block_size);
        return {
            .begin = block_index - history_samples,
            .end = saturating_sample_index_add(block_end, latency),
        };
    }

    using Event = std::variant<MidiEvent, TriggerEvent, BoundaryEvent, EmptyEvent>;

    struct TimedEvent {
        EventTime time = 0;
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
            // Only conversions with objective payload semantics belong here.
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
    // uses EventOutputProperties::max_events_per_sample instead; keep this for
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
        double max_events_per_sample) noexcept
    {
        // Both comparisons are false for NaN; +infinity exceeds max().
        return max_events_per_sample >= 0.0
            && max_events_per_sample <= std::numeric_limits<double>::max();
    }

    // Static event-buffer sizing rule. For a representation whose relevant
    // temporal span is W samples, reserve ceil(max_events_per_sample * W)
    // event slots before any physical power-of-two sequence rounding. This is
    // not a runtime constraint on how those events are distributed by timestamp.
    [[nodiscard]] inline std::optional<size_t> event_count_for_sample_span(
        double max_events_per_sample,
        size_t sample_count) noexcept
    {
        if (!is_valid_event_buffer_rate(max_events_per_sample)) return std::nullopt;
        if (max_events_per_sample == 0.0 || sample_count == 0) return size_t{0};

        long double const product = static_cast<long double>(max_events_per_sample)
            * static_cast<long double>(sample_count);
        long double const limit = static_cast<long double>(
            std::numeric_limits<size_t>::max());
        if (!(product >= 0.0L) || product > limit) return std::nullopt;
        return static_cast<size_t>(std::ceil(product));
    }

    // EventSharedPortData uses a power-of-two ring mask. Physical bounded
    // sequences therefore round the requested event count upward while
    // preserving zero-capacity declarations exactly.
    [[nodiscard]] inline std::optional<size_t> event_sequence_capacity_for_sample_span(
        double max_events_per_sample,
        size_t sample_count) noexcept
    {
        auto const required = event_count_for_sample_span(
            max_events_per_sample, sample_count);
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
        std::span<A> first {};
        std::span<A> second {};

        template<typename B = A>
            requires (!std::is_const_v<B>)
        constexpr operator BlockView<std::add_const_t<B>, Layout>() const
        {
            return {
                std::span<std::add_const_t<B>>(first),
                std::span<std::add_const_t<B>>(second),
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
                ? first[index]
                : second[index - first.size()];
        }

        struct iterator {
            using value_type = A;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::forward_iterator_tag;
            using iterator_concept = std::forward_iterator_tag;
            using reference = A;
            using pointer = void;

            A const* first_ptr = nullptr;
            size_t split = 0;
            std::ptrdiff_t second_offset = 0;
            size_t index = 0;

            constexpr reference operator*() const
            {
                return index < split
                    ? first_ptr[index]
                    : (first_ptr + second_offset)[index - split];
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
                first.size(),
                second.data() - first.data(),
                0
            };
        }

        constexpr iterator end() const
        {
            return iterator{
                first.data(),
                first.size(),
                second.data() - first.data(),
                size()
            };
        }

        template<typename Dst>
        IV_FORCEINLINE constexpr void copy_to(BlockView<Dst, Layout> dst) const
        {
            IV_ASSERT(size() == dst.size(), "BlockView::copy_to requires matching block sizes");

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
    // facades reconstructed from immutable compiler bindings, never as
    // persistent connection state or NodeStorage-owned objects.
    struct SamplePortStorageView {
        std::span<Sample> buffer;
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
            buffer(buffer),
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
        }

        constexpr explicit SamplePortStorageView(SharedPortData const& shared_data)
            : SamplePortStorageView(
                shared_data.buffer,
                shared_data.latency,
                shared_data.channel_layout,
                shared_data.frame_capacity)
        {}

        constexpr size_t sample_index(size_t frame, size_t channel) const
        {
            IV_ASSERT(frame < frame_capacity, "port frame index out of bounds");
            IV_ASSERT(channel < channel_count(channel_layout), "port channel index out of bounds");
            return channel_layout.sample_layout == SampleStreamLayout::planar
                ? channel * frame_capacity + frame
                : frame * channel_count(channel_layout) + channel;
        }
    };

    class InputPort {
        SamplePortStorageView _storage;
        size_t _history;
        size_t _latency_samples = 0;
        size_t _read_position = 0;

        friend void advance_input(InputPort&, size_t);
        friend void advance_inputs(std::span<InputPort>, size_t);

        IV_FORCEINLINE constexpr size_t current_read_position() const
        {
            return _read_position & (buffer_size() - 1);
        }

    private:
        IV_FORCEINLINE constexpr void advance(size_t amount = 1)
        {
            _read_position = (_read_position + amount) & (buffer_size() - 1);
        }

    public:
        explicit InputPort(
            SamplePortStorageView storage,
            size_t history,
            size_t latency_samples = 0,
            SampleIndex index = 0
        ) :
            _storage(storage),
            _history(history),
            _latency_samples(latency_samples)
        {
            IV_ASSERT(is_power_of_2(_storage.frame_capacity), "buffer frame capacity should be a power of 2");
            IV_ASSERT(_latency_samples < _storage.frame_capacity, "input latency must fit its shared ring buffer");
            auto const mask = _storage.frame_capacity - 1;
            _read_position = static_cast<size_t>(index & mask);
            _read_position = (_read_position + _storage.frame_capacity
                - _latency_samples) & mask;
        }

        explicit InputPort(
            SharedPortData& shared_data,
            size_t history,
            size_t latency_samples = 0,
            SampleIndex index = 0
        ) : InputPort(
            SamplePortStorageView{shared_data}, history, latency_samples, index)
        {}

        IV_FORCEINLINE constexpr Sample get(size_t offset = 0, size_t channel = 0) const
        {
            if (offset > _history) return 0.0f;
            size_t const idx = (current_read_position() + buffer_size() - offset) & (buffer_size() - 1);
            return _storage.buffer[_storage.sample_index(idx, channel)];
        }

        IV_FORCEINLINE constexpr Sample get_frame(size_t sample_offset, size_t channel = 0) const
        {
            size_t const sample = (current_read_position() + sample_offset) & (buffer_size() - 1);
            return _storage.buffer[_storage.sample_index(sample, channel)];
        }

        IV_FORCEINLINE constexpr BlockView<Sample> get_block(size_t block_size, size_t sample_offset = 0) const
        {
            if (sample_offset > block_size) {
                return {};
            }

            size_t const start = (current_read_position() + sample_offset) & (buffer_size() - 1);
            return make_block_view(_storage.buffer, start, block_size - sample_offset);
        }

        IV_FORCEINLINE constexpr size_t latency() const
        {
            return _latency_samples;
        }

        IV_FORCEINLINE constexpr size_t buffer_size() const
        {
            return _storage.frame_capacity;
        }

        IV_FORCEINLINE constexpr ChannelLayout channel_layout() const
        {
            return _storage.channel_layout;
        }
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
                _storage.buffer[_storage.sample_index(frame, channel)] = values[channel];
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
            SampleIndex index = 0
        ) : OutputPort(storage, history, index, storage.latency)
        {}

        explicit OutputPort(
            SamplePortStorageView storage,
            size_t history,
            SampleIndex index,
            size_t latency
        ) :
            _storage(storage),
            _history(history),
            _latency(latency),
            _position(static_cast<size_t>(index & (storage.frame_capacity - 1))),
            _source_layout(storage.channel_layout)
        {
            IV_ASSERT(is_power_of_2(_storage.frame_capacity), "buffer frame capacity should be a power of 2");
            IV_ASSERT(_latency < _storage.frame_capacity, "output latency must fit its shared ring buffer");
        }

        explicit OutputPort(
            SharedPortData& shared_data,
            size_t history,
            SampleIndex index = 0
        ) :
            OutputPort(SamplePortStorageView{shared_data}, history, index)
        {}

        explicit OutputPort(
            SharedPortData& shared_data,
            size_t history,
            SampleIndex index,
            size_t latency
        ) :
            OutputPort(
                SamplePortStorageView{shared_data}, history, index, latency)
        {}

        explicit OutputPort(
            SamplePortStorageView storage,
            size_t history,
            ChannelLayout source_layout,
            ChannelConversionPlan conversion,
            SampleIndex index = 0
        ) : OutputPort(
            storage, history, source_layout, conversion, index, storage.latency)
        {}

        explicit OutputPort(
            SamplePortStorageView storage,
            size_t history,
            ChannelLayout source_layout,
            ChannelConversionPlan conversion,
            SampleIndex index,
            size_t latency
        ) :
            _storage(storage),
            _history(history),
            _latency(latency),
            _position(static_cast<size_t>(index & (storage.frame_capacity - 1))),
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
            SampleIndex index = 0
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
            SampleIndex index,
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
            return _storage.buffer[_storage.sample_index(idx, channel)];
        }

        IV_FORCEINLINE constexpr void write_frame(size_t frame_offset, size_t channel, Sample value)
        {
            IV_ASSERT(_source_layout == _storage.channel_layout, "direct frame writes require matching source and target channel layouts");
            size_t const frame = (_position + _storage.latency + frame_offset) & (buffer_size() - 1);
            _storage.buffer[_storage.sample_index(frame, channel)] = value;
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
            size_t const count = std::min(block_size, available - sample_offset);
            size_t const start = (
                _position + _storage.latency + buffer_size() - (sample_offset + count)
            ) & (buffer_size() - 1);

            return make_block_view(_storage.buffer, start, count);
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
            size_t const start = (
                _position + _storage.latency + buffer_size() - samples.size()
            ) & (buffer_size() - 1);
            auto dst = make_block_view(_storage.buffer, start, samples.size());
            auto src = make_block_view(samples, 0, samples.size());
            for (size_t i = 0; i < samples.size(); ++i) {
                dst[i] += src[i];
            }
        }

        IV_FORCEINLINE constexpr void accumulate_block(BlockView<Sample const> samples)
        {
            size_t const start = (
                _position + _storage.latency + buffer_size() - samples.size()
            ) & (buffer_size() - 1);
            auto dst = make_block_view(_storage.buffer, start, samples.size());
            for (size_t i = 0; i < samples.size(); ++i) {
                dst[i] += samples[i];
            }
        }

        IV_FORCEINLINE constexpr void push_silence(size_t block_size)
        {
            size_t const start = (_position + _storage.latency) & (buffer_size() - 1);
            auto block = make_block_view(_storage.buffer, start, block_size);
            std::fill(block.first.begin(), block.first.end(), 0.0f);
            std::fill(block.second.begin(), block.second.end(), 0.0f);
            _position = (_position + block_size) & (buffer_size() - 1);
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
            return make_block_view(std::span<Sample const>(_storage.buffer), start, block_size);
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
                    frame[channel] = _storage.buffer[
                        _storage.sample_index(source_frame, channel)];
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

        BlockView<TimedEvent const> get_block(size_t block_index, size_t block_size) const
        {
            if (!_shared_data || _shared_data->buffer.empty()) {
                return {};
            }

            auto& shared_data = *_shared_data;
            size_t const block_end = block_index + block_size;
            size_t const mask = shared_data.buffer.size() - 1;

            // EventOutputPort's producer contract keeps this sequence sorted
            // by absolute time, so both scans may stop at the first boundary.
            while (shared_data.read_index != shared_data.write_index) {
                TimedEvent const& oldest = shared_data.buffer[shared_data.read_index & mask];
                if (oldest.time >= block_index) {
                    break;
                }
                ++shared_data.read_index;
            }

            size_t const available = shared_data.write_index - shared_data.read_index;
            size_t end = 0;
            while (end < available && shared_data.buffer[(shared_data.read_index + end) & mask].time < block_end) {
                ++end;
            }

            return make_block_view(
                std::span<TimedEvent const>(shared_data.buffer.data(), shared_data.buffer.size()),
                shared_data.read_index & mask,
                end
            );
        }

        template<typename Fn>
        void for_each_in_block(size_t block_index, size_t block_size, Fn&& fn) const
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
    // nondecreasing absolute TimedEvent::time order across sequential
    // tick/tick_block invocations and any slices of one root call. Consumers
    // and GraphJit merge paths rely on this ordering contract; EventOutputPort
    // does not sort the stream or add an O(n) release-time validation pass.
    class EventOutputPort {
        EventSharedPortData* _shared_data = nullptr;
        EventTypeId _source_type {};
        std::uint64_t* _overflow_count = nullptr;
        size_t _history = 0;
        size_t _latency = 0;
        bool _has_conversion = false;
        EventConversionPlan _conversion {};
        std::optional<RealtimePortWindow> _active_window {};

        [[nodiscard]] RealtimePortWindow window_for(
            SampleIndex block_index, size_t block_size) const noexcept
        {
            return realtime_port_window(
                block_index, block_size, _history, _latency);
        }

        static void validate_time(
            TimedEvent const& event, RealtimePortWindow window)
        {
            auto const time = static_cast<SampleIndex>(event.time);
            if (!window.contains(time)) {
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
            TimedEvent const& timed_event, RealtimePortWindow window) const
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

        void begin_block(SampleIndex block_index, size_t block_size)
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
            size_t block_index,
            size_t block_size) const
        {
            auto const time = saturating_sample_index_add(
                static_cast<SampleIndex>(block_index), sample_offset);
            push_in_window(
                TimedEvent{
                    .time = static_cast<EventTime>(time),
                    .value = std::move(event),
                },
                window_for(static_cast<SampleIndex>(block_index), block_size));
        }

        void push(
            TimedEvent const& timed_event,
            size_t block_index,
            size_t block_size) const
        {
            push_in_window(
                timed_event,
                window_for(static_cast<SampleIndex>(block_index), block_size));
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
            size_t block_index,
            size_t block_size) const
        {
            auto const window = window_for(
                static_cast<SampleIndex>(block_index), block_size);
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

        bool empty_in_block(size_t block_index, size_t block_size) const
        {
            if (!_shared_data) {
                return true;
            }
            return EventInputPort(*_shared_data)
                .get_block(block_index, block_size)
                .size() == 0;
        }
    };

    // The sample/event distinction is one axis of a logical port declaration.
    // Its temporal access model is a separate axis: realtime ports have a
    // finite history/latency contract, while compiled ports are random-access
    // and therefore do not carry realtime timing requirements.
    struct SampleInputProperties {
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        // The total-read value for an unavailable or out-of-range compiled
        // input. This is deliberately separate from default_value, which is
        // the value used for an ordinary disconnected sequential input.
        Sample neutral_value = 0.0;
        Sample default_value = 0.0;
        Sample min = -std::numeric_limits<Sample::storage>::infinity();
        Sample max = std::numeric_limits<Sample::storage>::infinity();
    };

    struct SampleOutputProperties {
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
    };

    constexpr ChannelLayout effective_channel_layout(SampleInputProperties const& config)
    {
        return config.channel_layout;
    }

    constexpr ChannelLayout effective_channel_layout(SampleOutputProperties const& config)
    {
        return config.channel_layout;
    }

    struct EventInputProperties {
        EventTypeId type {};
    };

    struct EventOutputProperties {
        EventTypeId type {};
        // Static storage-sizing rate. For a representation spanning W samples,
        // GraphJIT reserves ceil(max_events_per_sample * W) event slots. This
        // does not constrain how events are distributed among sample timestamps.
        double max_events_per_sample = DEFAULT_MAX_EVENTS_PER_SAMPLE;
    };

    struct RealtimeInputConfig {
        size_t history = 0;

        constexpr bool operator==(RealtimeInputConfig const&) const = default;
    };

    struct RealtimeOutputConfig {
        size_t history = 0;
        size_t latency = 0;

        constexpr bool operator==(RealtimeOutputConfig const&) const = default;
    };

    struct CompiledPortConfig {
        constexpr bool operator==(CompiledPortConfig const&) const = default;
    };

    using InputAccessConfig = std::variant<RealtimeInputConfig, CompiledPortConfig>;
    using OutputAccessConfig = std::variant<RealtimeOutputConfig, CompiledPortConfig>;

    inline constexpr CompiledPortConfig compiled_port {};

    [[nodiscard]] constexpr bool is_compiled(InputAccessConfig const& config)
    {
        return std::holds_alternative<CompiledPortConfig>(config);
    }

    [[nodiscard]] constexpr bool is_compiled(OutputAccessConfig const& config)
    {
        return std::holds_alternative<CompiledPortConfig>(config);
    }

    [[nodiscard]] constexpr bool is_realtime(InputAccessConfig const& config)
    {
        return std::holds_alternative<RealtimeInputConfig>(config);
    }

    [[nodiscard]] constexpr bool is_realtime(OutputAccessConfig const& config)
    {
        return std::holds_alternative<RealtimeOutputConfig>(config);
    }

    [[nodiscard]] constexpr size_t realtime_history(InputAccessConfig const& config)
    {
        return std::get<RealtimeInputConfig>(config).history;
    }

    [[nodiscard]] constexpr size_t realtime_history(OutputAccessConfig const& config)
    {
        return std::get<RealtimeOutputConfig>(config).history;
    }

    [[nodiscard]] constexpr size_t realtime_latency(OutputAccessConfig const& config)
    {
        return std::get<RealtimeOutputConfig>(config).latency;
    }

    [[nodiscard]] constexpr size_t realtime_history_or_zero(
        InputAccessConfig const& config)
    {
        if (auto const* realtime = std::get_if<RealtimeInputConfig>(&config)) {
            return realtime->history;
        }
        return 0;
    }

    [[nodiscard]] constexpr size_t realtime_history_or_zero(
        OutputAccessConfig const& config)
    {
        if (auto const* realtime = std::get_if<RealtimeOutputConfig>(&config)) {
            return realtime->history;
        }
        return 0;
    }

    [[nodiscard]] constexpr size_t realtime_latency_or_zero(
        OutputAccessConfig const& config)
    {
        if (auto const* realtime = std::get_if<RealtimeOutputConfig>(&config)) {
            return realtime->latency;
        }
        return 0;
    }

    [[nodiscard]] constexpr InputAccessConfig inward_input_access(
        OutputAccessConfig const& config)
    {
        if (auto const* realtime = std::get_if<RealtimeOutputConfig>(&config)) {
            return RealtimeInputConfig{.history = realtime->history};
        }
        return CompiledPortConfig{};
    }

    [[nodiscard]] constexpr OutputAccessConfig inward_output_access(
        InputAccessConfig const& config)
    {
        if (auto const* realtime = std::get_if<RealtimeInputConfig>(&config)) {
            return RealtimeOutputConfig{.history = realtime->history};
        }
        return CompiledPortConfig{};
    }

    // Authored declarations keep payload kind and temporal/access semantics
    // orthogonal. Compiled ports still expose their ordinary current-block
    // typed wrappers in tick()/tick_block(); the additive capability lives in
    // that accessor surface rather than in a realtime timing declaration.
    struct InputConfig {
        std::string name {};
        std::variant<SampleInputProperties, EventInputProperties> kind {};
        InputAccessConfig access {RealtimeInputConfig{}};

        constexpr InputConfig() = default;
        constexpr explicit InputConfig(std::string name)
            : name(std::move(name))
        {}
        constexpr InputConfig(
            std::string name,
            SampleInputProperties config,
            InputAccessConfig access = RealtimeInputConfig{})
            : name(std::move(name))
            , kind(std::move(config))
            , access(std::move(access))
        {}
        constexpr InputConfig(
            std::string name,
            EventInputProperties config,
            InputAccessConfig access = RealtimeInputConfig{})
            : name(std::move(name))
            , kind(std::move(config))
            , access(std::move(access))
        {}
    };

    struct OutputConfig {
        std::string name {};
        std::variant<SampleOutputProperties, EventOutputProperties> kind {};
        OutputAccessConfig access {RealtimeOutputConfig{}};

        constexpr OutputConfig() = default;
        constexpr explicit OutputConfig(std::string name)
            : name(std::move(name))
        {}
        constexpr OutputConfig(
            std::string name,
            SampleOutputProperties config,
            OutputAccessConfig access = RealtimeOutputConfig{})
            : name(std::move(name))
            , kind(std::move(config))
            , access(std::move(access))
        {}
        constexpr OutputConfig(
            std::string name,
            EventOutputProperties config,
            OutputAccessConfig access = RealtimeOutputConfig{})
            : name(std::move(name))
            , kind(std::move(config))
            , access(std::move(access))
        {}
    };

    [[nodiscard]] constexpr InputConfig sample_input(
        std::string name = {},
        SampleInputProperties properties = {},
        InputAccessConfig access = RealtimeInputConfig{})
    {
        return InputConfig{
            std::move(name), std::move(properties), std::move(access)};
    }

    [[nodiscard]] constexpr OutputConfig sample_output(
        std::string name = {},
        SampleOutputProperties properties = {},
        OutputAccessConfig access = RealtimeOutputConfig{})
    {
        return OutputConfig{
            std::move(name),
            std::move(properties),
            std::move(access)};
    }

    [[nodiscard]] constexpr InputConfig event_input(
        std::string name = {},
        EventTypeId type = {},
        InputAccessConfig access = RealtimeInputConfig{})
    {
        return InputConfig{
            std::move(name),
            EventInputProperties{.type = type},
            std::move(access)};
    }

    [[nodiscard]] constexpr OutputConfig event_output(
        std::string name = {},
        EventTypeId type = {},
        OutputAccessConfig access = RealtimeOutputConfig{})
    {
        return OutputConfig{
            std::move(name),
            EventOutputProperties{.type = type},
            std::move(access)};
    }

    [[nodiscard]] constexpr OutputConfig event_output(
        std::string name,
        EventOutputProperties properties,
        OutputAccessConfig access = RealtimeOutputConfig{})
    {
        return OutputConfig{
            std::move(name), std::move(properties), std::move(access)};
    }

    [[nodiscard]] constexpr InputConfig compiled_sample_input(
        std::string name = {},
        SampleInputProperties properties = {})
    {
        return sample_input(std::move(name), std::move(properties), compiled_port);
    }

    [[nodiscard]] constexpr OutputConfig compiled_sample_output(
        std::string name = {},
        SampleOutputProperties properties = {})
    {
        return sample_output(std::move(name), std::move(properties), compiled_port);
    }

    [[nodiscard]] constexpr InputConfig compiled_event_input(
        std::string name = {},
        EventTypeId type = {})
    {
        return event_input(std::move(name), type, compiled_port);
    }

    [[nodiscard]] constexpr OutputConfig compiled_event_output(
        std::string name = {},
        EventTypeId type = {})
    {
        return event_output(std::move(name), type, compiled_port);
    }

    [[nodiscard]] constexpr OutputConfig compiled_event_output(
        std::string name,
        EventOutputProperties properties)
    {
        return event_output(std::move(name), std::move(properties), compiled_port);
    }

    [[nodiscard]] constexpr InputConfig realtime_sample_input(
        std::string name = {},
        SampleInputProperties properties = {},
        RealtimeInputConfig access = {})
    {
        return sample_input(std::move(name), std::move(properties), std::move(access));
    }

    [[nodiscard]] constexpr OutputConfig realtime_sample_output(
        std::string name = {},
        SampleOutputProperties properties = {},
        RealtimeOutputConfig access = {})
    {
        return sample_output(std::move(name), std::move(properties), std::move(access));
    }

    [[nodiscard]] constexpr InputConfig realtime_event_input(
        std::string name = {},
        EventTypeId type = {},
        RealtimeInputConfig access = {})
    {
        return event_input(std::move(name), type, std::move(access));
    }

    [[nodiscard]] constexpr OutputConfig realtime_event_output(
        std::string name = {},
        EventTypeId type = {},
        RealtimeOutputConfig access = {})
    {
        return event_output(std::move(name), type, std::move(access));
    }

    [[nodiscard]] constexpr OutputConfig realtime_event_output(
        std::string name,
        EventOutputProperties properties,
        RealtimeOutputConfig access = {})
    {
        return event_output(
            std::move(name), std::move(properties), std::move(access));
    }

    // The configured graph keeps physical sample/event lists because lowering
    // uses separate sample and event collections. It preserves the same access
    // variant instead of flattening compiled ports back into meaningless
    // realtime history/latency fields.
    struct EventInputConfig {
        std::string name {};
        EventTypeId type {};
        InputAccessConfig access {RealtimeInputConfig{}};
    };

    struct EventOutputConfig {
        std::string name {};
        EventTypeId type {};
        double max_events_per_sample = DEFAULT_MAX_EVENTS_PER_SAMPLE;
        OutputAccessConfig access {RealtimeOutputConfig{}};
    };

    struct SampleInputConfig {
        std::string name {};
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        InputAccessConfig access {RealtimeInputConfig{}};
        Sample neutral_value = 0.0;
        Sample default_value = 0.0;
        Sample min = -std::numeric_limits<Sample::storage>::infinity();
        Sample max = std::numeric_limits<Sample::storage>::infinity();
    };

    struct SampleOutputConfig {
        std::string name {};
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        OutputAccessConfig access {RealtimeOutputConfig{}};
    };

    constexpr ChannelLayout effective_channel_layout(SampleInputConfig const& config)
    {
        return config.channel_layout;
    }

    constexpr ChannelLayout effective_channel_layout(SampleOutputConfig const& config)
    {
        return config.channel_layout;
    }

    [[nodiscard]] constexpr bool is_compiled(InputConfig const& config)
    {
        return is_compiled(config.access);
    }

    [[nodiscard]] constexpr bool is_compiled(OutputConfig const& config)
    {
        return is_compiled(config.access);
    }

    [[nodiscard]] constexpr bool is_compiled(SampleInputConfig const& config)
    {
        return is_compiled(config.access);
    }

    [[nodiscard]] constexpr bool is_compiled(SampleOutputConfig const& config)
    {
        return is_compiled(config.access);
    }

    [[nodiscard]] constexpr bool is_compiled(EventInputConfig const& config)
    {
        return is_compiled(config.access);
    }

    [[nodiscard]] constexpr bool is_compiled(EventOutputConfig const& config)
    {
        return is_compiled(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_history(InputConfig const& config)
    {
        return realtime_history(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_history(OutputConfig const& config)
    {
        return realtime_history(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_history(SampleInputConfig const& config)
    {
        return realtime_history(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_history(SampleOutputConfig const& config)
    {
        return realtime_history(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_history(EventInputConfig const& config)
    {
        return realtime_history(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_history(EventOutputConfig const& config)
    {
        return realtime_history(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_latency(OutputConfig const& config)
    {
        return realtime_latency(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_latency(SampleOutputConfig const& config)
    {
        return realtime_latency(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_latency(EventOutputConfig const& config)
    {
        return realtime_latency(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_history_or_zero(
        InputConfig const& config)
    {
        return realtime_history_or_zero(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_history_or_zero(
        OutputConfig const& config)
    {
        return realtime_history_or_zero(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_history_or_zero(
        SampleInputConfig const& config)
    {
        return realtime_history_or_zero(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_history_or_zero(
        SampleOutputConfig const& config)
    {
        return realtime_history_or_zero(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_history_or_zero(
        EventInputConfig const& config)
    {
        return realtime_history_or_zero(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_history_or_zero(
        EventOutputConfig const& config)
    {
        return realtime_history_or_zero(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_latency_or_zero(
        OutputConfig const& config)
    {
        return realtime_latency_or_zero(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_latency_or_zero(
        SampleOutputConfig const& config)
    {
        return realtime_latency_or_zero(config.access);
    }

    [[nodiscard]] constexpr size_t realtime_latency_or_zero(
        EventOutputConfig const& config)
    {
        return realtime_latency_or_zero(config.access);
    }

    [[nodiscard]] constexpr SampleInputConfig materialize_sample_config(
        InputConfig const& config)
    {
        auto const& properties = std::get<SampleInputProperties>(config.kind);
        return {
            .name = config.name,
            .channel_layout = properties.channel_layout,
            .access = config.access,
            .neutral_value = properties.neutral_value,
            .default_value = properties.default_value,
            .min = properties.min,
            .max = properties.max,
        };
    }

    [[nodiscard]] constexpr SampleOutputConfig materialize_sample_config(
        OutputConfig const& config)
    {
        auto const& properties = std::get<SampleOutputProperties>(config.kind);
        return {
            .name = config.name,
            .channel_layout = properties.channel_layout,
            .access = config.access,
        };
    }

    [[nodiscard]] constexpr EventInputConfig materialize_event_config(
        InputConfig const& config)
    {
        return {
            .name = config.name,
            .type = std::get<EventInputProperties>(config.kind).type,
            .access = config.access,
        };
    }

    [[nodiscard]] constexpr EventOutputConfig materialize_event_config(
        OutputConfig const& config)
    {
        auto const& properties = std::get<EventOutputProperties>(config.kind);
        return {
            .name = config.name,
            .type = properties.type,
            .max_events_per_sample = properties.max_events_per_sample,
            .access = config.access,
        };
    }

    [[nodiscard]] constexpr InputConfig make_input_config(
        SampleInputConfig const& config)
    {
        return {
            config.name,
            SampleInputProperties{
                .channel_layout = config.channel_layout,
                .neutral_value = config.neutral_value,
                .default_value = config.default_value,
                .min = config.min,
                .max = config.max,
            },
            config.access,
        };
    }

    [[nodiscard]] constexpr InputConfig make_input_config(
        EventInputConfig const& config)
    {
        return {
            config.name,
            EventInputProperties{.type = config.type},
            config.access,
        };
    }

    [[nodiscard]] constexpr OutputConfig make_output_config(
        SampleOutputConfig const& config)
    {
        return {
            config.name,
            SampleOutputProperties{.channel_layout = config.channel_layout},
            config.access,
        };
    }

    [[nodiscard]] constexpr OutputConfig make_output_config(
        EventOutputConfig const& config)
    {
        return {
            config.name,
            EventOutputProperties{
                .type = config.type,
                .max_events_per_sample = config.max_events_per_sample,
            },
            config.access,
        };
    }

    [[nodiscard]] constexpr bool is_sample(InputConfig const& config)
    {
        return std::holds_alternative<SampleInputProperties>(config.kind);
    }

    [[nodiscard]] constexpr bool is_sample(OutputConfig const& config)
    {
        return std::holds_alternative<SampleOutputProperties>(config.kind);
    }

    template<typename Ports>
    constexpr size_t count_sample_ports(Ports const& ports)
    {
        size_t result = 0;
        for (auto const& port : ports) {
            result += is_sample(port);
        }
        return result;
    }

    template<typename Ports>
    constexpr size_t count_event_ports(Ports const& ports)
    {
        return ports.size() - count_sample_ports(ports);
    }

    [[nodiscard]] constexpr SampleInputProperties const& sample_properties(
        InputConfig const& config)
    {
        return std::get<SampleInputProperties>(config.kind);
    }

    [[nodiscard]] constexpr SampleOutputProperties const& sample_properties(
        OutputConfig const& config)
    {
        return std::get<SampleOutputProperties>(config.kind);
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
