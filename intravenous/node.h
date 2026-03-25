#pragma once
#include "compat.h"
#include <algorithm>
#include <span>
#include <cassert>
#include <cstddef>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <array>
#include <vector>
#include <iostream>


namespace iv {
	struct Sample {
        using storage = float;
        storage value{};

        constexpr Sample() = default;

        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        constexpr Sample(T v) : value(static_cast<float>(v)) {}

        constexpr operator float() const noexcept {
            return value;
        }

        constexpr Sample& operator+=(Sample other) noexcept { value += other.value; return *this; }
        constexpr Sample& operator-=(Sample other) noexcept { value -= other.value; return *this; }
        constexpr Sample& operator*=(Sample other) noexcept { value *= other.value; return *this; }
        constexpr Sample& operator/=(Sample other) noexcept { value /= other.value; return *this; }

        friend constexpr Sample operator+(Sample a, Sample b) noexcept { return a.value + b.value; }
        friend constexpr Sample operator-(Sample a, Sample b) noexcept { return a.value - b.value; }
        friend constexpr Sample operator*(Sample a, Sample b) noexcept { return a.value * b.value; }
        friend constexpr Sample operator/(Sample a, Sample b) noexcept { return a.value / b.value; }

        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator+(Sample a, T b) noexcept {
            return a.value + static_cast<float>(b);
        }
        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator-(Sample a, T b) noexcept {
            return a.value - static_cast<float>(b);
        }
        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator*(Sample a, T b) noexcept {
            return a.value * static_cast<float>(b);
        }
        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator/(Sample a, T b) noexcept {
            return a.value / static_cast<float>(b);
        }

        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator+(T a, Sample b) noexcept {
            return static_cast<float>(a) + b.value;
        }
        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator-(T a, Sample b) noexcept {
            return static_cast<float>(a) - b.value;
        }
        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator*(T a, Sample b) noexcept {
            return static_cast<float>(a) * b.value;
        }
        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator/(T a, Sample b) noexcept {
            return static_cast<float>(a) / b.value;
        }

        constexpr Sample& operator++() noexcept {
            ++value;
            return *this;
        }
        constexpr Sample operator++(int) noexcept {
            Sample tmp = *this;
            ++(*this);
            return tmp;
        }

        constexpr Sample& operator--() noexcept {
            --value;
            return *this;
        }
        constexpr Sample operator--(int) noexcept {
            Sample tmp = *this;
            --(*this);
            return tmp;
        }
    };

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

    IV_FORCEINLINE size_t prev_power_of_2(size_t n)
    {
        if (n == 0) {
            return 0;
        }
        size_t power = 1;
        while ((power << 1) != 0 && (power << 1) <= n) {
            power <<= 1;
        }
        return power;
    }

    struct SampleBlockView {
        std::span<Sample const> first {};
        std::span<Sample const> second {};

        constexpr size_t size() const
        {
            return first.size() + second.size();
        }

        constexpr bool empty() const
        {
            return size() == 0;
        }

        constexpr Sample operator[](size_t index) const
        {
            return index < first.size()
                ? first[index]
                : second[index - first.size()];
        }

        struct iterator {
            using value_type = Sample;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::forward_iterator_tag;
            using iterator_concept = std::forward_iterator_tag;
            using reference = Sample;
            using pointer = void;

            Sample const* first_ptr = nullptr;
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
    };

    IV_FORCEINLINE constexpr SampleBlockView make_block_view(
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
            assert(is_power_of_2(_shared_data.buffer.size()) && "buffer size should be a power of 2");
        }

        IV_FORCEINLINE constexpr Sample get(size_t offset = 0) const
        {
            if (offset > _history) return 0.0f;
            size_t const idx = (current_read_position() + buffer_size() - offset) & (buffer_size() - 1);
            return _shared_data.buffer[idx];
        }

        IV_FORCEINLINE constexpr SampleBlockView get_block(size_t block_size, size_t sample_offset = 0) const
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
            assert(is_power_of_2(_shared_data.buffer.size()) && "buffer size should be a power of 2");
        }

