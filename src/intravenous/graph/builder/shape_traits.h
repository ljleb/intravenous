#pragma once

#include <intravenous/node/traits.h>

#include <type_traits>

namespace iv::details {
    template<typename Node>
    inline constexpr bool should_preserve_node_type_v =
        has_fixed_num_inputs_v<std::remove_cvref_t<Node>>
        || has_fixed_num_outputs_v<std::remove_cvref_t<Node>>
        || has_fixed_num_event_inputs_v<std::remove_cvref_t<Node>>
        || has_fixed_num_event_outputs_v<std::remove_cvref_t<Node>>;
}
