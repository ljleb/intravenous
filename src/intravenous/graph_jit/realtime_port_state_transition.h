#pragma once

#include <intravenous/graph_jit/realtime_port_state_plan.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace iv::graph_jit {

// One surviving semantic port-state piece across a graph-generation boundary.
// Indices refer to the immutable metadata of the predecessor/current compiled
// graphs. The inherited interval is relative to the cutover position: history
// occupies a negative interval ending at zero, while authored output latency
// occupies a non-negative interval beginning at zero.
struct RealtimePortStateTransition {
    std::size_t previous_requirement_index = 0;
    std::size_t current_requirement_index = 0;
    std::size_t previous_realization_index = 0;
    std::size_t current_realization_index = 0;
    std::int64_t inherited_begin = 0;
    std::int64_t inherited_end = 0;
    std::size_t inherited_extent_samples = 0;
    std::size_t newly_exposed_extent_samples = 0;
    std::size_t discarded_extent_samples = 0;
};

// Cold semantic reconciliation for one prospective generation cutover. This
// intentionally contains no raw-region transfers: physical transition
// realization consumes these matches after stable node-port ownership has
// already determined what must survive.
struct RealtimePortStateTransitionPlan {
    std::vector<RealtimePortStateTransition> sample_states{};
    std::vector<RealtimePortStateTransition> event_states{};
};

[[nodiscard]] std::expected<RealtimePortStateTransitionPlan, std::string>
plan_realtime_port_state_transition(
    RealtimePortStateRequirements const& previous_requirements,
    RealtimePortStateRealizations const& previous_realizations,
    RealtimePortStateRequirements const& current_requirements,
    RealtimePortStateRealizations const& current_realizations);

} // namespace iv::graph_jit