        IV_FORCEINLINE constexpr Sample get(size_t offset = 0) const
        {
            if (offset > _shared_data.latency + _history) return 0.0f;
            size_t const idx = (
                _position + _shared_data.latency + buffer_size() - 1 - offset
            ) & (buffer_size() - 1);
            return _shared_data.buffer[idx];
        }

        IV_FORCEINLINE constexpr SampleBlockView get_block(size_t block_size, size_t sample_offset = 0) const
        {
            if (sample_offset > block_size) {
                return {};
            }

            size_t const start = (
                _position + _shared_data.latency + buffer_size() - block_size + sample_offset
            ) & (buffer_size() - 1);
            return make_block_view(_shared_data.buffer, start, block_size - sample_offset);
        }

        IV_FORCEINLINE constexpr void push(Sample value)
        {
            size_t const idx = (_position + _shared_data.latency) & (buffer_size() - 1);
            _shared_data.buffer[idx] = value;
            _position = (_position + 1) & (buffer_size() - 1);
        }

        IV_FORCEINLINE constexpr void push_block(std::span<Sample const> samples)
        {
            for (Sample sample : samples) {
                push(sample);
            }
        }

        IV_FORCEINLINE constexpr void push_block(SampleBlockView samples)
        {
            push_block(samples.first);
            push_block(samples.second);
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

        IV_FORCEINLINE constexpr size_t buffer_size() const
        {
            return _shared_data.buffer.size();
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

    struct NodeState {
        std::span<InputPort> inputs;
        std::span<OutputPort> outputs;
        std::span<std::byte> buffer;

        template<typename State>
        State& get_state() const {
            void* ptr = buffer.data();
            size_t space = buffer.size();
            return *reinterpret_cast<State*>(std::align(alignof(State), sizeof(State), ptr, space));
        }
    };

    enum struct MidiMessageType {
        NOTE_ON,
        NOTE_OFF,
        PITCH_WHEEL,
    };

    struct MidiMessage {
        MidiMessageType type;
        union {
            struct {
                uint8_t note_number;
                uint8_t channel;
                uint8_t amplitude;
            } note_on;
            struct {
                uint8_t note_number;
                uint8_t channel;
            } note_off;
            struct {
                uint16_t pitch_value;
                uint8_t channel;
            } pitch_wheel;
        };
    };

    struct TickState : public NodeState {
        std::span<MidiMessage const> midi;
        size_t index;

        TickState(NodeState base, std::span<MidiMessage const> midi, size_t index) :
            NodeState(base), midi(midi), index(index)
        {}
    };

    struct BlockTickState : public NodeState {
        std::span<MidiMessage const> midi;
        size_t index;
        size_t block_size;

        BlockTickState(
            NodeState base,
            std::span<MidiMessage const> midi,
            size_t index,
            size_t block_size
        ) :
            NodeState(base),
            midi(midi),
            index(index),
            block_size(block_size)
        {}
    };

    struct InitBufferContext {
        enum class PassMode {
            counting,
            initializing,
        };

        struct InitBufferRecord {
            std::string id;
            void const* type_tag = nullptr;
            size_t count = 0;
            bool declared = false;
            bool registered = false;
            bool fulfilled = false;
            std::shared_ptr<void> storage;
        };

        struct TickBufferRecord {
            std::string id;
            void const* type_tag = nullptr;
            size_t count = 0;
            ptrdiff_t offset = std::numeric_limits<ptrdiff_t>::min();
            bool registered = false;
            bool used = false;
        };

        PassMode mode = PassMode::counting;
        std::span<std::byte> runtime_buffer;
        size_t max_block_size = 1;
        std::unordered_map<std::string, InitBufferRecord> init_buffers;
        std::unordered_map<std::string, TickBufferRecord> tick_buffers;

    // public:
        InitBufferContext() = default;

        InitBufferContext(PassMode mode_, std::span<std::byte> runtime_buffer_ = {}) :
            mode(mode_),
            runtime_buffer(runtime_buffer_)
        {}

        InitBufferContext make_initializing_context(std::span<std::byte> new_runtime_buffer) const
        {
            InitBufferContext replay(PassMode::initializing, new_runtime_buffer);
            replay.max_block_size = max_block_size;
            replay.init_buffers = init_buffers;
            replay.tick_buffers = tick_buffers;
            for (auto& [_, record] : replay.init_buffers) {
                record.registered = false;
                record.fulfilled = false;
            }
            for (auto& [_, record] : replay.tick_buffers) {
                record.registered = false;
            }
            return replay;
        }

        void validate_after_counting() const
        {
            for (auto const& [_, record] : tick_buffers) {
                if (record.used && !record.registered) {
                    throw std::logic_error("tick buffer '" + record.id + "' was used but never registered during the first pass");
                }
            }
        }

        void validate_after_initialization() const
        {
            for (auto const& [_, record] : init_buffers) {
                if (record.declared && !record.fulfilled) {
                    throw std::logic_error("init buffer '" + record.id + "' was not registered again during the second pass");
                }
            }
            for (auto const& [_, record] : tick_buffers) {
                if (was_tick_buffer_declared(record) && !record.registered) {
                    throw std::logic_error("tick buffer '" + record.id + "' was not registered again during the second pass");
                }
            }
        }

        // size_t max_block_size() const {
        //     return _max_block_size;
        // }

        bool has_init_buffer(std::string const& id) const
        {
            return init_buffers.contains(id);
        }

        bool has_tick_buffer(std::string const& id) const
        {
            return tick_buffers.contains(id);
        }

        template<typename T>
        std::span<T> register_init_buffer(std::string const& id, size_t count)
        {
            auto& record = init_buffers[id];
            if (record.id.empty()) {
                record.id = id;
            }
            if (record.count != 0 && record.count != count) {
                throw std::logic_error("init buffer '" + record.id + "' changed element count between registrations");
            }
            if (mode == PassMode::counting) {
                if (record.registered) {
                    throw std::logic_error("init buffer '" + record.id + "' was registered more than once");
                }
                if (!record.storage) {
                    std::shared_ptr<T[]> storage(new T[count](), std::default_delete<T[]>());
                    record.storage = std::shared_ptr<void>(storage, storage.get());
                }
                record.type_tag = type_token<T>();
                record.count = count;
                record.declared = true;
                record.registered = true;
                record.fulfilled = false;
                return { static_cast<T*>(record.storage.get()), record.count };
            }

            if (!record.declared) {
                throw std::logic_error("init buffer '" + record.id + "' was not registered during the first pass");
            }
            if (record.fulfilled) {
                throw std::logic_error("init buffer '" + record.id + "' was registered more than once on the second pass");
            }
            record.registered = true;
            record.fulfilled = true;
            return { static_cast<T*>(record.storage.get()), record.count };
        }

        template<typename T>
        std::span<T> use_init_buffer(std::string const& id)
        {
            auto it = init_buffers.find(id);
            if (it == init_buffers.end()) {
                throw std::logic_error("init buffer '" + id + "' was used before registration");
            }
            auto& record = it->second;
            if (!record.registered) {
                throw std::logic_error("init buffer '" + record.id + "' was used before registration in the current pass");
            }
            return { static_cast<T*>(record.storage.get()), record.count };
        }

        template<typename T>
        void register_tick_buffer(std::string const& id, std::span<T> tick_buffer)
        {
            auto& record = tick_buffers[id];
            if (record.id.empty()) {
                record.id = id;
            }
            validate_tick_buffer_identity<T>(record, tick_buffer.size());
            ptrdiff_t const offset = compute_runtime_offset(tick_buffer.data());

            if (mode == PassMode::counting) {
                if (record.registered) {
                    throw std::logic_error("tick buffer '" + record.id + "' was registered more than once");
                }
                record.type_tag = type_token<T>();
                record.count = tick_buffer.size();
                record.offset = offset;
                record.registered = true;
                return;
            }

            if (!was_tick_buffer_declared(record)) {
                throw std::logic_error("tick buffer '" + record.id + "' was not registered during the first pass");
            }
            if (record.registered) {
                throw std::logic_error("tick buffer '" + record.id + "' was registered more than once on the second pass");
            }
            if (record.offset != offset) {
                throw std::logic_error("tick buffer '" + record.id + "' changed offset between init passes");
            }
            record.registered = true;
        }

        template<typename T>
        std::span<T> use_tick_buffer(std::string const& id)
        {
            auto& record = tick_buffers[id];
            if (record.id.empty()) {
                record.id = id;
            }

            record.used = true;
            if (!record.type_tag) {
                record.type_tag = type_token<T>();
            }

            if (mode == PassMode::counting) {
                return {};
            }
            if (!was_tick_buffer_declared(record)) {
                throw std::logic_error("tick buffer '" + record.id + "' was used before first-pass registration");
            }

            auto* data = reinterpret_cast<T*>(runtime_buffer.data() + record.offset);
            return { data, record.count };
        }

    private:
        template<typename T>
        static void const* type_token()
        {
            static int token = 0;
            return &token;
        }

        ptrdiff_t compute_runtime_offset(void const* data) const
        {
            if (!runtime_buffer.data() || !data) {
                return 0;
            }
            auto const* base = reinterpret_cast<std::byte const*>(runtime_buffer.data());
            auto const* ptr = reinterpret_cast<std::byte const*>(data);
            return ptr - base;
        }

        template<typename T>
        void validate_tick_buffer_identity(TickBufferRecord const& record, size_t count) const
        {
            if (record.count != 0 && record.count != count) {
                throw std::logic_error("tick buffer '" + record.id + "' changed element count between registrations");
            }
        }

        static bool was_tick_buffer_declared(TickBufferRecord const& record)
        {
            return record.offset != unresolved_tick_offset();
        }

        static constexpr ptrdiff_t unresolved_tick_offset()
        {
            return std::numeric_limits<ptrdiff_t>::min();
        }
    };

    namespace details
    {
        template <typename Node>
        concept has_outputs = requires(Node node, std::span<OutputConfig const> outputs)
        {
            outputs = node.outputs();
        };

        template <typename Node>
        concept has_num_outputs = requires(Node node, size_t num_outputs)
        {
            num_outputs = node.num_outputs();
        };

        template <typename Node>
        concept has_inputs = requires(Node node, std::span<InputConfig const> inputs)
        {
            inputs = node.inputs();
        };

        template <typename Node>
        concept has_num_inputs = requires(Node node, size_t num_inputs)
        {
            num_inputs = node.num_inputs();
        };

        template <typename Node, typename Allocator>
        concept has_init_buffer = requires(Node node, Allocator allocator)
        {
            node.init_buffer(allocator);
        };

        template <typename Node, typename Allocator>
        concept has_init_buffer_ctx = requires(Node node, Allocator allocator, InitBufferContext ctx)
        {
            node.init_buffer(allocator, ctx);
        };

        template <typename Node>
        concept has_tick = requires(Node node, TickState state)
        {
            node.tick(state);
        };

        template <typename Node>
        concept has_tick_block = requires(Node node, BlockTickState state)
        {
            node.tick_block(state);
        };

        template <typename Node>
        concept has_internal_latency = requires(Node node, size_t internal_latency)
        {
            internal_latency = node.internal_latency();
        };

        template <typename Node>
        concept has_max_block_size_method = requires(Node node, size_t block_size)
        {
            block_size = node.max_block_size();
        };
    }

    template<typename Node>
    constexpr auto get_outputs(Node const& node)
    {
        if constexpr (details::has_outputs<Node>)
        {
            return node.outputs();
        }
        else
        {
            return std::span<OutputConfig, 0>{};
        }
    }

    template<typename Node>
    constexpr auto get_num_outputs(Node const& node)
    {
        if constexpr (details::has_num_outputs<Node>)
        {
            return node.num_outputs();
        }
        else
        {
            return get_outputs(node).size();
        }
    }

    template<typename Node>
    constexpr auto get_inputs(Node const& node)
    {
        if constexpr (details::has_inputs<Node>)
        {
            return node.inputs();
        }
        else
        {
            return std::span<InputConfig, 0>{};
        }
    }

    template<typename Node>
    constexpr auto get_num_inputs(Node const& node)
    {
        if constexpr (details::has_num_inputs<Node>)
        {
            return node.num_inputs();
        }
        else
        {
            return get_inputs(node).size();
        }
    }

    template<typename Node, typename Allocator>
    constexpr std::span<std::byte> do_init_buffer(Node const& node, Allocator& allocator, InitBufferContext& ctx)
    {
        if constexpr (details::has_init_buffer_ctx<Node, Allocator> || details::has_init_buffer<Node, Allocator>)
        {
            std::span<std::byte> memory_before = allocator.get_buffer();
            if constexpr (details::has_init_buffer_ctx<Node, Allocator>)
            {
                node.init_buffer(allocator, ctx);
            }
            else
            {
                node.init_buffer(allocator);
            }
            std::span<std::byte> memory_after = allocator.get_buffer();

#ifndef NDEBUG
            std::byte* before_end = memory_before.data() + memory_before.size();
            std::byte* after_end = memory_after.data() + memory_after.size();
            assert(before_end == after_end);
#endif
            return { memory_before.data(), memory_before.size() - memory_after.size() };
        }
        else
        {
            return { allocator.get_buffer().data(), 0 };
        }
    }

    template<typename Node>
    constexpr size_t get_internal_latency(Node const& node)
    {
        if constexpr (details::has_internal_latency<Node>)
        {
            return node.internal_latency();
        }
        else
        {
            return 0;
        }
    }

    template<typename Node>
    constexpr size_t get_max_block_size(Node const& node)
    {
        if constexpr (details::has_max_block_size_method<Node>)
        {
            return node.max_block_size();
        }
        else
        {
            return MAX_BLOCK_SIZE;
        }
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

    template<typename Node>
    void do_tick_block(Node& node, BlockTickState const& state);

    template<typename Node>
    void do_tick(Node& node, TickState const& state)
    {
        if constexpr (details::has_tick<Node>)
        {
            node.tick(state);
            advance_inputs(state.inputs, 1);
        }
        else if constexpr (details::has_tick_block<Node>)
        {
            do_tick_block(node, {
                static_cast<NodeState const&>(state),
                state.midi,
                state.index,
                1,
            });
        }
        else
        {
            static_assert(details::has_tick<Node> || details::has_tick_block<Node>, "node must implement tick() or tick_block()");
        }
    }

    template<typename Node>
    void do_tick_block(Node& node, BlockTickState const& state)
    {
        if (state.block_size == 0) {
            return;
        }
        validate_block_size(state.block_size);

        if constexpr (details::has_tick_block<Node>)
        {
            node.tick_block(state);
            advance_inputs(state.inputs, state.block_size);
        }
        else
        {
            for (size_t sample = 0; sample < state.block_size; ++sample) {
                do_tick(node, {
                    static_cast<NodeState const&>(state),
                    state.midi,
                    state.index + sample,
                });
            }
        }
    }

    template <typename Node>
    void info(Node&& node) {
        std::cout << "internal latency: " << get_internal_latency(node) << "\n";

        auto const block_size = get_max_block_size(node);
        std::cout << "max block size: " << ((block_size == MAX_BLOCK_SIZE) ? std::string("unbounded") : std::to_string(block_size)) << "\n";
        std::cout << "params:\n";

        for (auto const& in : get_inputs(node)) {
            std::cout << "    in: " << in.name << " (" << in.default_value << ")\n";
        }
        for (auto const& out : get_outputs(node)) {
            std::cout << "    out: " << out.name << "\n";
        }
    }
}
