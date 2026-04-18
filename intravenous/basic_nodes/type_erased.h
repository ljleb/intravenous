#pragma once

#include "node/lifecycle.h"

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <typeinfo>
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
        std::vector<EventInputConfig> _event_inputs;
        std::vector<EventOutputConfig> _event_outputs;
        size_t _internal_latency;
        size_t _max_block_size;
        std::optional<size_t> _ttl_samples;
        bool _can_skip_block = false;
        char const* _type_name = "<unknown>";
        std::type_info const* _type_info = &typeid(void);
        void const* (*_const_ptr_fn)(void const*) = +[](void const*) -> void const* { return nullptr; };
        void (*_declare_fn)(void*, DeclarationContext<TypeErasedNode> const&) = nullptr;
        void (*_tick_fn)(void*, TickSampleContext<TypeErasedNode> const&) = nullptr;
        void (*_tick_block_fn)(void*, TickBlockContext<TypeErasedNode> const&) = nullptr;
        void (*_skip_block_fn)(void*, SkipBlockContext<TypeErasedNode> const&) = nullptr;

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
            auto const inputs = get_inputs(node);
            auto const outputs = get_outputs(node);
            auto const event_inputs = get_event_inputs(node);
            auto const event_outputs = get_event_outputs(node);
            _inputs.assign(inputs.begin(), inputs.end());
            _outputs.assign(outputs.begin(), outputs.end());
            _event_inputs.assign(event_inputs.begin(), event_inputs.end());
            _event_outputs.assign(event_outputs.begin(), event_outputs.end());
            _internal_latency = get_internal_latency(node);
            _max_block_size = get_max_block_size(node);
            _ttl_samples = get_ttl_samples(node);
            _can_skip_block = get_can_skip_block(node);
            _type_name = typeid(Node).name();
            _type_info = &typeid(Node);
            validate_max_block_size(_max_block_size, "node max_block_size() must be a power of 2");

            if constexpr (std::is_empty_v<Node>) {
                _node = NodeStoragePtr(nullptr, +[](void*) {});
                _const_ptr_fn = +[](void const*) -> void const* { return nullptr; };
                _declare_fn = [](void*, DeclarationContext<TypeErasedNode> const& ctx) {
                    auto const& state = ctx.state();
                    do_declare(Node{}, ctx);
                    ctx.nested_node_states(state.nested_node_states);
                };
                _tick_fn = [](void*, TickSampleContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    do_tick(Node{}, TickSampleContext<Node> {
                        TickContext<Node> {
                            .inputs = ctx.inputs,
                            .outputs = ctx.outputs,
                            .event_inputs = ctx.event_inputs,
                            .event_outputs = ctx.event_outputs,
                            .scc_feedback_latency = ctx.scc_feedback_latency,
                            .buffer = state.nested_node_states[0]
                        },
                        ctx.index,
                    });
                };
                _tick_block_fn = [](void*, TickBlockContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    do_tick_block(Node{}, TickBlockContext<Node> {
                        TickContext<Node> {
                            .inputs = ctx.inputs,
                            .outputs = ctx.outputs,
                            .event_inputs = ctx.event_inputs,
                            .event_outputs = ctx.event_outputs,
                            .scc_feedback_latency = ctx.scc_feedback_latency,
                            .buffer = state.nested_node_states[0]
                        },
                        ctx.index,
                        ctx.block_size,
                    });
                };
                _skip_block_fn = [](void*, SkipBlockContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    do_skip_block(Node{}, SkipBlockContext<Node> {
                        TickContext<Node> {
                            .inputs = ctx.inputs,
                            .outputs = ctx.outputs,
                            .event_inputs = ctx.event_inputs,
                            .event_outputs = ctx.event_outputs,
                            .scc_feedback_latency = ctx.scc_feedback_latency,
                            .buffer = state.nested_node_states[0]
                        },
                        ctx.index,
                        ctx.block_size,
                    });
                };
            } else {
                _node = NodeStoragePtr(
                    new Node(std::move(node)),
                    +[](void* ptr) { delete static_cast<Node*>(ptr); }
                );
                _const_ptr_fn = +[](void const* ptr) -> void const* { return ptr; };
                _declare_fn = [](void* node, DeclarationContext<TypeErasedNode> const& ctx) {
                    auto const& state = ctx.state();
                    do_declare(*static_cast<Node*>(node), ctx);
                    ctx.nested_node_states(state.nested_node_states);
                };
                _tick_fn = [](void* node, TickSampleContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    do_tick(*static_cast<Node*>(node), TickSampleContext<Node> {
                        TickContext<Node> {
                            .inputs = ctx.inputs,
                            .outputs = ctx.outputs,
                            .event_inputs = ctx.event_inputs,
                            .event_outputs = ctx.event_outputs,
                            .scc_feedback_latency = ctx.scc_feedback_latency,
                            .buffer = state.nested_node_states[0]
                        },
                        ctx.index,
                    });
                };
                _tick_block_fn = [](void* node, TickBlockContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    do_tick_block(*static_cast<Node*>(node), TickBlockContext<Node> {
                        TickContext<Node> {
                            .inputs = ctx.inputs,
                            .outputs = ctx.outputs,
                            .event_inputs = ctx.event_inputs,
                            .event_outputs = ctx.event_outputs,
                            .scc_feedback_latency = ctx.scc_feedback_latency,
                            .buffer = state.nested_node_states[0]
                        },
                        ctx.index,
                        ctx.block_size,
                    });
                };
                _skip_block_fn = [](void* node, SkipBlockContext<TypeErasedNode> const& ctx) {
                    auto& state = ctx.state();
                    do_skip_block(*static_cast<Node*>(node), SkipBlockContext<Node> {
                        TickContext<Node> {
                            .inputs = ctx.inputs,
                            .outputs = ctx.outputs,
                            .event_inputs = ctx.event_inputs,
                            .event_outputs = ctx.event_outputs,
                            .scc_feedback_latency = ctx.scc_feedback_latency,
                            .buffer = state.nested_node_states[0]
                        },
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

        std::vector<EventInputConfig> const& event_inputs() const
        {
            return _event_inputs;
        }

        std::vector<EventOutputConfig> const& event_outputs() const
        {
            return _event_outputs;
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

        template<class Node>
        Node const* try_as() const
        {
            if (*_type_info != typeid(Node)) {
                return nullptr;
            }
            return static_cast<Node const*>(_const_ptr_fn(_node.get()));
        }

        std::optional<size_t> ttl_samples() const
        {
            return _ttl_samples;
        }

        bool can_skip_block() const
        {
            return _can_skip_block;
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

        void skip_block(SkipBlockContext<TypeErasedNode> const& ctx) const
        {
            _skip_block_fn(_node.get(), ctx);
        }
    };
}
