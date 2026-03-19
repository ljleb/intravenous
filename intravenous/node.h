#pragma once
#include "compat.h"
#include <span>
#include <cassert>
#include <cstddef>
#include <memory>
#include <string_view>


namespace iv {
	using Sample = float;

    IV_FORCEINLINE bool is_power_of_2(size_t n) noexcept
    {
        return n && !(n & (n - 1));
    }

    struct SharedPortData {
        std::span<Sample> buffer;
        size_t position;
        size_t latency;

        constexpr explicit SharedPortData(
            std::span<Sample> buffer = {},
            size_t latency = 0
        ) noexcept :
            buffer(buffer),
            position(0),
            latency(latency)
        {}
    };

    class InputPort {
        SharedPortData& _shared_data;
        size_t _history;

    public:
        explicit InputPort(
            SharedPortData& shared_data,
            size_t history
        ) noexcept :
            _shared_data(shared_data),
            _history(history)
        {
            assert(is_power_of_2(_shared_data.buffer.size()) && "buffer size should be a power of 2");
        }

        IV_FORCEINLINE constexpr Sample get(size_t offset = 0) const noexcept
        {
            if (offset > _history) return 0.0;
            size_t idx = (_shared_data.position + _shared_data.buffer.size() - offset) & (_shared_data.buffer.size() - 1);
            return _shared_data.buffer[idx];
        }

        IV_FORCEINLINE constexpr void push(Sample value) const noexcept
        {
            _shared_data.position = (_shared_data.position + 1) & (_shared_data.buffer.size() - 1);
            update(value);
        }

        IV_FORCEINLINE constexpr void update(Sample value, size_t offset = 0) const noexcept
        {
            if (offset > _shared_data.latency) return;
            size_t idx = (_shared_data.position + _shared_data.latency - offset) & (_shared_data.buffer.size() - 1);
            _shared_data.buffer[idx] = value;
        }

        IV_FORCEINLINE constexpr size_t latency() const noexcept
        {
            return _shared_data.latency;
        }

        IV_FORCEINLINE constexpr size_t buffer_size() const noexcept
        {
            return _shared_data.buffer.size();
        }
    };

    class OutputPort {
        SharedPortData& _shared_data;
        size_t _history;

    public:
        explicit OutputPort(SharedPortData& shared_data, size_t history) noexcept :
            _shared_data(shared_data),
            _history(history)
        {
            assert(is_power_of_2(_shared_data.buffer.size()) && "buffer size should be a power of 2");
        }

        IV_FORCEINLINE constexpr Sample get(size_t offset = 0) const noexcept
        {
            if (offset > _shared_data.latency + _history) return 0.0;
            size_t idx = (_shared_data.position + _shared_data.latency - offset) & (_shared_data.buffer.size() - 1);
            return _shared_data.buffer[idx];
        }

        IV_FORCEINLINE constexpr void push(Sample value) const noexcept
        {
            _shared_data.position = (_shared_data.position + 1) & (_shared_data.buffer.size() - 1);
            update(value);
        }

        IV_FORCEINLINE constexpr void update(Sample value, size_t offset = 0) const noexcept
        {
            if (offset > _shared_data.latency) return;
            size_t idx = (_shared_data.position + _shared_data.latency - offset) & (_shared_data.buffer.size() - 1);
            _shared_data.buffer[idx] = value;
        }
    };

    struct InputConfig {
        std::string_view name {};
        size_t history = 0;
        Sample default_value = 0.0;
    };

    struct OutputConfig {
        std::string_view name {};
        size_t latency = 0;
        size_t history = 0;
    };

    struct NodeState {
        std::span<InputPort> inputs;
        std::span<OutputPort> outputs;
        std::span<std::byte> buffer;
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

        TickState(NodeState base, std::span<MidiMessage const> midi, size_t index) noexcept :
            NodeState(base), midi(midi), index(index)
        {}
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

        template <typename Node>
        concept has_internal_latency = requires(Node node, size_t internal_latency)
        {
            internal_latency = node.internal_latency();
        };
    }

    template<typename Node>
    constexpr auto get_outputs(Node const& node) noexcept
    {
        if constexpr (details::has_outputs<Node>)
        {
            return node.outputs();
        }
        else
        {
            return std::array<OutputConfig, 0>{};
        }
    }

    template<typename Node>
    constexpr auto get_num_outputs(Node const& node) noexcept
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
    constexpr auto get_inputs(Node const& node) noexcept
    {
        if constexpr (details::has_inputs<Node>)
        {
            return node.inputs();
        }
        else
        {
            return std::array<InputConfig, 0>{};
        }
    }

    template<typename Node>
    constexpr auto get_num_inputs(Node const& node) noexcept
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
    constexpr std::span<std::byte> do_init_buffer(Node const& node, Allocator& allocator) noexcept
    {
        if constexpr (details::has_init_buffer<Node, Allocator>)
        {
            std::span<std::byte> memory_before = allocator.get_buffer();
            node.init_buffer(allocator);
            std::span<std::byte> memory_after = allocator.get_buffer();

            std::byte* before_end = memory_before.data() + memory_before.size();
            std::byte* after_end = memory_after.data() + memory_after.size();
            assert(before_end == after_end);
            return { memory_before.data(), memory_before.size() - memory_after.size() };
        }
        else
        {
            return { allocator.get_buffer().data(), 0 };
        }
    }

    template<typename Node>
    constexpr size_t get_internal_latency(Node const& node) noexcept
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

    template<typename State>
    static State& get_state(std::span<std::byte> buffer) {
        void* ptr = buffer.data();
        size_t space = buffer.size();
        return *reinterpret_cast<State*>(std::align(alignof(State), sizeof(State), ptr, space));
    }
}
