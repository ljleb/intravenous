#pragma once

#include <intravenous/graph_jit/connection_plan.h>

namespace iv::graph_jit::detail {

// Partition ports by their uses, then group Tick producers and record each
// group's live interval and working and retained storage requirements.
std::expected<void, std::string> plan_port_groups(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& realization,
    ConnectionAnalysisPlan& plan,
    std::size_t kernel_block_size);

} // namespace iv::graph_jit::detail
