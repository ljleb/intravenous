#pragma once

#include "node_layout.h"
#include "node_execution.h"

namespace iv {
    template<typename Node, typename Ctx>
    constexpr void do_declare(Node const& node, Ctx& ctx)
    {
        DeclarationContext<Node> node_ctx(ctx, node);
        if constexpr (details::has_declare<Node>)
        {
            node.declare(node_ctx);
        }
    }

    template<typename Node, typename Ctx>
    constexpr void do_initialize(Node const& node, Ctx& ctx)
    {
        InitializationContext<Node> node_ctx(ctx);
        if constexpr (details::has_initialize<Node>)
        {
            node.initialize(node_ctx);
        }
    }

    template<typename Node, typename Ctx>
    constexpr void do_release(Node const& node, Ctx& ctx)
    {
        ReleaseContext<Node> node_ctx(ctx);
        if constexpr (details::has_release<Node>)
        {
            node.release(node_ctx);
        }
    }

    template<typename Node, typename Ctx>
    constexpr void do_move(Node const& node, Ctx& ctx)
    {
        MoveContext<Node> node_ctx(ctx);
        if constexpr (details::has_move<Node>)
        {
            node.move(node_ctx);
        }
    }
}
