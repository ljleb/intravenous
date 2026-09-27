#pragma once

#include <intravenous/graph/reflected_node_operations.h>

#include <type_traits>

namespace iv::graph_jit {

// Narrow generated-root ABI for storage selected at one Tick callback boundary.
// The owning runtime frame keeps every backing snapshot alive; generated code
// receives only already-resolved views and cannot recover an executor, page
// store, or materialization owner from this record.
struct TickInvocationCall {
    ReflectedSpan<ReflectedSampleInputPortBinding const>
        sequential_sample_inputs{};
    ReflectedSpan<ReflectedEventInputPortBinding const>
        sequential_event_inputs{};
    ReflectedSpan<RandomAccessSampleInputPort const>
        random_access_sample_inputs{};
    ReflectedSpan<RandomAccessEventInputPort const>
        random_access_event_inputs{};
};

static_assert(std::is_standard_layout_v<TickInvocationCall>);
static_assert(std::is_trivially_copyable_v<TickInvocationCall>);

} // namespace iv::graph_jit
