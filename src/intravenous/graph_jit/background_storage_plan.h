#pragma once

#include <intravenous/graph_jit/connection_plan.h>

#include <expected>
#include <optional>
#include <string>

namespace iv::graph_jit::detail {

// A control/background-path storage bound. Finite authored counts survive
// conversions and fan-in; other sources keep their declared density bound.
struct BackgroundEventCountBound {
    std::size_t count = 0;
    bool uses_finite_count = false;
};
[[nodiscard]] std::optional<BackgroundEventCountBound> background_event_count_bound(
    BackgroundStoragePlan const& plan, PortStorageIndex storage,
    std::size_t window_size);

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

// A Random Access input that no BackgroundConnectionPlan targets has empty
// coverage. Its runtime binding is the only input binding without storage.
[[nodiscard]] bool is_disconnected_random_access_input(
    BackgroundEvaluationPlan const& plan,
    BackgroundPortIndex port);

// Retain compact per-node Tick input-binding and persisted-output capture ranges
// over the same immutable storage plan. Runtime realization never rediscovers
// topology.
std::expected<void, std::string> finalize_tick_runtime_plan(
    BackgroundEvaluationPlan& plan,
    SchedulePlan const& schedule,
    std::size_t maximum_block_size);

std::expected<void, std::string> validate_tick_runtime_plan(
    BackgroundEvaluationPlan const& plan);

} // namespace iv::graph_jit::detail
