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
        ReflectedSampleOutputPortBinding const* output,
        std::size_t sample_index,
        std::size_t block_size) noexcept;

    void* context = nullptr;
    Capture capture = nullptr;
};

struct TickEventCaptureOperation {
    using Capture = void (*)(
        void* context,
        ReflectedEventOutputPortBinding const* output,
        std::size_t sample_index,
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
    ReflectedSpan<TickSampleCaptureOperation const> sample_captures{};
    ReflectedSpan<TickEventCaptureOperation const> event_captures{};
};

static_assert(std::is_standard_layout_v<TickInvocationCall>);
static_assert(std::is_trivially_copyable_v<TickInvocationCall>);

} // namespace iv::graph_jit
