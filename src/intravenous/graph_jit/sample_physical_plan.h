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
// objects. Storage residence is independent from the conversion, composition,
// or feedback operations which read and write the representation.
struct SampleRepresentationPlan {
    std::size_t producer_group_index = 0;
    bool canonical_producer_representation = true;
    RealtimeBufferStorageKind storage =
        RealtimeBufferStorageKind::transient_stack;
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

struct SampleCompositionInputPlan {
    std::size_t source_representation = no_sample_representation;
    std::size_t source_channel = 0;
    std::size_t read_latency = 0;
};

// One normalized gather -> semantic conversion -> projection contribution.
// sources are ordered by source_layout's semantic channels; target_channels
// map converted semantic channels into the final target-port representation.
struct SampleCompositionContributionPlan {
    ChannelLayout source_layout{};
    ChannelLayout converted_layout{};
    std::vector<SampleCompositionInputPlan> sources{};
    std::vector<std::size_t> target_channels{};

    // Unequal-latency channel mixing cannot be emitted by independently
    // shifting converted target writes: conversion must observe source
    // channels at one logical timestamp. Detached contributions that need
    // this use a branch-local persistent source-layout ring. Current source
    // samples are staged there, then conversion reads the required aligned
    // past frames before writing the feedback timeline.
    std::size_t feedback_alignment_representation = no_sample_representation;
    std::size_t feedback_alignment_write_latency = 0;
};

// A feed-forward composed connection gathers independently-timed semantic
// source channels, applies each configured channel-count/layout conversion,
// and projects the converted channels into one target-layout transient
// representation. It writes a timestamp-aligned window, so its eventual
// InputPort binding reads with zero additional latency and target history is
// reconstructed while composition runs. Detached composition is represented
// uniformly by SampleFeedbackTimelinePlan below instead of a special mode here.
struct SampleCompositionPlan {
    std::size_t connection_index = 0;
    std::vector<SampleCompositionContributionPlan> contributions{};
    std::size_t target_representation = no_sample_representation;
    std::size_t after_execution_position = 0;
    ChannelLayout target_layout{};
    std::size_t target_history = 0;
};

// Exact transient byte range assigned to one representation. Ranges may overlap
// iff their inclusive schedule live intervals do not overlap. There is no
// runtime slot object or allocator metadata.
struct SampleTransientAllocationPlan {
    std::size_t representation_index = no_sample_representation;
    std::size_t size_bytes = 0;
    std::size_t alignment = alignof(Sample);
    std::size_t region_relative_offset = 0;
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

// stack_with_persistent_carry uses a transient absolute-indexed working ring
// while a minimal persistent tail crosses root invocations. The tail is restored
// immediately before the producer and committed after its materializations.
struct SampleCarryOperationPlan {
    std::size_t representation_index = no_sample_representation;
    std::size_t persistent_allocation = no_sample_persistent_allocation;
    std::size_t producer_execution_position = 0;
    std::size_t retained_frames = 0;
};

enum class SampleFeedbackTimelineWriterKind {
    // The primitive output binding names the feedback timeline representation
    // directly. No post-producer operation is emitted.
    producer_home,
    // Copy the newly-produced canonical slice into the branch-local timeline.
    copy,
    // Gather projected/permuted source channels into the target-layout timeline,
    // shifting each current source write forward by its ordinary read latency.
    composition,
};

// One write strategy for a detached branch timeline. Every feedback branch is
// represented by the same persistent-timeline abstraction regardless of how
// current samples arrive in it; consumer-side conversion/materialization then
// reads uniformly from timeline_representation.
struct SampleFeedbackTimelineWriterPlan {
    SampleFeedbackTimelineWriterKind kind =
        SampleFeedbackTimelineWriterKind::producer_home;
    std::size_t after_execution_position = 0;

    // Number of already-authored source frames that must be recopied on every
    // invocation because OutputPort::update() may revise any of them. This is
    // the producer's authored latency horizon, not the consumer read latency.
    std::size_t revision_frames = 0;

    // copy only
    std::size_t source_representation = no_sample_representation;

    // composition only
    std::vector<SampleCompositionContributionPlan> composition_contributions{};
};


struct SampleFeedbackTimelinePlan {
    std::size_t connection_index = 0;
    std::size_t timeline_representation = no_sample_representation;
    ChannelLayout channel_layout{};
    std::size_t retained_frames = 0;
    std::size_t loop_extra_latency = 1;
    Sample initial_value{};
    SampleFeedbackTimelineWriterPlan writer{};
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
    // Feed-forward channel compositions only. Detached channel composition is
    // a SampleFeedbackTimelinePlan with a composition writer.
    std::vector<SampleCompositionPlan> compositions{};
    std::vector<SampleCarryOperationPlan> carry_operations{};
    // One entry per detached sample connection. producer_home timelines need no
    // scheduled write; copy/composition writers are emitted after their source
    // execution position.
    std::vector<SampleFeedbackTimelinePlan> feedback_timelines{};
    // Unequal-latency feedback mixing uses ordinary persistent sample
    // allocations as source-layout alignment rings. Their initialized contents
    // are the branch prehistory; no separate validity/warmup state is needed.

    // One exact range per transient representation. The arena high-water mark
    // is independent of any individual representation's maximum size.
    std::vector<SampleTransientAllocationPlan> transient_allocations{};
    std::size_t transient_arena_size = 0;
    std::size_t transient_arena_alignment = 1;

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
// choose_sample_connection_storage_plan() decisions; it does not duplicate
// policy. Carry and full persistent storage are realized as distinct physical
// representations while conversion remains an explicit operation.
std::expected<SamplePhysicalPlan, std::string> build_sample_physical_plan(
    ConnectionAnalysisPlan const& connections,
    std::size_t kernel_block_size);

// Reserve canonical NodeStorage for persistent connection state. Transient
// backing belongs to the generated root stack. Persistent feedback rings with
// authored initial values install raw-region initialization callbacks, so
// realtime execution performs no setup/allocation.
std::expected<void, std::string> declare_sample_physical_storage(
    NodeLayoutBuilder& builder,
    SamplePhysicalPlan& plan);

std::expected<void, std::string> finalize_sample_physical_storage(
    NodeLayout const& layout,
    SamplePhysicalPlan& plan);

} // namespace iv::graph_jit::detail
