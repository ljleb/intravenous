#pragma once

#include "alligator.h"
#include "node_lifecycle.h"

#include <array>
#include <memory>
#include <sstream>
#include <type_traits>
#include <vector>

namespace iv {
    struct Constant {
        Sample _value;

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickSampleContext<Constant> const& state) const
        {
            state.outputs[0].push(_value);
        }
    };

    class TypeErasedNode {
        using NodeStoragePtr = std::unique_ptr<void, void(*)(void*)>;

        NodeStoragePtr _node { nullptr, +[](void*) {} };
        std::vector<InputConfig> _inputs;
        std::vector<OutputConfig> _outputs;
        size_t _internal_latency;
        size_t _max_block_size;
        char const* _type_name = "<unknown>";
        void (*_declare_fn)(void*, DeclarationContext<TypeErasedNode> const&);
        void (*_tick_fn)(void*, TickSampleContext<TypeErasedNode> const&);
        void (*_tick_block_fn)(void*, TickBlockContext<TypeErasedNode> const&);

    public:
        struct State {
            std::span<std::span<std::byte>> nested_node_states;
        };

        TypeErasedNode() = default;
        TypeErasedNode(TypeErasedNode const&) = delete;
        TypeErasedNode& operator=(TypeErasedNode const&) = delete;

        TypeErasedNode(TypeErasedNode&&) noexcept = default;
        TypeErasedNode& operator=(TypeErasedNode&&) noexcept = default;

        template<typename Node>
        /*implicit*/ TypeErasedNode(Node node)
        {
            _inputs.assign_range(get_inputs(node));
            _outputs.assign_range(get_outputs(node));
            _internal_latency = get_internal_latency(node);
            _max_block_size = get_max_block_size(node);
            _type_name = typeid(Node).name();
            validate_max_block_size(_max_block_size, "node max_block_size() must be a power of 2");

            if constexpr (std::is_empty_v<Node>) {
                _node = NodeStoragePtr(nullptr, +[](void*) {});
                _declare_fn = [](void*, DeclarationContext<TypeErasedNode> const& ctx) {
                    auto const& state = ctx.state();
                    do_declare(Node{}, ctx);
                    ctx.nested_node_states(state.nested_node_states);
                };
                _tick_fn = [](void*, TickSampleContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    do_tick(Node{}, TickSampleContext<Node> {
                        TickContext<Node> { .inputs = ctx.inputs, .outputs = ctx.outputs, .buffer = state.nested_node_states[0] },
                        ctx.index,
                    });
                };
                _tick_block_fn = [](void*, TickBlockContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    do_tick_block(Node{}, TickBlockContext<Node> {
                        TickContext<Node> { .inputs = ctx.inputs, .outputs = ctx.outputs, .buffer = state.nested_node_states[0] },
                        ctx.index,
                        ctx.block_size,
                    });
                };
            } else {
                _node = NodeStoragePtr(
                    new Node(std::move(node)),
                    +[](void* ptr) { delete static_cast<Node*>(ptr); }
                );
                _declare_fn = [](void* node, DeclarationContext<TypeErasedNode> const& ctx) {
                    auto const& state = ctx.state();
                    do_declare(*static_cast<Node*>(node), ctx);
                    ctx.nested_node_states(state.nested_node_states);
                };
                _tick_fn = [](void* node, TickSampleContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    do_tick(*static_cast<Node*>(node), TickSampleContext<Node> {
                        TickContext<Node> { .inputs = ctx.inputs, .outputs = ctx.outputs, .buffer = state.nested_node_states[0] },
                        ctx.index,
                    });
                };
                _tick_block_fn = [](void* node, TickBlockContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    do_tick_block(*static_cast<Node*>(node), TickBlockContext<Node> {
                        TickContext<Node> { .inputs = ctx.inputs, .outputs = ctx.outputs, .buffer = state.nested_node_states[0] },
                        ctx.index,
                        ctx.block_size,
                    });
                };
            }
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

        char const* type_name() const
        {
            return _type_name;
        }

        void declare(DeclarationContext<TypeErasedNode> const& ctx) const
        {
            return _declare_fn(_node.get(), ctx);
        }

        void tick(TickSampleContext<TypeErasedNode> const& ctx) const
        {
            _tick_fn(_node.get(), ctx);
        }

        void tick_block(TickBlockContext<TypeErasedNode> const& ctx) const
        {
            _tick_block_fn(_node.get(), ctx);
        }
    };
}
