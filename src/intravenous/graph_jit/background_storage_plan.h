#pragma once

#include <intravenous/graph_jit/connection_plan.h>

#include <expected>
#include <string>

namespace iv::graph_jit::detail {

// Select immutable source views and derived materializations for every
// background or mixed-domain connection. No data memory is allocated here;
// GraphExecutor later realizes these records for a requested range/version.
std::expected<BackgroundStoragePlan, std::string> build_background_storage_plan(
    ConnectionAnalysisPlan const& connections);

// Retain the compact runtime binding/operation/replay schedule over the
// already-selected storage plan, then validate every retained index and order.
std::expected<void, std::string> finalize_background_runtime_plan(
    BackgroundEvaluationPlan& plan);

std::expected<void, std::string> validate_background_runtime_plan(
    BackgroundEvaluationPlan const& plan);

// Retain compact per-node Tick input-binding and persisted-output capture ranges
// over the same immutable storage plan. Runtime realization never rediscovers
// topology.
std::expected<void, std::string> finalize_tick_runtime_plan(
    BackgroundEvaluationPlan& plan);

std::expected<void, std::string> validate_tick_runtime_plan(
    BackgroundEvaluationPlan const& plan);

} // namespace iv::graph_jit::detail
