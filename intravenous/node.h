#pragma once
#include "compat.h"
#include <span>
#include <cassert>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <array>
#include <vector>


namespace iv {
	using Sample = float;

    IV_FORCEINLINE bool is_power_of_2(size_t n)
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
        ) :
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
        ) :
            _shared_data(shared_data),
            _history(history)
        {
            assert(is_power_of_2(_shared_data.buffer.size()) && "buffer size should be a power of 2");
        }

        IV_FORCEINLINE constexpr Sample get(size_t offset = 0) const
        {
            if (offset > _history) return 0.0;
            size_t idx = (_shared_data.position + _shared_data.buffer.size() - offset) & (_shared_data.buffer.size() - 1);
            return _shared_data.buffer[idx];
        }

        IV_FORCEINLINE constexpr void push(Sample value) const
        {
            _shared_data.position = (_shared_data.position + 1) & (_shared_data.buffer.size() - 1);
            update(value);
        }

        IV_FORCEINLINE constexpr void update(Sample value, size_t offset = 0) const
        {
            if (offset > _shared_data.latency) return;
            size_t idx = (_shared_data.position + _shared_data.latency - offset) & (_shared_data.buffer.size() - 1);
            _shared_data.buffer[idx] = value;
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

    public:
        explicit OutputPort(SharedPortData& shared_data, size_t history) :
            _shared_data(shared_data),
            _history(history)
        {
            assert(is_power_of_2(_shared_data.buffer.size()) && "buffer size should be a power of 2");
        }

        IV_FORCEINLINE constexpr Sample get(size_t offset = 0) const
        {
            if (offset > _shared_data.latency + _history) return 0.0;
            size_t idx = (_shared_data.position + _shared_data.latency - offset) & (_shared_data.buffer.size() - 1);
            return _shared_data.buffer[idx];
        }

        IV_FORCEINLINE constexpr void push(Sample value) const
        {
            _shared_data.position = (_shared_data.position + 1) & (_shared_data.buffer.size() - 1);
            update(value);
        }

        IV_FORCEINLINE constexpr void update(Sample value, size_t offset = 0) const
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

    struct GraphInitContext {
        enum class PassMode {
            counting,
            initializing,
        };

        struct SharedBufferRecord {
            std::string id;
            void const* type_tag = nullptr;
            size_t count = 0;
            ptrdiff_t offset = 0;
            bool registered = false;
            bool fetched = false;
            bool fulfilled = false;
        };

        struct NodeBufferFrame {
            std::vector<size_t> immediate_sizes;
            std::vector<NodeBufferFrame> child_frames;
            size_t replay_size_index = 0;
            size_t replay_child_index = 0;
        };

        PassMode mode = PassMode::counting;
        std::span<std::byte> buffer;
        std::unordered_map<std::string, SharedBufferRecord> shared_buffers;
        NodeBufferFrame node_buffer_root;
        std::vector<NodeBufferFrame*> active_node_buffer_frames;

        GraphInitContext() = default;

        GraphInitContext(PassMode mode_, std::span<std::byte> buffer_ = {}) :
            mode(mode_),
            buffer(buffer_)
        {}

        GraphInitContext make_replay_context(std::span<std::byte> new_buffer) const
        {
            GraphInitContext replay(PassMode::initializing, new_buffer);
            replay.shared_buffers = shared_buffers;
            replay.node_buffer_root = node_buffer_root;
            replay.reset_node_buffer_replay(replay.node_buffer_root);
            for (auto& [_, record] : replay.shared_buffers) {
                record.fulfilled = false;
            }
            return replay;
        }

        void validate_after_counting() const
        {
            for (auto const& [_, record] : shared_buffers) {
                if (record.fetched && !record.registered) {
                    throw std::logic_error(
                        "shared buffer '" + record.id + "' was used but never registered during the first pass"
                    );
                }
            }
        }

        void validate_after_initialization() const
        {
            for (auto const& [_, record] : shared_buffers) {
                if (record.fetched && !record.registered) {
                    throw std::logic_error(
                        "shared buffer '" + record.id + "' was used but never registered"
                    );
                }
                if (record.registered && !record.fulfilled) {
                    throw std::logic_error(
                        "shared buffer '" + record.id + "' was not registered again during the second pass"
                    );
                }
            }
            if (!active_node_buffer_frames.empty()) {
                throw std::logic_error("graph node buffer replay scopes were not balanced during initialization");
            }
            if (!is_node_buffer_frame_fully_consumed(node_buffer_root)) {
                throw std::logic_error("node buffer size replay did not consume the full counting-pass record");
            }
        }

        void record_node_buffer_size(size_t size)
        {
            if (mode != PassMode::counting) {
                throw std::logic_error("node buffer sizes may only be recorded during the first pass");
            }
            current_node_buffer_frame().immediate_sizes.push_back(size);
        }

        void begin_graph_buffer_scope()
        {
            if (mode == PassMode::counting) {
                if (active_node_buffer_frames.empty()) {
                    node_buffer_root = {};
                    active_node_buffer_frames.push_back(&node_buffer_root);
                    return;
                }

                auto& child = active_node_buffer_frames.back()->child_frames.emplace_back();
                active_node_buffer_frames.push_back(&child);
                return;
            }

            if (active_node_buffer_frames.empty()) {
                active_node_buffer_frames.push_back(&node_buffer_root);
                return;
            }

            NodeBufferFrame& parent = *active_node_buffer_frames.back();
            if (parent.replay_child_index >= parent.child_frames.size()) {
                throw std::logic_error("node buffer frame replay ran past the counting-pass graph record");
            }
            active_node_buffer_frames.push_back(&parent.child_frames[parent.replay_child_index++]);
        }

        void end_graph_buffer_scope()
        {
            if (active_node_buffer_frames.empty()) {
                throw std::logic_error("graph node buffer scope stack underflow");
            }
            active_node_buffer_frames.pop_back();
        }

        size_t replay_node_buffer_size()
        {
            if (mode != PassMode::initializing) {
                throw std::logic_error("node buffer sizes may only be replayed during the second pass");
            }
            NodeBufferFrame& frame = current_node_buffer_frame();
            if (frame.replay_size_index >= frame.immediate_sizes.size()) {
                throw std::logic_error("node buffer size replay ran past the counting-pass record");
            }
            return frame.immediate_sizes[frame.replay_size_index++];
        }

        template<typename T>
        void register_buffer(std::string const& id, std::span<T> shared_buffer)
        {
            auto& record = shared_buffers[id];
            if (record.id.empty()) {
                record.id = id;
            }

            validate_buffer_identity<T>(record, shared_buffer);
            ptrdiff_t const offset = compute_offset(shared_buffer.data());

            if (mode == PassMode::counting) {
                if (record.registered) {
                    throw std::logic_error("shared buffer '" + record.id + "' was registered more than once");
                }
                record.type_tag = type_token<T>();
                record.count = shared_buffer.size();
                record.offset = offset;
                record.registered = true;
                record.fulfilled = false;
                return;
            }

            if (record.fulfilled) {
                throw std::logic_error("shared buffer '" + record.id + "' was registered more than once on the second pass");
            }
            if (!record.registered) {
                throw std::logic_error("shared buffer '" + record.id + "' was not registered during the first pass");
            }
            if (record.offset != offset) {
                throw std::logic_error("shared buffer '" + record.id + "' changed offset between init passes");
            }

            record.fulfilled = true;
        }

        template<typename T>
        std::span<T> use_buffer(std::string const& id)
        {
            auto& record = shared_buffers[id];
            if (record.id.empty()) {
                record.id = id;
            }

            if (record.type_tag && record.type_tag != type_token<T>()) {
                throw std::logic_error("shared buffer '" + record.id + "' was used with a different element type");
            }

            record.fetched = true;
            if (!record.type_tag) {
                record.type_tag = type_token<T>();
            }

            if (mode == PassMode::counting) {
                return {};
            }

            if (!record.registered) {
                throw std::logic_error("shared buffer '" + record.id + "' was used before first-pass registration");
            }
            if (record.type_tag != type_token<T>()) {
                throw std::logic_error("shared buffer '" + record.id + "' was registered with a different element type");
            }

            auto* data = reinterpret_cast<T*>(buffer.data() + record.offset);
            return { data, record.count };
        }

    private:
        template<typename T>
        static void const* type_token()
        {
            static int token = 0;
            return &token;
        }

        ptrdiff_t compute_offset(void const* data) const
        {
            if (!buffer.data() || !data) {
                return 0;
            }
            auto const* base = reinterpret_cast<std::byte const*>(buffer.data());
            auto const* ptr = reinterpret_cast<std::byte const*>(data);
            return ptr - base;
        }

        template<typename T>
        void validate_buffer_identity(SharedBufferRecord const& record, std::span<T> shared_buffer) const
        {
            if (record.type_tag && record.type_tag != type_token<T>()) {
                throw std::logic_error("shared buffer '" + record.id + "' was registered with a different element type");
            }
            if (record.count != 0 && record.count != shared_buffer.size()) {
                throw std::logic_error("shared buffer '" + record.id + "' changed element count between registrations");
            }
        }

        NodeBufferFrame& current_node_buffer_frame()
        {
            if (active_node_buffer_frames.empty()) {
                throw std::logic_error("graph node buffer scope was not active");
            }
            return *active_node_buffer_frames.back();
        }

        static void reset_node_buffer_replay(NodeBufferFrame& frame)
        {
            frame.replay_size_index = 0;
            frame.replay_child_index = 0;
            for (auto& child : frame.child_frames) {
                reset_node_buffer_replay(child);
            }
        }

        static bool is_node_buffer_frame_fully_consumed(NodeBufferFrame const& frame)
        {
            if (frame.replay_size_index != frame.immediate_sizes.size()) {
                return false;
            }
            if (frame.replay_child_index != frame.child_frames.size()) {
                return false;
            }
            for (auto const& child : frame.child_frames) {
                if (!is_node_buffer_frame_fully_consumed(child)) {
                    return false;
                }
            }
            return true;
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
        concept has_init_buffer_ctx = requires(Node node, Allocator allocator, GraphInitContext ctx)
        {
            node.init_buffer(allocator, ctx);
        };

        template <typename Node>
        concept has_internal_latency = requires(Node node, size_t internal_latency)
        {
            internal_latency = node.internal_latency();
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
    constexpr std::span<std::byte> do_init_buffer(Node const& node, Allocator& allocator, GraphInitContext& ctx)
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
}
