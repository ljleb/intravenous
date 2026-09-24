#pragma once

#include <intravenous/graph_jit/connection_plan.h>

#include <expected>
#include <string>

namespace iv::graph_jit::detail {

// Select immutable source views and derived materializations for every
// background or mixed-domain connection. No payload memory is allocated here;
// GraphExecutor later realizes these records for a requested range/version.
std::expected<IndexedPhysicalPlan, std::string> build_indexed_physical_plan(
    ConnectionAnalysisPlan const& connections);

} // namespace iv::graph_jit::detail
