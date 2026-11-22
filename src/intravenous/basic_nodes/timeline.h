#pragma once

#include <intravenous/graph/builder.h>
#include <intravenous/graph/programmatic_inputs.h>

namespace iv {
struct Timeline {
    void build_graph(GraphBuilder& graph) const
    {
        iv::programmatic::declare_event_input(graph, "beats", EventTypeId::beat, InputAccess::coverage);
        graph.outputs();
    }
};
} // namespace iv
