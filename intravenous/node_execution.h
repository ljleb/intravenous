#pragma once

#include "node_traits.h"

#include <memory>
#include <span>

namespace iv {
    template<typename Node>
    struct TickContext {
        std::span<InputPort> inputs;
        std::span<OutputPort> outputs;
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
    }

    template<typename Node>
    std::add_lvalue_reference_t<typename TickContext<Node>::State> TickContext<Node>::state() const
    requires(!std::is_void_v<State>)
    {
        void* ptr = buffer.data();
        size_t space = buffer.size();
        return *reinterpret_cast<State*>(std::align(alignof(State), sizeof(State), ptr, space));
    }

    template<typename Node>
    TickSampleContext<Node>::TickSampleContext(TickContext<Node> base, size_t index)
    : TickContext<Node>(base), index(index)
    {}

    template<typename Node>
    TickBlockContext<Node>::TickBlockContext(
        TickContext<Node> base,
        size_t index,
        size_t block_size
    )
    : TickContext<Node>(base)
    , index(index)
    , block_size(block_size)
    {}

    template<typename Node>
    void do_tick_block(Node& node, TickBlockContext<Node> const& state);

    template<typename Node>
    void do_tick(Node& node, TickSampleContext<Node> const& ctx)
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
    void do_tick_block(Node& node, TickBlockContext<Node> const& ctx)
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
}
