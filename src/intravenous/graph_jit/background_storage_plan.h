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

} // namespace iv::graph_jit::detail
