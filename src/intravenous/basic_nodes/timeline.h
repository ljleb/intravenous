#pragma once

#include <intravenous/graph/builder.h>

namespace iv {
struct Timeline {
    void build_graph(GraphBuilder& graph) const
    {
        graph.event_input<"beats">(EventTypeId::beat, InputAccess::coverage);
        graph.outputs();
    }
};
} // namespace iv
