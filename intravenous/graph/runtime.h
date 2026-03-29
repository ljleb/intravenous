#pragma once

#include "../basic_nodes/type_erased.h"
#include "../node_tick.h"

namespace iv {
    template<typename OuterNode>
    inline void tick_nested_node(
        TypeErasedNode const& node,
        std::byte* nested_state,
        TickBlockContext<OuterNode> const& outer,
        std::span<InputPort> inputs,
        std::span<OutputPort> outputs
    )
    {
        node.tick_block({
            make_nested_tick_context<TypeErasedNode>(
                static_cast<TickContext<OuterNode> const&>(outer),
                nested_state,
                inputs,
                outputs
            ),
            outer.index,
            outer.block_size
        });
    }
}
