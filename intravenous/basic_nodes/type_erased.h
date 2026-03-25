#pragma once

#include "alligator.h"
#include "node.h"

#include <array>
#include <memory>
#include <type_traits>
#include <vector>

namespace iv {
    struct Constant {
        Sample _value;

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        constexpr void tick(TickState const& state)
        {
            state.outputs[0].push(_value);
        }
    };

    class TypeErasedNode {
        std::shared_ptr<void> _node;
        std::vector<InputConfig> _inputs;
        std::vector<OutputConfig> _outputs;
        size_t _internal_latency;
        size_t _max_block_size;
        std::span<std::byte>(*_init_buffer_fn)(void*, TypeErasedAllocator, InitBufferContext&);
        void (*_tick_fn)(void*, TickState const&);
        void (*_tick_block_fn)(void*, BlockTickState const&);

    public:
        template<typename Node>
        constexpr /*implicit*/ TypeErasedNode(Node node)
        {
            if constexpr (std::is_empty_v<Node>) {
                _node = nullptr;
                _init_buffer_fn = [](void*, TypeErasedAllocator allocator, InitBufferContext& ctx) {
                    return do_init_buffer(Node{}, allocator, ctx);
                };
                _tick_fn = [](void*, TickState const& state) {
                    Node node {};
                    do_tick(node, state);
                };
                _tick_block_fn = [](void*, BlockTickState const& state) {
                    Node node {};
                    do_tick_block(node, state);
                };
            } else {
                _node = std::make_shared<Node>(node);
                _init_buffer_fn = [](void* node_ptr, TypeErasedAllocator allocator, InitBufferContext& ctx) {
                    return do_init_buffer(*static_cast<Node*>(node_ptr), allocator, ctx);
                };
                _tick_fn = [](void* node_ptr, TickState const& state) {
                    do_tick(*static_cast<Node*>(node_ptr), state);
                };
                _tick_block_fn = [](void* node_ptr, BlockTickState const& state) {
                    do_tick_block(*static_cast<Node*>(node_ptr), state);
                };
            }
            _inputs.assign_range(get_inputs(node));
            _outputs.assign_range(get_outputs(node));
            _internal_latency = get_internal_latency(node);
            _max_block_size = get_max_block_size(node);
            validate_max_block_size(_max_block_size, "node max_block_size() must be a power of 2");
        }

        constexpr std::vector<InputConfig> const& inputs() const
        {
            return _inputs;
        }

        constexpr std::vector<OutputConfig> const& outputs() const
        {
            return _outputs;
        }

        constexpr size_t internal_latency() const
        {
            return _internal_latency;
        }

        constexpr size_t max_block_size() const
        {
            return _max_block_size;
        }

        template<typename Allocator>
        constexpr std::span<std::byte> init_buffer(Allocator& allocator, InitBufferContext& ctx) const
        {
            return _init_buffer_fn(_node.get(), TypeErasedAllocator { allocator }, ctx);
        }

        void tick(TickState const& state)
        {
            _tick_fn(_node.get(), state);
        }

        void tick_block(BlockTickState const& state)
        {
            _tick_block_fn(_node.get(), state);
        }
    };
}
