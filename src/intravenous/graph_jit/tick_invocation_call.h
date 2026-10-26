#pragma once

#include <intravenous/graph/reflected_node_operations.h>

#include <cstddef>
#include <type_traits>

namespace iv::graph_jit {

// Runtime-resolved capture operations. Generated code may invoke the function
// with the matching finalized output binding, but cannot recover the executor,
// capture store, persisted identity, or transaction owner from the opaque
// context. Null callbacks are legal while a realization has no capture sink.
struct TickSampleCaptureOperation {
    using Capture = void (*)(
        void* context,
        ReflectedContinuousOutputPortBinding const* output,
        GlobalIndex sample_index,
        std::size_t block_size) noexcept;

    void* context = nullptr;
    Capture capture = nullptr;
};

struct TickEventCaptureOperation {
    using Capture = void (*)(
        void* context,
        ReflectedEventOutputPortBinding const* output,
        GlobalIndex sample_index,
        std::size_t block_size) noexcept;

    void* context = nullptr;
    Capture capture = nullptr;
};

static_assert(std::is_standard_layout_v<TickSampleCaptureOperation>);
static_assert(std::is_trivially_copyable_v<TickSampleCaptureOperation>);
static_assert(std::is_standard_layout_v<TickEventCaptureOperation>);
static_assert(std::is_trivially_copyable_v<TickEventCaptureOperation>);

// Narrow generated-root ABI for storage selected at one Tick callback boundary.
// The owning runtime frame keeps every backing snapshot alive; generated code
// receives only already-resolved views and cannot recover an executor, persisted-data
// store, or materialization owner from this record. When a compiled graph has a
// non-empty kind-specific Tick runtime slot array, its invocation must provide a
// non-null span with exactly that array's extent. TickInvocationWorkspace validates
// the retained slot plan when it is constructed and always returns those complete
// spans. The generated realtime root relies on this precondition and does not repeat
// pointer or extent checks per node.
struct TickInvocationCall {
    ReflectedSpan<ReflectedContinuousInputPortBinding const>
        sequential_sample_inputs{};
    ReflectedSpan<ReflectedEventInputPortBinding const>
        sequential_event_inputs{};
    ReflectedSpan<RandomAccessContinuousInputPort const>
        random_access_sample_inputs{};
    ReflectedSpan<RandomAccessEventInputPort const>
        random_access_event_inputs{};
    ReflectedSpan<TickSampleCaptureOperation const> sample_captures{};
    ReflectedSpan<TickEventCaptureOperation const> event_captures{};
};

static_assert(std::is_standard_layout_v<TickInvocationCall>);
static_assert(std::is_trivially_copyable_v<TickInvocationCall>);

} // namespace iv::graph_jit
