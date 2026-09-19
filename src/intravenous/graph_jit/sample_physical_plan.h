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
inline constexpr std::size_t no_sample_producer_group =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t no_sample_transient_allocation =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t no_sample_persistent_allocation =
    std::numeric_limits<std::size_t>::max();

// One compiler-visible physical sample representation. Node API facades are
// reconstructed from these immutable facts and never become persistent graph
// objects. A representation may use transient storage, a persistent ring, or
// transient working storage backed by compact persistent carry state.
struct SampleRepresentationPlan {
    std::size_t producer_group_index = 0;
    bool canonical_producer_representation = true;
    SampleConnectionImplementationKind implementation =
        SampleConnectionImplementationKind::direct;
    ChannelLayout channel_layout{};
    std::size_t frame_capacity = 0;
    ConnectionLiveIntervalPlan live_interval{};

    std::size_t transient_allocation = no_sample_transient_allocation;
    std::size_t persistent_allocation = no_sample_persistent_allocation;
};

struct SampleProducerPhysicalPlan {
    std::size_t canonical_representation = no_sample_representation;
};

// One explicit post-producer transformation from a canonical/derived sample
// representation into another transient representation. History/latency are
// part of the materialization window rather than mutable OutputPort state.
struct SampleMaterializationPlan {
    std::size_t source_representation = no_sample_representation;
    std::size_t target_representation = no_sample_representation;
    std::size_t after_execution_position = 0;
    // Feedback conversion is consumer-driven rather than producer-driven: in a
    // cyclic region the consumer may precede the producer in deterministic
    // execution order, so the delayed window must be materialized from the
    // persistent feedback ring immediately before that consumer executes.
    std::optional<std::size_t> before_execution_position{};
    ChannelLayout source_layout{};
    ChannelLayout target_layout{};

    // Shared converted fanout materializes the union of every consumer window.
    // retained_before is the distance from the current sample index to the
    // earliest frame any consumer can address. latest_read_latency is the
    // smallest effective read latency, hence the latest frame any consumer
    // needs from the current block. For one consumer these reduce to
    // target_history + read_latency and read_latency respectively.
    std::size_t retained_before = 0;
    std::size_t latest_read_latency = 0;
};

struct SampleCompositionSourcePlan {
    std::size_t source_representation = no_sample_representation;
    std::size_t source_channel = 0;
    std::size_t target_channel = 0;
    std::size_t read_latency = 0;
};

// A composed connection gathers independently-timed channels from canonical
// producer representations into one target-layout representation. Feed-forward
// composition writes a timestamp-aligned transient window, so its eventual
// InputPort binding reads with zero additional latency and target history is
// reconstructed while composition runs. Detached composition instead owns a
// persistent initialized ring and shifts current source writes forward by each
// channel's read latency; its InputPort applies only the common detach delay.
struct SampleCompositionPlan {
    std::size_t connection_index = 0;
    std::vector<SampleCompositionSourcePlan> sources{};
    std::size_t target_representation = no_sample_representation;
    std::size_t after_execution_position = 0;
    ChannelLayout target_layout{};
    std::size_t target_history = 0;

    // Detached composition stores one persistent target-layout timeline without
    // first materializing a transient aggregate. Each producer channel writes
    // its current source frame at target absolute index source + read_latency.
    // This preserves per-channel path latency while leaving every unwritten
    // pre-roll frame at the authored detach initial value. The eventual input
    // binding therefore needs only the common detach latency.
    bool shift_writes_by_read_latency = false;
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

enum class SamplePersistentStorageKind {
    compact_carry,
    ring,
};

// Persistent connection state is always represented by canonical raw
// NodeStorage. compact_carry stores exactly retained_frames; ring stores the
// whole power-of-two working ring. migration_identity is non-empty so exact-
// shape NodeStorage migration preserves connection history across generations.
struct SamplePersistentAllocationPlan {
    std::size_t representation_index = no_sample_representation;
    SamplePersistentStorageKind kind = SamplePersistentStorageKind::ring;
    ChannelLayout channel_layout{};
    std::size_t retained_frames = 0;
    std::size_t frame_capacity = 0;
    std::size_t size_bytes = 0;
    std::size_t alignment = alignof(Sample);
    std::string migration_identity{};
    std::optional<Sample> initialize_value{};
    NodeLayout::RegionHandle region{};

    // Assigned after canonical NodeLayout finalization.
    std::size_t storage_offset = 0;
};

// compact_persistent_carry uses a transient absolute-indexed working ring while
// a minimal persistent tail crosses root invocations. The tail is restored
// immediately before the producer and committed after its materializations.
struct SampleCarryOperationPlan {
    std::size_t representation_index = no_sample_representation;
    std::size_t persistent_allocation = no_sample_persistent_allocation;
    std::size_t producer_execution_position = 0;
    std::size_t retained_frames = 0;
};

// One fallback branch-local delayed copy of a canonical producer representation.
// Compatible feedback may instead make the persistent ring the producer's
// canonical home and needs no operation here. A copied ring is indexed in the
// same absolute sample timeline as the source; execution lowering copies each
// produced slice into it. Consumer bindings add loop_extra_latency to the
// source's ordinary read latency, while the ring retains that full delay plus
// target history. initial_value defines every unproduced frame observed before
// the delayed source timeline reaches zero.
struct SampleFeedbackOperationPlan {
    std::size_t source_representation = no_sample_representation;
    std::size_t ring_representation = no_sample_representation;
    std::size_t producer_execution_position = 0;
    std::size_t loop_extra_latency = 1;
    Sample initial_value{};
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
    std::vector<SampleCompositionPlan> compositions{};
    std::vector<SampleCarryOperationPlan> carry_operations{};
    // Only feedback branches that cannot alias the producer home appear here.
    std::vector<SampleFeedbackOperationPlan> feedback_operations{};

    // One exact range per transient representation. The arena high-water mark
    // is independent of any individual representation's maximum size.
    std::vector<SampleTransientAllocationPlan> transient_allocations{};
    std::size_t transient_arena_size = 0;
    std::size_t transient_arena_alignment = 1;
    NodeLayout::RegionHandle transient_region{};

    // Persistent allocations never alias. They are separate raw regions so the
    // canonical NodeLayout can carry stable migration identities per logical
    // producer representation.
    std::vector<SamplePersistentAllocationPlan> persistent_allocations{};

    [[nodiscard]] bool empty() const noexcept
    {
        return representations.empty();
    }
};

// Pure host-side physical-representation planning. This consumes already-made
// choose_sample_connection_implementation() decisions; it does not duplicate
// policy. compact carry and persistent ring are realized as distinct physical
// representations while converted fanout remains explicit transient materialization.
std::expected<SamplePhysicalPlan, std::string> build_sample_physical_plan(
    ConnectionAnalysisPlan const& connections,
    std::size_t kernel_block_size);

// Reserve canonical NodeStorage for transient scratch and persistent connection
// state. No C++ objects are constructed. Persistent feedback rings with authored
// initial values install raw-region initialization callbacks, so realtime
// execution performs no setup/allocation.
std::expected<void, std::string> declare_sample_physical_storage(
    NodeLayoutBuilder& builder,
    SamplePhysicalPlan& plan);

std::expected<void, std::string> finalize_sample_physical_storage(
    NodeLayout const& layout,
    SamplePhysicalPlan& plan);

} // namespace iv::graph_jit::detail
