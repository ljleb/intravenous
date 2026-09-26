#pragma once

// Runtime-owned data frame consumed by the generated background graph programs.
// The call contains no topology: node indices and traversal order are fixed in
// generated code from BackgroundEvaluationPlan. GraphExecutor owns every
// referenced coverage, page, port, and accumulator for the duration of the call.

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/coverage.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace iv::graph_jit {

enum class BackgroundNodeActivity : std::uint8_t {
    none = 0,
    forward = 1 << 0,
    reverse = 1 << 1,
    evaluate = 1 << 2,
};

constexpr BackgroundNodeActivity operator|(
    BackgroundNodeActivity left,
    BackgroundNodeActivity right) noexcept
{
    using Value = std::underlying_type_t<BackgroundNodeActivity>;
    return static_cast<BackgroundNodeActivity>(
        static_cast<Value>(left) | static_cast<Value>(right));
}

constexpr bool has_activity(
    BackgroundNodeActivity value,
    BackgroundNodeActivity activity) noexcept
{
    using Value = std::underlying_type_t<BackgroundNodeActivity>;
    return (static_cast<Value>(value) & static_cast<Value>(activity)) != 0;
}

using ReplayForwardCoverageFunction = void (*)(
    void*, ReflectedNodeForwardCoverageContext const&);
using ReplayReverseCoverageFunction = void (*)(
    void*, ReflectedNodeReverseCoverageContext const&);

struct BackgroundNodeCall {
    BackgroundNodeActivity activity = BackgroundNodeActivity::none;

    // Authored Tock nodes consume these reflected one-node contexts directly.
    // A pointwise replay node consumes the same accumulator-shaped F/R contexts
    // through the callbacks below.
    ReflectedNodeForwardCoverageContext forward {};
    ReflectedNodeReverseCoverageContext reverse {};
    ReflectedNodeTockCoverageContext tock {};

    void* replay_context = nullptr;
    ReplayForwardCoverageFunction replay_forward = nullptr;
    ReplayReverseCoverageFunction replay_reverse = nullptr;

    // Replay reuses the ordinary imported tick_block wrapper. The executor
    // supplies isolated invocation-local port storage for each requested
    // block; no live realtime State or buffers may be bound here.
    ReflectedNodeTickContext replay {};
    ReflectedSpan<IndexRegion const> replay_regions {};
};

struct BackgroundEvaluationCall {
    // Dense BackgroundEvaluationPlan node-index order.
    ReflectedSpan<BackgroundNodeCall> nodes {};
};

static_assert(std::is_standard_layout_v<BackgroundNodeCall>);
static_assert(std::is_trivially_copyable_v<BackgroundNodeCall>);
static_assert(std::is_standard_layout_v<BackgroundEvaluationCall>);
static_assert(std::is_trivially_copyable_v<BackgroundEvaluationCall>);

} // namespace iv::graph_jit
