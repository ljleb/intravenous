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
inline constexpr std::size_t no_sample_transient_allocation =
    std::numeric_limits<std::size_t>::max();

// One compiler-visible physical sample representation. Node API facades are
// reconstructed from these immutable facts and never become persistent graph
// objects. Derived fanout representations extend this indirection without changing
// primitive binding identity.
struct SampleRepresentationPlan {
    std::size_t producer_group_index = 0;
    bool canonical_producer_representation = true;
    SampleConnectionImplementationKind implementation =
        SampleConnectionImplementationKind::direct;
    ChannelLayout channel_layout{};
    std::size_t frame_capacity = 0;
    ConnectionLiveIntervalPlan live_interval{};

    // direct/transient_materialization currently use an exact byte-range in the
    // compile-time transient arena. Future retained/ring implementations can
    // point at persistent storage instead without changing primitive bindings.
    std::size_t transient_allocation = no_sample_transient_allocation;
};

struct SampleProducerPhysicalPlan {
    std::size_t canonical_representation = no_sample_representation;
};

// One explicit post-producer transformation from a canonical/derived sample
// representation into another transient representation. The operation is
// scheduled immediately after the producer execution position and is emitted
// directly into whole-project LLVM; it is not hidden inside OutputPort.
struct SampleMaterializationPlan {
    std::size_t source_representation = no_sample_representation;
    std::size_t target_representation = no_sample_representation;
    std::size_t after_execution_position = 0;
    ChannelLayout source_layout{};
    ChannelLayout target_layout{};
};

// Exact transient byte range assigned to one representation. Ranges may overlap
// iff their inclusive schedule live intervals do not overlap. There is no
// runtime slot object or allocator metadata.
struct SampleTransientAllocationPlan {
    std::size_t representation_index = no_sample_representation;
    std::size_t size_bytes = 0;
    std::size_t alignment = alignof(Sample);
    std::size_t region_relative_offset = 0;

    // Assigned after canonical NodeLayout finalization.
    std::size_t storage_offset = 0;
};

struct SamplePhysicalPlan {
    // Indexed by ConnectionAnalysisPlan::sample_producer_groups.
    std::vector<std::optional<SampleProducerPhysicalPlan>> producer_groups{};
    // Indexed by physical representation handle.
    std::vector<SampleRepresentationPlan> representations{};
    // Indexed by ConnectionAnalysisPlan::sample_connections. Identity fanout
    // branches resolve to the producer's canonical representation; converted
    // branches resolve to a shared derived representation when their static
    // transformation is identical.
    std::vector<std::optional<std::size_t>> connection_representations{};

    // Explicit conversion/materialization operations. These are scheduled after
    // the source producer and before every consumer bound to the target
    // representation.
    std::vector<SampleMaterializationPlan> materializations{};

    // One exact range per currently-transient representation. The arena high
    // water mark is independent of any individual representation's maximum size.
    std::vector<SampleTransientAllocationPlan> transient_allocations{};
    std::size_t transient_arena_size = 0;
    std::size_t transient_arena_alignment = 1;
    NodeLayout::RegionHandle transient_region{};

    [[nodiscard]] bool empty() const noexcept
    {
        return representations.empty();
    }
};

// Pure host-side physical-representation planning. This consumes already-made
// choose_sample_connection_implementation() decisions; it does not duplicate
// policy. Direct/transient canonical and whole-port converted fanout branches are
// realized here; the representation indirection remains the stable seam for
// history/rings and future channel-projection transforms.
std::expected<SamplePhysicalPlan, std::string> build_sample_physical_plan(
    ConnectionAnalysisPlan const& connections,
    std::size_t kernel_block_size);

// Reserve one compiler-owned canonical NodeStorage arena for all transient byte
// ranges. No initializer is registered because this is payload scratch, not C++
// object state. Final absolute offsets are filled by
// finalize_sample_physical_storage().
std::expected<void, std::string> declare_sample_physical_storage(
    NodeLayoutBuilder& builder,
    SamplePhysicalPlan& plan);

std::expected<void, std::string> finalize_sample_physical_storage(
    NodeLayout const& layout,
    SamplePhysicalPlan& plan);

} // namespace iv::graph_jit::detail
