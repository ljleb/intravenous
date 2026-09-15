#pragma once

#include <intravenous/node/lifecycle.h>

#include <concepts>
#include <optional>
#include <type_traits>
#include <typeinfo>
#include <vector>

namespace iv {
    class WeakTypeErasedNode {
        void const* _node = nullptr;
        std::vector<SampleInputConfig> (*_inputs_fn)(void const*) = nullptr;
        std::vector<SampleOutputConfig> (*_outputs_fn)(void const*) = nullptr;
        std::vector<EventInputConfig> (*_event_inputs_fn)(void const*) = nullptr;
        std::vector<EventOutputConfig> (*_event_outputs_fn)(void const*) = nullptr;
        size_t (*_internal_latency_fn)(void const*) = nullptr;
        size_t (*_max_block_size_fn)(void const*) = nullptr;
        std::optional<size_t> (*_ttl_samples_fn)(void const*) = nullptr;
        bool (*_can_skip_block_fn)(void const*) = nullptr;
        char const* _type_name = "<unknown>";
        std::type_info const* _type_info = &typeid(void);
        void (*_declare_fn)(void const*, DeclarationContext<WeakTypeErasedNode> const&) = nullptr;
        void (*_tick_fn)(void const*, TickSampleContext<WeakTypeErasedNode> const&) = nullptr;
        void (*_tick_block_fn)(void const*, TickBlockContext<WeakTypeErasedNode> const&) = nullptr;
        void (*_skip_block_fn)(void const*, SkipBlockContext<WeakTypeErasedNode> const&) = nullptr;

        template<class Config, class Range>
        static std::vector<Config> copy_configs(Range&& range)
        {
            return {range.begin(), range.end()};
        }

    public:
        WeakTypeErasedNode() = default;
        WeakTypeErasedNode(WeakTypeErasedNode const&) = default;
        WeakTypeErasedNode& operator=(WeakTypeErasedNode const&) = default;
        WeakTypeErasedNode(WeakTypeErasedNode&&) noexcept = default;
        WeakTypeErasedNode& operator=(WeakTypeErasedNode&&) noexcept = default;

        template<class Node>
            requires (!std::same_as<std::remove_cvref_t<Node>, WeakTypeErasedNode>)
        /*implicit*/ WeakTypeErasedNode(Node const& node)
            : _node(&node)
        {
            _inputs_fn = [](void const* node_ptr) {
                return get_inputs(*static_cast<Node const*>(node_ptr));
            };
            _outputs_fn = [](void const* node_ptr) {
                return get_outputs(*static_cast<Node const*>(node_ptr));
            };
            _event_inputs_fn = [](void const* node_ptr) {
                return get_event_inputs(*static_cast<Node const*>(node_ptr));
            };
            _event_outputs_fn = [](void const* node_ptr) {
                return get_event_outputs(*static_cast<Node const*>(node_ptr));
            };
            _internal_latency_fn = [](void const* node_ptr) {
                return get_internal_latency(*static_cast<Node const*>(node_ptr));
            };
            _max_block_size_fn = [](void const* node_ptr) {
                return get_max_block_size(*static_cast<Node const*>(node_ptr));
            };
            _ttl_samples_fn = [](void const* node_ptr) {
                return get_ttl_samples(*static_cast<Node const*>(node_ptr));
            };
            _can_skip_block_fn = [](void const* node_ptr) {
                return get_can_skip_block(*static_cast<Node const*>(node_ptr));
            };
            _type_name = typeid(Node).name();
            _type_info = &typeid(Node);
            validate_max_block_size(
                _max_block_size_fn(_node),
                "node max_block_size() must be a power of 2");

            _declare_fn = [](void const* node_ptr, DeclarationContext<WeakTypeErasedNode> const& ctx) {
                do_declare(*static_cast<Node const*>(node_ptr), ctx);
            };
            _tick_fn = [](void const* node_ptr, TickSampleContext<WeakTypeErasedNode> const& ctx) {
                do_tick(*static_cast<Node const*>(node_ptr), TickSampleContext<Node> {
                    TickContext<Node> {
                        .inputs = ctx.inputs,
                        .outputs = ctx.outputs,
                        .event_inputs = ctx.event_inputs,
                        .event_outputs = ctx.event_outputs,
                        .sample_rate = ctx.sample_rate,
                        .scc_feedback_latency = ctx.scc_feedback_latency,
                        .buffer = ctx.buffer
                    },
                    ctx.index,
                });
            };
            _tick_block_fn = [](void const* node_ptr, TickBlockContext<WeakTypeErasedNode> const& ctx) {
                do_tick_block(*static_cast<Node const*>(node_ptr), TickBlockContext<Node> {
                    TickContext<Node> {
                        .inputs = ctx.inputs,
                        .outputs = ctx.outputs,
                        .event_inputs = ctx.event_inputs,
                        .event_outputs = ctx.event_outputs,
                        .sample_rate = ctx.sample_rate,
                        .scc_feedback_latency = ctx.scc_feedback_latency,
                        .buffer = ctx.buffer
                    },
                    ctx.index,
                    ctx.block_size,
                });
            };
            _skip_block_fn = [](void const* node_ptr, SkipBlockContext<WeakTypeErasedNode> const& ctx) {
                do_skip_block(*static_cast<Node const*>(node_ptr), SkipBlockContext<Node> {
                    TickContext<Node> {
                        .inputs = ctx.inputs,
                        .outputs = ctx.outputs,
                        .event_inputs = ctx.event_inputs,
                        .event_outputs = ctx.event_outputs,
                        .sample_rate = ctx.sample_rate,
                        .scc_feedback_latency = ctx.scc_feedback_latency,
                        .buffer = ctx.buffer
                    },
                    ctx.index,
                    ctx.block_size,
                });
            };
        }

