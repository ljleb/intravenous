#pragma once

#include <intravenous/global_index.h>

#include <cstddef>
#include <expected>
#include <memory>
#include <string>

namespace iv::graph_jit {
struct LoweringInput;
struct TickInvocationCall;
namespace detail {
struct LoweringPlan;
}

// Immutable graph-dependent data of one realtime root realization. The root's
// code, execute_realtime_root(), is part of the application binary, so a graph
// edit builds a new RealtimeRootPlan and compiles no realtime code. The
// definition is private to realtime_root.cpp.
struct RealtimeRootPlan;

// Builds the realtime root that executes plan. The root takes ownership of the
// plan sections that only it reads: sample_ports, event_ports, execution,
// root_stack, declarations.primitive_activity, and
// declarations.activity_frontiers. Every other section stays in plan for the
// generated background roots and LoweringOutput. Every plan invariant the root
// relies on is checked here, so a root call checks no plan structure.
std::expected<std::shared_ptr<RealtimeRootPlan const>, std::string>
build_realtime_root_plan(LoweringInput const& input, detail::LoweringPlan& plan);

// Executes one root block of plan, which is never null. block_size > 0. If
// the plan has Tick runtime slots, invocation is non-null and carries the
// complete kind-specific spans described by
// BackgroundEvaluationPlan::tick_runtime.
void execute_realtime_root(
    RealtimeRootPlan const* plan,
    std::byte* storage_base,
    TickInvocationCall const* invocation,
    GlobalIndex sample_index,
    std::size_t block_size) noexcept;
} // namespace iv::graph_jit
