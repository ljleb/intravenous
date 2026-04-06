#pragma once

#include "compat.h"
#include "sample.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <concepts>
#include <cstdint>
#include <array>
#include <functional>
#include <tuple>
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
    };

    struct TriggerEvent {};

    struct BoundaryEvent {
        bool is_begin = false;
    };

    enum class EventTypeId : unsigned int {
        midi,
        trigger,
        boundary,
        count,
    };

    using EventTime = size_t;

    using Event = std::variant<MidiEvent, TriggerEvent, BoundaryEvent>;

    struct TimedEvent {
        EventTime time = 0;
        Event value {};
    };

    enum class EventConversionStepId : std::uint8_t {
        midi_to_trigger,
        midi_to_boundary,
        trigger_to_boundary,
        trigger_to_midi,
        boundary_to_trigger,
        boundary_to_midi,
    };

    struct EventConversionPlan {
        EventTypeId source_type {};
        EventTypeId target_type {};
        std::vector<EventConversionStepId> steps;

        bool operator==(EventConversionPlan const&) const = default;
    };

    class EventConversionRegistry {
        struct Score {
            int loss = 0;
            int assumptions = 0;
            int hops = 0;

            auto tie() const
            {
                return std::tie(loss, assumptions, hops);
            }

            bool operator<(Score const& other) const
            {
                return tie() < other.tie();
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
            return {{
                { EventTypeId::midi, EventTypeId::trigger, EventConversionStepId::midi_to_trigger, { 1, 0, 1 } },
                { EventTypeId::midi, EventTypeId::boundary, EventConversionStepId::midi_to_boundary, { 1, 0, 1 } },
                { EventTypeId::trigger, EventTypeId::boundary, EventConversionStepId::trigger_to_boundary, { 1, 1, 1 } },
                { EventTypeId::trigger, EventTypeId::midi, EventConversionStepId::trigger_to_midi, { 1, 1, 1 } },
                { EventTypeId::boundary, EventTypeId::trigger, EventConversionStepId::boundary_to_trigger, { 1, 0, 1 } },
                { EventTypeId::boundary, EventTypeId::midi, EventConversionStepId::boundary_to_midi, { 1, 1, 1 } },
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

        static bool is_note_on(MidiEvent const& midi)
        {
            if (midi.size < 3) return false;
            std::uint8_t const status = midi.bytes[0] & 0xF0;
            return status == 0x90 && midi.bytes[2] != 0;
        }

        static bool is_note_off(MidiEvent const& midi)
        {
            if (midi.size < 3) return false;
            std::uint8_t const status = midi.bytes[0] & 0xF0;
            return status == 0x80 || (status == 0x90 && midi.bytes[2] == 0);
        }

        static MidiEvent default_note_on()
        {
            return MidiEvent { .bytes = { 0x90, 60, 127 }, .size = 3 };
        }

        static MidiEvent default_note_off()
        {
            return MidiEvent { .bytes = { 0x80, 60, 0 }, .size = 3 };
        }

        template<typename Emit>
        static void apply_step(EventConversionStepId step, TimedEvent const& event, Emit&& emit)
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
            case EventConversionStepId::trigger_to_boundary:
                if (std::holds_alternative<TriggerEvent>(event.value)) {
                    emit(TimedEvent { .time = event.time, .value = BoundaryEvent { .is_begin = true } });
                    emit(TimedEvent { .time = event.time + 1, .value = BoundaryEvent { .is_begin = false } });
                }
                break;
            case EventConversionStepId::trigger_to_midi:
                if (std::holds_alternative<TriggerEvent>(event.value)) {
                    emit(TimedEvent { .time = event.time, .value = default_note_on() });
                    emit(TimedEvent { .time = event.time + 1, .value = default_note_off() });
                }
                break;
            case EventConversionStepId::boundary_to_trigger:
                if (auto boundary = std::get_if<BoundaryEvent>(&event.value); boundary && boundary->is_begin) {
                    emit(TimedEvent { .time = event.time, .value = TriggerEvent {} });
                }
                break;
            case EventConversionStepId::boundary_to_midi:
                if (auto boundary = std::get_if<BoundaryEvent>(&event.value)) {
                    emit(TimedEvent {
                        .time = event.time,
                        .value = boundary->is_begin ? Event(default_note_on()) : Event(default_note_off())
                    });
                }
                break;
            }
        }

        template<typename Emit>
        static void apply_steps_recursive(
            EventConversionPlan const& plan,
            size_t step_index,
            TimedEvent const& event,
            Emit&& emit
        )
        {
            if (step_index >= plan.steps.size()) {
                emit(event);
                return;
            }

            apply_step(plan.steps[step_index], event, [&](TimedEvent const& converted) {
                apply_steps_recursive(plan, step_index + 1, converted, emit);
            });
        }

    public:
        static EventConversionRegistry const& instance()
        {
            static EventConversionRegistry registry;
            return registry;
        }

        EventConversionPlan plan(EventTypeId source, EventTypeId target) const
        {
            if (!is_valid_type(source) || !is_valid_type(target)) {
                throw std::logic_error("invalid event type");
            }

            if (source == target) {
                return EventConversionPlan {
                    .source_type = source,
                    .target_type = target,
                    .steps = {},
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
                if (current == type_count()) {
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

            std::vector<EventConversionStepId> steps;
            for (EventTypeId current = target; current != source; current = previous_type[type_index(current)]) {
                steps.push_back(previous_step[type_index(current)]);
            }
            std::reverse(steps.begin(), steps.end());

            return EventConversionPlan {
                .source_type = source,
                .target_type = target,
                .steps = std::move(steps),
            };
        }

        template<typename Emit>
        void convert(EventConversionPlan const& plan, TimedEvent const& event, Emit&& emit) const
        {
            apply_steps_recursive(plan, 0, event, std::forward<Emit>(emit));
        }
    };

    class EventBlockView;

    class EventStreamStorage {
        struct EventRecord {
            Event value {};
        };

        struct EventEntry {
            size_t event_offset = 0;
            size_t time = 0;
        };

        struct Stream {
            EventTypeId type {};
            size_t ref_begin = 0;
            size_t ref_size = 0;
            size_t ref_capacity = 0;
        };

        std::vector<EventRecord> _events;
        std::vector<EventEntry> _entries;
        std::vector<size_t> _refs;
        std::vector<Stream> _streams;
        mutable std::vector<size_t> _view_refs;
        void ensure_stream_capacity(size_t stream_id, size_t required)
        {
            auto& stream = _streams[stream_id];
            if (required <= stream.ref_capacity) {
                return;
            }

            size_t const new_capacity = std::max<size_t>(4, next_power_of_2(required));
            size_t const new_begin = _refs.size();
            _refs.resize(new_begin + new_capacity);

            if (stream.ref_size != 0) {
                std::copy_n(_refs.data() + stream.ref_begin, stream.ref_size, _refs.data() + new_begin);
            }

            stream.ref_begin = new_begin;
            stream.ref_capacity = new_capacity;
        }

        size_t append_event_record(Event event)
        {
            _events.push_back(EventRecord { .value = std::move(event) });
            return _events.size() - 1;
        }

        size_t append_event_entry(size_t event_offset, size_t time)
        {
            _entries.push_back(EventEntry {
                .event_offset = event_offset,
                .time = time,
            });
            return _entries.size() - 1;
        }

        void append_ref(size_t stream_id, size_t ref)
        {
            auto& stream = _streams[stream_id];
            ensure_stream_capacity(stream_id, stream.ref_size + 1);
            _refs[stream.ref_begin + stream.ref_size] = ref;
            ++stream.ref_size;
        }

        TimedEvent absolute_event(size_t ref) const
        {
            auto const& entry = _entries[ref];
            return TimedEvent {
                .time = entry.time,
                .value = _events[entry.event_offset].value,
            };
        }

        friend class EventBlockView;

    public:
        size_t allocate(EventTypeId type, size_t initial_capacity = 4)
        {
            _streams.push_back(Stream { .type = type });
            ensure_stream_capacity(_streams.size() - 1, initial_capacity);
            return _streams.size() - 1;
        }

        EventTypeId stream_type(size_t stream_id) const
        {
            return _streams[stream_id].type;
        }

        void append(size_t stream_id, TimedEvent event)
        {
            size_t const event_offset = append_event_record(std::move(event.value));
            size_t const ref = append_event_entry(event_offset, event.time);
            append_ref(stream_id, ref);
        }

        void append_existing(size_t stream_id, size_t ref)
        {
            append_ref(stream_id, ref);
        }

        EventBlockView block(size_t stream_id, size_t block_index, size_t block_size) const;
    };

    class EventBlockView {
        EventStreamStorage const* _storage = nullptr;
        size_t _stream_id = 0;
        std::span<size_t const> _refs {};
        size_t _block_index = 0;
        EventTypeId _type {};

    public:
        EventBlockView() = default;

        EventBlockView(
            EventStreamStorage const& storage,
            size_t stream_id,
            std::span<size_t const> refs,
            size_t block_index,
            EventTypeId type
        ) :
            _storage(&storage),
            _stream_id(stream_id),
            _refs(refs),
            _block_index(block_index),
            _type(type)
        {}

        size_t size() const
        {
            return _refs.size();
        }

        EventTypeId type() const
        {
            return _type;
        }

        size_t block_index() const
        {
            return _block_index;
        }

        size_t stream_id() const
        {
            return _stream_id;
        }

        EventStreamStorage const* storage() const
        {
            return _storage;
        }

        std::span<size_t const> refs() const
        {
            return _refs;
        }

        TimedEvent operator[](size_t index) const
        {
            TimedEvent event = _storage->absolute_event(_refs[index]);
            event.time -= _block_index;
            return event;
        }

        template<typename Fn>
        void for_each(Fn&& fn) const
        {
            for (size_t i = 0; i < _refs.size(); ++i) {
                fn((*this)[i], _refs[i]);
            }
        }
    };

    inline EventBlockView EventStreamStorage::block(size_t stream_id, size_t block_index, size_t block_size) const
    {
        auto const& stream = _streams[stream_id];
        _view_refs.clear();
        _view_refs.reserve(stream.ref_size);

        size_t const block_end = block_index + block_size;
        for (size_t i = 0; i < stream.ref_size; ++i) {
            size_t const ref = _refs[stream.ref_begin + i];
            size_t const time = _entries[ref].time;
            if (time >= block_index && time < block_end) {
                _view_refs.push_back(ref);
            }
        }

        return EventBlockView(*this, stream_id, std::span<size_t const>(_view_refs), block_index, stream.type);
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

    template<typename A>
    struct BlockView {
        std::span<A> first {};
        std::span<A> second {};

        template<typename B = A>
            requires (!std::is_const_v<B>)
        constexpr operator BlockView<std::add_const_t<B>>() const
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
        IV_FORCEINLINE constexpr void copy_to(BlockView<Dst> dst) const
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

    IV_FORCEINLINE constexpr BlockView<Sample> make_block_view(
        std::span<Sample> buffer,
        size_t start,
        size_t count
    )
    {
        if (count == 0) {
            return {};
        }

        size_t const first_size = std::min(count, buffer.size() - start);
        return {
            buffer.subspan(start, first_size),
            buffer.subspan(0, count - first_size),
        };
    }

    IV_FORCEINLINE constexpr BlockView<Sample const> make_block_view(
        std::span<Sample const> buffer,
        size_t start,
        size_t count
    )
    {
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

        constexpr explicit SharedPortData(
            std::span<Sample> buffer = {},
            size_t latency = 0
        ) :
            buffer(buffer),
            latency(latency)
        {}
    };

    class InputPort {
        SharedPortData& _shared_data;
        size_t _history;
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
            SharedPortData& shared_data,
            size_t history
        ) :
            _shared_data(shared_data),
            _history(history)
        {
            IV_ASSERT(is_power_of_2(_shared_data.buffer.size()), "buffer size should be a power of 2");
        }

        IV_FORCEINLINE constexpr Sample get(size_t offset = 0) const
        {
            if (offset > _history) return 0.0f;
            size_t const idx = (current_read_position() + buffer_size() - offset) & (buffer_size() - 1);
            return _shared_data.buffer[idx];
        }

        IV_FORCEINLINE constexpr BlockView<Sample> get_block(size_t block_size, size_t sample_offset = 0) const
        {
            if (sample_offset > block_size) {
                return {};
            }

            size_t const start = (current_read_position() + sample_offset) & (buffer_size() - 1);
            return make_block_view(_shared_data.buffer, start, block_size - sample_offset);
        }

        IV_FORCEINLINE constexpr size_t latency() const
        {
            return _shared_data.latency;
        }

        IV_FORCEINLINE constexpr size_t buffer_size() const
        {
            return _shared_data.buffer.size();
        }
    };

    class OutputPort {
        SharedPortData& _shared_data;
        size_t _history;
        size_t _position = 0;

    public:
        explicit OutputPort(SharedPortData& shared_data, size_t history) :
            _shared_data(shared_data),
            _history(history)
        {
            IV_ASSERT(is_power_of_2(_shared_data.buffer.size()), "buffer size should be a power of 2");
        }

        IV_FORCEINLINE constexpr Sample get(size_t offset = 0) const
        {
            if (offset > _shared_data.latency + _history) return 0.0f;
            size_t const idx = (
                _position + _shared_data.latency + buffer_size() - 1 - offset
            ) & (buffer_size() - 1);
            return _shared_data.buffer[idx];
        }

        IV_FORCEINLINE constexpr BlockView<Sample> get_block(size_t block_size, size_t sample_offset = 0) const
        {
            size_t const available = _shared_data.latency + _history + 1;
            size_t const count = std::min(block_size, available - sample_offset);
            size_t const start = (
                _position + _shared_data.latency + buffer_size() - (sample_offset + count)
            ) & (buffer_size() - 1);

            return make_block_view(_shared_data.buffer, start, count);
        }

        IV_FORCEINLINE constexpr void push(Sample value)
        {
            size_t const idx = (_position + _shared_data.latency) & (buffer_size() - 1);
            _shared_data.buffer[idx] = value;
            _position = (_position + 1) & (buffer_size() - 1);
        }

        IV_FORCEINLINE constexpr void push_block(std::span<Sample const> samples)
        {
            size_t const start = (_position + _shared_data.latency) & (buffer_size() - 1);
            size_t const n = samples.size();

            size_t const first_count = std::min(n, _shared_data.buffer.size() - start);
            std::copy_n(samples.data(), first_count, _shared_data.buffer.data() + start);

            size_t const second_count = n - first_count;
            std::copy_n(samples.data() + first_count, second_count, _shared_data.buffer.data());

            _position = (_position + n) & (buffer_size() - 1);
        }

        IV_FORCEINLINE constexpr void push_block(BlockView<Sample const> samples)
        {
            size_t const start = (_position + _shared_data.latency) & (buffer_size() - 1);
            samples.copy_to(make_block_view(_shared_data.buffer, start, samples.size()));
            _position = (_position + samples.size()) & (buffer_size() - 1);
        }

        IV_FORCEINLINE constexpr void accumulate_block(std::span<Sample const> samples)
        {
            size_t const start = (
                _position + _shared_data.latency + buffer_size() - samples.size()
            ) & (buffer_size() - 1);
            auto dst = make_block_view(_shared_data.buffer, start, samples.size());
            auto src = make_block_view(samples, 0, samples.size());
            for (size_t i = 0; i < samples.size(); ++i) {
                dst[i] += src[i];
            }
        }

        IV_FORCEINLINE constexpr void accumulate_block(BlockView<Sample const> samples)
        {
            size_t const start = (
                _position + _shared_data.latency + buffer_size() - samples.size()
            ) & (buffer_size() - 1);
            auto dst = make_block_view(_shared_data.buffer, start, samples.size());
            for (size_t i = 0; i < samples.size(); ++i) {
                dst[i] += samples[i];
            }
        }

        IV_FORCEINLINE constexpr void push_silence(size_t block_size)
        {
            size_t const start = (_position + _shared_data.latency) & (buffer_size() - 1);
            auto block = make_block_view(_shared_data.buffer, start, block_size);
            std::fill(block.first.begin(), block.first.end(), 0.0f);
            std::fill(block.second.begin(), block.second.end(), 0.0f);
            _position = (_position + block_size) & (buffer_size() - 1);
        }

        IV_FORCEINLINE constexpr void update(Sample value, size_t offset = 0)
        {
            if (offset > _shared_data.latency) return;
            size_t const idx = (_position + _shared_data.latency + buffer_size() - offset) & (buffer_size() - 1);
            _shared_data.buffer[idx] = value;
        }

        IV_FORCEINLINE constexpr size_t position() const
        {
            return _position;
        }

        IV_FORCEINLINE constexpr BlockView<Sample const> current_block(size_t block_size) const
        {
            size_t const start = (_position + buffer_size() - block_size) & (buffer_size() - 1);
            return make_block_view(std::span<Sample const>(_shared_data.buffer), start, block_size);
        }

        IV_FORCEINLINE constexpr size_t buffer_size() const
        {
            return _shared_data.buffer.size();
        }
    };

    struct EventSharedPortData {
        size_t stream_id = 0;
        EventTypeId type {};

        constexpr EventSharedPortData(
            size_t stream_id = 0,
            EventTypeId type = {}
        ) :
            stream_id(stream_id),
            type(type)
        {}
    };

    class EventInputPort {
        EventSharedPortData _shared_data;

    public:
        EventInputPort() = default;

        explicit EventInputPort(EventSharedPortData shared_data) :
            _shared_data(shared_data)
        {}

        EventBlockView get_block(EventStreamStorage& storage, size_t block_index, size_t block_size) const
        {
            return storage.block(_shared_data.stream_id, block_index, block_size);
        }

        template<typename Fn>
        void for_each_in_block(EventStreamStorage& storage, size_t block_index, size_t block_size, Fn&& fn) const
        {
            get_block(storage, block_index, block_size).for_each(std::forward<Fn>(fn));
        }

        EventTypeId type() const
        {
            return _shared_data.type;
        }
    };

    class EventOutputPort {
        EventSharedPortData _shared_data;
        EventTypeId _source_type {};
        bool _has_conversion = false;
        EventConversionPlan _conversion {};

    public:
        EventOutputPort() = default;

        explicit EventOutputPort(
            EventSharedPortData shared_data,
            EventTypeId source_type
        ) :
            _shared_data(shared_data),
            _source_type(source_type)
        {}

        explicit EventOutputPort(
            EventSharedPortData shared_data,
            EventTypeId source_type,
            EventConversionPlan const& conversion
        ) :
            _shared_data(shared_data),
            _source_type(source_type),
            _has_conversion(true),
            _conversion(conversion)
        {}

        void push(EventStreamStorage& storage, Event event, size_t sample_offset, size_t block_index, size_t block_size) const
        {
            (void)block_size;
            TimedEvent const timed_event {
                .time = block_index + sample_offset,
                .value = std::move(event)
            };
            if (_has_conversion) {
                EventConversionRegistry::instance().convert(_conversion, timed_event, [&](TimedEvent const& converted) {
                    storage.append(_shared_data.stream_id, converted);
                });
            } else {
                storage.append(_shared_data.stream_id, timed_event);
            }
        }

        void push_block(EventStreamStorage& storage, EventBlockView const& events, size_t block_index, size_t block_size) const
        {
            (void)block_size;
            bool const can_move =
                !_has_conversion &&
                events.storage() == &storage &&
                events.block_index() == block_index &&
                events.type() == _shared_data.type;

            if (can_move) {
                for (size_t ref : events.refs()) {
                    storage.append_existing(_shared_data.stream_id, ref);
                }
                return;
            }

            events.for_each([&](TimedEvent const& event, size_t) {
                push(storage, event.value, event.time, block_index, block_size);
            });
        }

        void append_block(EventStreamStorage& storage, EventBlockView const& events, size_t block_index, size_t block_size) const
        {
            push_block(storage, events, block_index, block_size);
        }

        EventTypeId source_type() const
        {
            return _source_type;
        }

        bool empty_in_block(EventStreamStorage& storage, size_t block_index, size_t block_size) const
        {
            return storage.block(_shared_data.stream_id, block_index, block_size).size() == 0;
        }
    };

    struct InputConfig {
        std::string name {};
        size_t history = 0;
        Sample default_value = 0.0;
    };

    struct OutputConfig {
        std::string name {};
        size_t latency = 0;
        size_t history = 0;
    };

    struct EventInputConfig {
        std::string name {};
        EventTypeId type {};
    };

    struct EventOutputConfig {
        std::string name {};
        EventTypeId type {};
    };

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