        template<class Node>
            requires (!std::is_lvalue_reference_v<Node>)
        WeakTypeErasedNode(Node&&) = delete;

        explicit operator bool() const { return _node != nullptr; }

        std::vector<InputConfig> inputs() const
        {
            auto const sample_inputs = _inputs_fn(_node);
            auto const event_inputs = _event_inputs_fn(_node);
            std::vector<InputConfig> result;
            result.reserve(sample_inputs.size() + event_inputs.size());
            for (SampleInputConfig const& input : sample_inputs) {
                result.emplace_back(input.name, SampleInputProperties{
                    .channel_layout = input.channel_layout,
                    .history = input.history,
                    .default_value = input.default_value,
                    .min = input.min,
                    .max = input.max,
                });
            }
            for (EventInputConfig const& input : event_inputs) {
                result.emplace_back(
                    input.name, EventInputProperties{.type = input.type});
            }
            return result;
        }

        std::vector<OutputConfig> outputs() const
        {
            auto const sample_outputs = _outputs_fn(_node);
            auto const event_outputs = _event_outputs_fn(_node);
            std::vector<OutputConfig> result;
            result.reserve(sample_outputs.size() + event_outputs.size());
            for (SampleOutputConfig const& output : sample_outputs) {
                result.emplace_back(output.name, SampleOutputProperties{
                    .channel_layout = output.channel_layout,
                    .latency = output.latency,
                    .history = output.history,
                });
            }
            for (EventOutputConfig const& output : event_outputs) {
                result.emplace_back(
                    output.name, EventOutputProperties{.type = output.type});
            }
            return result;
        }
        size_t internal_latency() const { return _internal_latency_fn(_node); }
        size_t max_block_size() const { return _max_block_size_fn(_node); }
        char const* type_name() const { return _type_name; }

        template<class Node>
        Node const* try_as() const
        {
            if (*_type_info != typeid(Node)) return nullptr;
            return static_cast<Node const*>(_node);
        }

        std::optional<size_t> ttl_samples() const { return _ttl_samples_fn(_node); }
        bool can_skip_block() const { return _can_skip_block_fn(_node); }
        void declare(DeclarationContext<WeakTypeErasedNode> const& ctx) const { _declare_fn(_node, ctx); }
        void tick(TickSampleContext<WeakTypeErasedNode> const& ctx) const { _tick_fn(_node, ctx); }
        void tick_block(TickBlockContext<WeakTypeErasedNode> const& ctx) const { _tick_block_fn(_node, ctx); }
        void skip_block(SkipBlockContext<WeakTypeErasedNode> const& ctx) const { _skip_block_fn(_node, ctx); }
    };
}
