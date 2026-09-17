#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/graph_jit/connection_plan.h>
#include <intravenous/node/layout.h>

#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace iv::graph_jit::detail {

inline constexpr std::size_t no_sample_representation =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t no_sample_transient_slot =
    std::numeric_limits<std::size_t>::max();

// One compiler-visible physical sample representation. Node API facades are
// reconstructed from these immutable facts and never become persistent graph
// objects. Point 8 can add derived branch representations without changing
// primitive binding identity.
struct SampleRepresentationPlan {
    std::size_t producer_group_index = 0;
    bool canonical_producer_representation = true;
    SampleConnectionImplementationKind implementation =
        SampleConnectionImplementationKind::direct;
    ChannelLayout channel_layout{};
    std::size_t frame_capacity = 0;
    ConnectionLiveIntervalPlan live_interval{};

    // direct/transient_materialization currently use a reusable transient slot.
    // Future retained/ring implementations can point at persistent storage
    // instead without changing primitive bindings.
    std::size_t transient_slot = no_sample_transient_slot;
};

struct SampleProducerPhysicalPlan {
    std::size_t canonical_representation = no_sample_representation;
};

// Physical scratch slot shared by sample representations whose inclusive
// schedule live intervals do not overlap. The slot is raw sample bytes only;
// it has no façade/cursor/lifecycle object.
struct SampleTransientSlotPlan {
    std::size_t size_bytes = 0;
    std::size_t alignment = alignof(Sample);
    std::vector<std::size_t> representations{};

    // Assigned during canonical NodeLayout declaration/finalization.
    std::size_t region_relative_offset = 0;
    std::size_t storage_offset = 0;
};

struct SamplePhysicalPlan {
    // Indexed by ConnectionAnalysisPlan::sample_producer_groups.
    std::vector<std::optional<SampleProducerPhysicalPlan>> producer_groups{};
    // Indexed by physical representation handle.
    std::vector<SampleRepresentationPlan> representations{};
    // Indexed by ConnectionAnalysisPlan::sample_connections. Identity branches
    // currently resolve to the producer's canonical representation; point 8 may
    // instead resolve selected connections to derived materialized branches.
    std::vector<std::optional<std::size_t>> connection_representations{};
    std::vector<SampleTransientSlotPlan> transient_slots{};
    NodeLayout::RegionHandle transient_region{};

    [[nodiscard]] bool empty() const noexcept
    {
        return representations.empty();
    }
};

// Pure host-side physical-representation planning. This consumes already-made
// choose_sample_connection_implementation() decisions; it does not duplicate
// policy. Point 7 intentionally realizes only direct/transient groups, but the
// representation indirection is the stable seam for fanout/history/rings.
std::expected<SamplePhysicalPlan, std::string> build_sample_physical_plan(
    ConnectionAnalysisPlan const& connections,
    std::size_t kernel_block_size);

// Reserve compiler-owned canonical NodeStorage for all reusable transient slots.
// No initializer is registered because this is payload scratch, not C++ object
// state. Final absolute offsets are filled by finalize_sample_physical_storage().
std::expected<void, std::string> declare_sample_physical_storage(
    NodeLayoutBuilder& builder,
    SamplePhysicalPlan& plan);

std::expected<void, std::string> finalize_sample_physical_storage(
    NodeLayout const& layout,
    SamplePhysicalPlan& plan);

} // namespace iv::graph_jit::detail
