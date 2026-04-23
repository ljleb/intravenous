#pragma once

#include "basic_nodes/type_erased.h"
#include "node/tick.h"

namespace iv {
    template<typename OuterNode>
    IV_FORCEINLINE void tick_nested_node(
        TypeErasedNode const& node,
        std::byte* nested_state,
        TickBlockContext<OuterNode> const& outer,
        std::span<InputPort> inputs,
        std::span<OutputPort> outputs,
        std::span<EventInputPort> event_inputs = {},
        std::span<EventOutputPort> event_outputs = {}
    )
    {
        node.tick_block({
            make_nested_tick_context<TypeErasedNode>(
                static_cast<TickContext<OuterNode> const&>(outer),
                nested_state,
                inputs,
                outputs,
                event_inputs,
                event_outputs
            ),
            outer.index,
            outer.block_size
        });
    }
}
