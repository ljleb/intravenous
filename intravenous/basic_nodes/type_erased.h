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
        void (*_initialize_fn)(void*, InitializationContext<TypeErasedNode> const&);
        void (*_tick_fn)(void*, TickSampleContext<TypeErasedNode> const&);
        void (*_tick_block_fn)(void*, TickBlockContext<TypeErasedNode> const&);

    public:
        struct State {
            std::span<std::byte*> nested_nodes;
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
                    if (ctx.pending_direct_nested_node_count() != 1) {
                        std::ostringstream oss;
                        oss << "type-erased node declare captured wrong direct child count"
                            << " (count=" << ctx.pending_direct_nested_node_count() << ")";
                        throw std::logic_error(oss.str());
                    }
                    ctx.nested_node_states(state.nested_nodes);
                };
                _initialize_fn = [](void*, InitializationContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    if (state.nested_nodes.size() != 1 || state.nested_nodes[0] == nullptr) {
                        std::ostringstream oss;
                        oss << "type-erased node has invalid nested state during initialize"
                            << " (count=" << state.nested_nodes.size()
                            << ", span_data=" << static_cast<void*>(state.nested_nodes.data());
                        if (!state.nested_nodes.empty()) {
                            oss << ", child=" << static_cast<void*>(state.nested_nodes[0]);
                        }
                        oss << ")";
                        throw std::logic_error(oss.str());
                    }
                    auto* const self_state = reinterpret_cast<std::byte*>(std::addressof(state));
                    if (state.nested_nodes[0] < self_state) {
                        std::ostringstream oss;
                        oss << "type-erased nested node state pointer must not precede the type-erased state"
                            << " (self=" << static_cast<void*>(self_state)
                            << ", child=" << static_cast<void*>(state.nested_nodes[0]) << ")";
                        throw std::logic_error(oss.str());
                    }
                };
                _tick_fn = [](void*, TickSampleContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    IV_ASSERT(state.nested_nodes.size() == 1, "type-erased node must own exactly one nested node state");
                    IV_ASSERT(state.nested_nodes[0] != nullptr, "type-erased nested node state pointer must not be null");
                    do_tick(Node{}, TickSampleContext<Node> {
                        make_nested_tick_context<Node>(
                            static_cast<TickContext<TypeErasedNode> const&>(ctx),
                            state.nested_nodes[0],
                            ctx.inputs,
                            ctx.outputs
                        ),
                        ctx.index,
                    });
                };
                _tick_block_fn = [](void*, TickBlockContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    if (state.nested_nodes.size() != 1 || state.nested_nodes[0] == nullptr) {
                        std::ostringstream oss;
                        oss << "type-erased node has invalid nested state"
                            << " (count=" << state.nested_nodes.size()
                            << ", span_data=" << static_cast<void*>(state.nested_nodes.data());
                        if (!state.nested_nodes.empty()) {
                            oss << ", child=" << static_cast<void*>(state.nested_nodes[0]);
                        }
                        oss << ")";
                        throw std::logic_error(oss.str());
                    }
                    do_tick_block(Node{}, TickBlockContext<Node> {
                        make_nested_tick_context<Node>(
                            static_cast<TickContext<TypeErasedNode> const&>(ctx),
                            state.nested_nodes[0],
                            ctx.inputs,
                            ctx.outputs
                        ),
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
                    if (ctx.pending_direct_nested_node_count() != 1) {
                        std::ostringstream oss;
                        oss << "type-erased node declare captured wrong direct child count"
                            << " (count=" << ctx.pending_direct_nested_node_count() << ")";
                        throw std::logic_error(oss.str());
                    }
                    ctx.nested_node_states(state.nested_nodes);
                };
                _initialize_fn = [](void*, InitializationContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    if (state.nested_nodes.size() != 1 || state.nested_nodes[0] == nullptr) {
                        std::ostringstream oss;
                        oss << "type-erased node has invalid nested state during initialize"
                            << " (count=" << state.nested_nodes.size()
                            << ", span_data=" << static_cast<void*>(state.nested_nodes.data());
                        if (!state.nested_nodes.empty()) {
                            oss << ", child=" << static_cast<void*>(state.nested_nodes[0]);
                        }
                        oss << ")";
                        throw std::logic_error(oss.str());
                    }
                    auto* const self_state = reinterpret_cast<std::byte*>(std::addressof(state));
                    if (state.nested_nodes[0] < self_state) {
                        std::ostringstream oss;
                        oss << "type-erased nested node state pointer must not precede the type-erased state"
                            << " (self=" << static_cast<void*>(self_state)
                            << ", child=" << static_cast<void*>(state.nested_nodes[0]) << ")";
                        throw std::logic_error(oss.str());
                    }
                };
                _tick_fn = [](void* node, TickSampleContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    IV_ASSERT(state.nested_nodes.size() == 1, "type-erased node must own exactly one nested node state");
                    IV_ASSERT(state.nested_nodes[0] != nullptr, "type-erased nested node state pointer must not be null");
                    do_tick(*static_cast<Node*>(node), TickSampleContext<Node> {
                        make_nested_tick_context<Node>(
                            static_cast<TickContext<TypeErasedNode> const&>(ctx),
                            state.nested_nodes[0],
                            ctx.inputs,
                            ctx.outputs
                        ),
                        ctx.index,
                    });
                };
                _tick_block_fn = [](void* node, TickBlockContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    if (state.nested_nodes.size() != 1 || state.nested_nodes[0] == nullptr) {
                        std::ostringstream oss;
                        oss << "type-erased node has invalid nested state"
                            << " (count=" << state.nested_nodes.size()
                            << ", span_data=" << static_cast<void*>(state.nested_nodes.data());
                        if (!state.nested_nodes.empty()) {
                            oss << ", child=" << static_cast<void*>(state.nested_nodes[0]);
                        }
                        oss << ")";
                        throw std::logic_error(oss.str());
                    }
                    do_tick_block(*static_cast<Node*>(node), TickBlockContext<Node> {
                        make_nested_tick_context<Node>(
                            static_cast<TickContext<TypeErasedNode> const&>(ctx),
                            state.nested_nodes[0],
                            ctx.inputs,
                            ctx.outputs
                        ),
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

        void initialize(InitializationContext<TypeErasedNode> const& ctx) const
        {
            return _initialize_fn(_node.get(), ctx);
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
