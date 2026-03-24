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
        std::span<std::byte>(*_init_buffer_fn)(void*, TypeErasedAllocator, GraphInitContext&);
        void (*_tick_fn)(void*, TickState const&);

    public:
        template<typename Node>
        constexpr /*implicit*/ TypeErasedNode(Node node)
        {
            if constexpr (std::is_empty_v<Node>) {
                _node = nullptr;
                _init_buffer_fn = [](void*, TypeErasedAllocator allocator, GraphInitContext& ctx) {
                    return do_init_buffer(Node{}, allocator, ctx);
                };
                _tick_fn = [](void*, TickState const& state) { Node{}.tick(state); };
            } else {
                _node = std::make_shared<Node>(node);
                _init_buffer_fn = [](void* node_ptr, TypeErasedAllocator allocator, GraphInitContext& ctx) {
                    return do_init_buffer(*static_cast<Node*>(node_ptr), allocator, ctx);
                };
                _tick_fn = [](void* node_ptr, TickState const& state) {
                    static_cast<Node*>(node_ptr)->tick(state);
                };
            }
            _inputs.assign_range(get_inputs(node));
            _outputs.assign_range(get_outputs(node));
            _internal_latency = get_internal_latency(node);
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

        template<typename Allocator>
        constexpr std::span<std::byte> init_buffer(Allocator& allocator, GraphInitContext& ctx) const
        {
            return _init_buffer_fn(_node.get(), TypeErasedAllocator { allocator }, ctx);
        }

        void tick(TickState const& state)
        {
            _tick_fn(_node.get(), state);
        }
    };
}
