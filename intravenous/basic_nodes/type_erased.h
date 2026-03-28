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

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickContext<Constant> const& state)
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
        void (*_declare_fn)(void*, DeclarationContext<TypeErasedNode> const&);
        void (*_initialize_fn)(void*, InitializationContext<TypeErasedNode> const&);
        void (*_tick_fn)(void*, TickContext<TypeErasedNode> const&);
        void (*_tick_block_fn)(void*, TickBlockContext<TypeErasedNode> const&);

    public:
        template<typename Node>
        /*implicit*/ TypeErasedNode(Node node)
        {
            if constexpr (std::is_empty_v<Node>) {
                _node = nullptr;
                _declare_fn = [](void*, DeclarationContext<TypeErasedNode> const& ctx) {
                    return do_declare(Node{}, ctx);
                };
                _initialize_fn = [](void*, InitializationContext<TypeErasedNode> const& ctx) {
                    return do_initialize(Node{}, ctx);
                };
                _tick_fn = [](void*, TickContext<TypeErasedNode> const& ctx) {
                    do_tick(Node{}, ctx);
                };
                _tick_block_fn = [](void*, TickBlockContext<TypeErasedNode> const& ctx) {
                    do_tick_block(Node{}, ctx);
                };
            } else {
                _node = std::make_shared<Node>(node);
                _declare_fn = [](void* node, DeclarationContext<TypeErasedNode> const& ctx) {
                    return do_declare(*static_cast<Node*>(node), ctx);
                };
                _initialize_fn = [](void* node, InitializationContext<TypeErasedNode> const& ctx) {
                    return do_initialize(*static_cast<Node*>(node), ctx);
                };
                _tick_fn = [](void* node, TickContext<TypeErasedNode> const& ctx) {
                    do_tick(*static_cast<Node*>(node), ctx);
                };
                _tick_block_fn = [](void* node, TickBlockContext<TypeErasedNode> const& ctx) {
                    do_tick_block(*static_cast<Node*>(node), ctx);
                };
            }
            _inputs.assign_range(get_inputs(node));
            _outputs.assign_range(get_outputs(node));
            _internal_latency = get_internal_latency(node);
            _max_block_size = get_max_block_size(node);
            validate_max_block_size(_max_block_size, "node max_block_size() must be a power of 2");
        }

        std::vector<InputConfig> const& inputs() const
        {
            return _inputs;
        }

        std::vector<OutputConfig> const& outputs() const
        {
            return _outputs;
        }

        size_t internal_latency() const
        {
            return _internal_latency;
        }

        size_t max_block_size() const
        {
            return _max_block_size;
        }

        void declare(DeclarationContext<TypeErasedNode> const& ctx) const
        {
            return _declare_fn(_node.get(), ctx);
        }

        void initialize(InitializationContext<TypeErasedNode> const& ctx) const
        {
            return _initialize_fn(_node.get(), ctx);
        }

        void tick(TickContext<TypeErasedNode> const& ctx)
        {
            _tick_fn(_node.get(), ctx);
        }

        void tick_block(TickBlockContext<TypeErasedNode> const& ctx)
        {
            _tick_block_fn(_node.get(), ctx);
        }
    };
}
