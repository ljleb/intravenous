#pragma once

// Runtime-owned data frame consumed by the generated indexed graph programs.
// The frame contains no topology: node ordinals and traversal order are fixed in
// generated code from IndexedPlan. GraphExecutor owns every pointed-to coverage,
// page, port, and accumulator object for the duration of one logical batch.

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/indexed_coverage.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace iv::graph_jit {

enum class IndexedNodeBatchActivity : std::uint8_t {
    none = 0,
    forward = 1 << 0,
    reverse = 1 << 1,
    evaluate = 1 << 2,
};

constexpr IndexedNodeBatchActivity operator|(
    IndexedNodeBatchActivity left,
    IndexedNodeBatchActivity right) noexcept
{
    using Value = std::underlying_type_t<IndexedNodeBatchActivity>;
    return static_cast<IndexedNodeBatchActivity>(
        static_cast<Value>(left) | static_cast<Value>(right));
}

constexpr bool has_activity(
    IndexedNodeBatchActivity value,
    IndexedNodeBatchActivity activity) noexcept
{
    using Value = std::underlying_type_t<IndexedNodeBatchActivity>;
    return (static_cast<Value>(value) & static_cast<Value>(activity)) != 0;
}

using SynthesizedForwardCoverageFunction = void (*)(
    void*, ReflectedNodeForwardCoverageContext const&);
using SynthesizedReverseCoverageFunction = void (*)(
    void*, ReflectedNodeReverseCoverageContext const&);

struct IndexedNodeBatchFrame {
    IndexedNodeBatchActivity activity = IndexedNodeBatchActivity::none;

    // Authored Tock nodes consume these reflected one-node contexts directly.
    // A synthesized pointwise replay node consumes the same accumulator-shaped
    // F/R contexts through the uniform thunks below. GraphExecutor may use one
    // shared generic thunk for every such node.
    ReflectedNodeForwardCoverageContext forward {};
    ReflectedNodeReverseCoverageContext reverse {};
    ReflectedNodeTockCoverageContext tock {};

    void* synthesized_context = nullptr;
    SynthesizedForwardCoverageFunction synthesized_forward = nullptr;
    SynthesizedReverseCoverageFunction synthesized_reverse = nullptr;

    // Replay reuses the ordinary imported tick_block wrapper. The executor
    // supplies isolated invocation-local bindings for each canonical requested
    // block; no live realtime State or buffers may be bound here.
    ReflectedNodeTickContext replay {};
    ReflectedSpan<IndexedRegion const> replay_regions {};
};

struct IndexedBatchFrame {
    // Dense IndexedPlan node-ordinal order. Generated roots reject a frame that
    // is shorter than their immutable plan before dereferencing any node entry.
    ReflectedSpan<IndexedNodeBatchFrame> nodes {};
};

static_assert(std::is_standard_layout_v<IndexedNodeBatchFrame>);
static_assert(std::is_trivially_copyable_v<IndexedNodeBatchFrame>);
static_assert(std::is_standard_layout_v<IndexedBatchFrame>);
static_assert(std::is_trivially_copyable_v<IndexedBatchFrame>);

} // namespace iv::graph_jit
