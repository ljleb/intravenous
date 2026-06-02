#pragma once

#include "traits.h"
#include "resources.h"

#include <sstream>
#include <memory>
#include <span>

namespace iv {
    template<typename Node>
    struct TickContext {
        std::span<InputPort> inputs;
        std::span<OutputPort> outputs;
        std::span<EventInputPort> event_inputs;
        std::span<EventOutputPort> event_outputs;
        size_t scc_feedback_latency = 0;
        std::span<std::byte> buffer;

        using State = typename NodeState<Node>::Type;

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>);
    };

    template<typename Node>
    struct TickSampleContext : public TickContext<Node> {
        size_t index;

        TickSampleContext(TickContext<Node> base, size_t index);
    };

    template<typename Node>
    struct TickBlockContext : public TickContext<Node> {
        size_t index;
        size_t block_size;

        TickBlockContext(
            TickContext<Node> base,
            size_t index,
            size_t block_size
        );
    };

    template<typename Node>
    struct SkipBlockContext : public TickBlockContext<Node> {
        using TickBlockContext<Node>::TickBlockContext;
    };

    namespace details {
        template <typename Node>
        concept has_tick = requires(Node node, TickSampleContext<Node> state)
        {
            node.tick(state);
        };

        template <typename Node>
        concept has_tick_block = requires(Node node, TickBlockContext<Node> state)
        {
            node.tick_block(state);
        };

        template <typename Node>
        concept has_skip_block = requires(Node node, SkipBlockContext<Node> state)
        {
            node.skip_block(state);
        };
    }

    template<typename Node>
    IV_FORCEINLINE std::add_lvalue_reference_t<typename TickContext<Node>::State> TickContext<Node>::state() const
    requires(!std::is_void_v<State>)
    {
        void* ptr = buffer.data();
        size_t space = buffer.size();
        return *reinterpret_cast<State*>(std::align(alignof(State), sizeof(State), ptr, space));
    }

    template<typename Node>
    IV_FORCEINLINE TickSampleContext<Node>::TickSampleContext(TickContext<Node> base, size_t index)
    : TickContext<Node>(base), index(index)
    {}

    template<typename Node>
    IV_FORCEINLINE TickBlockContext<Node>::TickBlockContext(
        TickContext<Node> base,
        size_t index,
        size_t block_size
    )
    : TickContext<Node>(base)
    , index(index)
    , block_size(block_size)
    {}

    template<typename Node>
    void do_tick_block(Node const& node, TickBlockContext<Node> const& state);

    template<typename Node>
    void do_skip_block(Node const& node, SkipBlockContext<Node> const& state);

    IV_FORCEINLINE std::span<std::byte> remaining_buffer(std::span<std::byte> buffer, std::byte* state_base)
    {
        if (!state_base) {
            throw std::logic_error("nested node state pointer cannot be null");
        }

        auto* const buffer_begin = buffer.data();
        auto* const buffer_end = buffer_begin + buffer.size();
        if (state_base < buffer_begin || state_base > buffer_end) {
            std::ostringstream oss;
            oss << "nested node state pointer is outside the enclosing buffer"
                << " (state=" << static_cast<void*>(state_base)
                << ", begin=" << static_cast<void*>(buffer_begin)
                << ", end=" << static_cast<void*>(buffer_end) << ")";
            throw std::logic_error(oss.str());
        }

        return { state_base, static_cast<size_t>(buffer_end - state_base) };
    }

    template<typename NestedNode, typename OuterNode>
    IV_FORCEINLINE TickContext<NestedNode> make_nested_tick_context(
        TickContext<OuterNode> const& outer,
        std::byte* nested_state,
        std::span<InputPort> inputs,
        std::span<OutputPort> outputs,
        std::span<EventInputPort> event_inputs = {},
        std::span<EventOutputPort> event_outputs = {}
    )
    {
        return TickContext<NestedNode> {
            .inputs = inputs,
            .outputs = outputs,
            .event_inputs = event_inputs,
            .event_outputs = event_outputs,
            .scc_feedback_latency = outer.scc_feedback_latency,
            .buffer = remaining_buffer(outer.buffer, nested_state),
        };
    }

    template<typename Node>
    IV_FORCEINLINE void do_tick(Node const& node, TickSampleContext<Node> const& ctx)
    {
        if constexpr (details::has_tick<Node>)
        {
            node.tick(ctx);
            advance_inputs(ctx.inputs, 1);
        }
        else if constexpr (details::has_tick_block<Node>)
        {
            do_tick_block(node, {
                static_cast<TickContext<Node> const&>(ctx),
                ctx.index,
                1,
            });
        }
        else
        {
            static_assert(details::has_tick<Node> || details::has_tick_block<Node>, "node must implement tick() or tick_block()");
        }
    }

    template<typename Node>
    IV_FORCEINLINE void do_tick_block(Node const& node, TickBlockContext<Node> const& ctx)
    {
        if (ctx.block_size == 0) {
            return;
        }
        validate_block_size(ctx.block_size);

        if constexpr (details::has_tick_block<Node>)
        {
            node.tick_block(ctx);
            advance_inputs(ctx.inputs, ctx.block_size);
        }
        else
        {
            for (size_t i = 0; i < ctx.block_size; ++i) {
                do_tick(node, {
                    static_cast<TickContext<Node> const&>(ctx),
                    ctx.index + i,
                });
            }
        }
    }

    template<typename Node>
    IV_FORCEINLINE void do_skip_block(Node const& node, SkipBlockContext<Node> const& ctx)
    {
        if (ctx.block_size == 0) {
            return;
        }
        validate_block_size(ctx.block_size);

        if constexpr (details::has_skip_block<Node>)
        {
            node.skip_block(ctx);
        }
        else
        {
            for (auto& output : ctx.outputs) {
                output.push_silence(ctx.block_size);
            }
        }

        advance_inputs(ctx.inputs, ctx.block_size);
    }
}
