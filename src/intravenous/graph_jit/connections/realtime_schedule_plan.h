#pragma once

#include <intravenous/graph/port_ids.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iv {
struct ConfiguredGraph;
}

namespace iv::graph_jit::detail {

struct ConnectionAnalysisPlan;

// Front 2 starts with a small fixed cap so native batch adapters cannot turn a
// large ready frontier into an unbounded callback stack frame. Front 16 owns
// measurement-driven sizing and joint stack/layout costing.
inline constexpr std::size_t initial_callback_batch_lane_limit = 8;

struct SccRegionPlan {
    std::vector<NodeBundleHandle> nodes{};
    // Explicit (non-detached) dependencies remain acyclic. Cyclic regions are
    // created only by restoring validated detached source->target dependencies,
    // so this order is a deterministic topological order with those temporal
    // feedback dependencies omitted.
    std::vector<NodeBundleHandle> execution_order{};
    bool cyclic = false;
    bool run_ahead = false;
    std::size_t maximum_block_size = 0;
    // Scheduling quantum exposed to callbacks in this region. This is derived
    // from node block limits and internal detach latencies; it is not the
    // transport delay of any particular detach.
    std::size_t scc_feedback_latency = 0;
};

struct SchedulePlan {
    std::vector<SccRegionPlan> regions{};
    std::vector<std::size_t> region_order{};
    // Each entry is one snapshot of mutually ready SCC regions. A multi-entry
    // group contains either compatible acyclic one-node regions or compatible
    // cyclic regions whose corresponding execution-order entries have equal
    // callback batch classes. Later lowering may keep the group scalar, but it
    // need not rediscover whether its members form an antichain.
    std::vector<std::vector<std::size_t>> ready_groups{};
    // Ready groups and flattened execution positions have the same boundary.
    // Finish the early groups before starting the foreground groups.
    std::size_t early_ready_groups = 0;
    std::size_t early_execution_steps = 0;
    // Entry positions correspond to configured NodeBundleHandle values.
    // Boundary/non-concrete bundles have no region.
    std::vector<std::optional<std::size_t>> bundle_to_region{};
    // Entry positions correspond to configured NodeBundleHandle values. Values
    // are positions in the flattened provisional execution schedule. For a
    // batched cyclic group, positions are phase-major across its regions.
    std::vector<std::optional<std::size_t>> bundle_execution_position{};
    // Entry positions correspond to configured NodeBundleHandle values. A set
    // entry means planning proved that the node's Tick result is immutable and
    // omitted its callback before constructing the schedule.
    std::vector<std::uint8_t> callback_elided{};

    // Raw outputs crossing from an early SCC to a foreground consumer.
    // One entry per source even when several connections read that source.
    // The normalized routes still own conversion, history and read latency.
    std::vector<SampleOutputChannelId> early_sample_outputs{};
    std::vector<EventOutputPortId> early_event_outputs{};
};

// Assign Tick callback order and feedback regions, then align feed-forward
// read latencies in the existing Sample and Event connections.
std::expected<void, std::string> plan_realtime_schedule(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan,
    std::span<std::size_t const> callback_batch_classes,
    std::span<NodeBundleHandle const> callback_elision_candidates);

} // namespace iv::graph_jit::detail
