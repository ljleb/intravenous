#pragma once

#include <intravenous/graph/realtime_port_planning.h>
#include <intravenous/graph_jit/connection_plan.h>
#include <intravenous/node/layout.h>
#include <intravenous/ports.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace iv::graph_jit::detail {

struct GraphAnalysis;

// Points 10-11 realize direct/transient realtime event flow plus bounded
// compact carry and persistent retained rings. Ordinary ports store a
// count plus a bounded TimedEvent sequence. Large retained windows bind producer
// and identity consumers directly to one migration-identified persistent ring
// carrying monotonic read/write indices, so retained events are expired
// incrementally rather than copied through a compact tail each root call.
// Retained canonical ports may also feed windowed transient
// materializations for sliced/converted consumer branches. The canonical
// producer storage reserves one overflow counter per logical event
// output; derived fanout ports never duplicate producer telemetry.
// Primitive callbacks receive only immutable bindings and reconstruct
// invocation-local EventInputPort/EventOutputPort facades.
inline constexpr std::size_t no_event_transient_allocation =
    std::numeric_limits<std::size_t>::max();

struct EventRepresentationPlan {
    std::size_t producer_group_index = 0;
    EventTypeId type = EventTypeId::empty;
    std::size_t event_capacity = 0;
    // A zero-capacity sequence whose count is zero in plan-owned
    // read-only data. Every disconnected Sequential input of this type binds
    // to it; it needs neither stack bytes nor per-invocation resets.
    bool immutable_empty = false;
    bool persistent = false;
    bool persistent_ring = false;
    // Empty means generation-local persistent storage. A non-empty value is
    // used only for exact-shape NodeStorage migration.
    std::string migration_identity{};
    bool has_producer_overflow_counter = false;
    std::size_t count_relative_offset = 0;
    std::size_t read_index_relative_offset = 0;
    std::size_t write_index_relative_offset = 0;
    // Persistent Sequential event storage owns one transport mapping. The
    // initialized byte distinguishes a genuine expected position of zero from
    // storage which has not completed an invocation yet.
    bool has_sequential_timeline = false;
    std::size_t timeline_initialized_relative_offset = 0;
    std::size_t expected_next_time_relative_offset = 0;
    std::size_t events_relative_offset = 0;
    // Ordered fan-in carries one compiler-only semantic source index beside
    // each TimedEvent. Staged fan-in needs it to preserve equal-time ordering
    // across execution regions; retained fan-in also needs it when individual
    // producers own history/latency state. Authored port facades still see only
    // TimedEvent.
    bool has_source_indices = false;
    std::size_t source_indices_relative_offset = 0;
    std::size_t size_bytes = 0;
    std::size_t alignment = 1;
    std::size_t transient_allocation = no_event_transient_allocation;
    NodeLayout::RegionHandle region{};
    NodeLayout::RegionHandle overflow_region{};
    // Canonical NodeStorage offsets exist only for persistent ports
    // and producer telemetry. Transient storage addresses come from the
    // realtime root's event arena plus region_relative_offset.
    std::size_t storage_offset = 0;
    std::size_t overflow_count_storage_offset = 0;
};

struct EventTransientAllocationPlan {
    std::size_t representation_index = 0;
    std::size_t size_bytes = 0;
    std::size_t alignment = 1;
    std::size_t region_relative_offset = 0;
    // Inclusive flattened schedule positions where this stack buffer may be
    // accessed. This is compile-time planning data only.
    ConnectionLiveIntervalPlan live_interval{};
};

struct PrimitiveEventInputBindingPlan {
    // Callback-visible sequence.
    std::optional<std::size_t> storage{};
    // Background evaluation supplies this Sequential input's current window
    // through its Sequential Tick input slot on every invocation. Without
    // history storage is empty and the realtime root binds the slot
    // directly. With history, storage is the input's node-owned persistent
    // timeline ring: the root inserts each delivered window into it before
    // the callback, so its history is the input's own record of earlier
    // deliveries.
    bool invocation_supplied = false;
    // Mixed delivery keeps the realtime contribution in
    // realtime_operand_storage and receives the background contribution
    // through TickInvocationCall. The realtime root merges both into
    // combined_storage, then inserts that merge's current window into the
    // timeline that storage names.
    bool combines_invocation_supplied = false;
    std::optional<std::size_t> realtime_operand_storage{};
    std::optional<std::size_t> combined_storage{};
    // A single realtime source needs no compiler sidecar. This retains its
    // EventConnectionPlan::sources position for the final ordered merge.
    std::optional<std::size_t> realtime_semantic_source_index{};
    // History retained by the timeline ring. Window insertion orders events
    // from the beginning of that history.
    std::size_t timeline_history_samples = 0;
    // See PrimitiveSampleInputBindingPlan::random_access.
    bool random_access = false;
};

struct PrimitiveEventOutputBindingPlan {
    // Entry positions correspond to declared storage event-port indices. Only
    // realtime entries are bound into ReflectedNodeTickContext.
    bool realtime = true;
    // Callback-facing storage. A retained or sliced output can use a separate
    // canonical_storage so the callback writes only its current invocation.
    std::optional<std::size_t> storage{};
    std::optional<std::size_t> canonical_storage{};
    EventTypeId source_type = EventTypeId::empty;
    std::size_t history = 0;
    std::size_t latency = 0;
    bool append_existing = false;
};

enum class EventOperationScopeKind : std::uint8_t {
    primitive,
    region,
};

enum class EventOperationPhase : std::uint8_t {
    before,
    after,
};

// Event operations are planned against the invocation whose index/block-size
// define their semantic window. A primitive scope executes for every SCC slice;
// a region scope executes once around the complete root-call region. Keeping
// this in the storage plan prevents execution planning from rediscovering a
// materialization's consumers and guessing which window it meant.
struct EventOperationScope {
    EventOperationScopeKind kind = EventOperationScopeKind::primitive;
    EventOperationPhase phase = EventOperationPhase::after;
    std::size_t index = 0;

    friend bool operator==(EventOperationScope const&, EventOperationScope const&)
        = default;
};

struct EventMaterializationPlan {
    std::size_t source_representation = 0;
    std::size_t target_representation = 0;
    EventConversionPlan conversion{};
    // A value selects only events authored by one semantic source from an
    // ordered fan-in representation. The selected stream no longer needs a
    // source-index sidecar.
    std::optional<std::size_t> source_index_filter{};
    // Keep the source representation's semantic-source sidecar parallel to
    // the converted target. Mixed Tick/Tock fan-in needs this information for
    // the final merge with the invocation-supplied contribution.
    bool preserve_source_indices = false;
    // Aggregate or retained canonical sources may contain events outside the
    // consumer invocation currently being materialized. Windowed selection uses
    // the index/block_size of the operation's scope: a cyclic step-local
    // materialization therefore selects one SCC slice, while a region-exit
    // materialization selects the complete root call. In both cases the window
    // is [index-history_samples, index+block_size) before conversion. Target
    // capacity remains source-capacity-sized because max_events_per_index is
    // only a storage-sizing rate, not a runtime density constraint.
    std::size_t history_samples = 0;
    // Exact unrounded upper bound for events written by this one shared
    // operation. Costing uses the operation once regardless of fanout count.
    std::size_t maximum_output_event_count = 0;
    // Total source events examined across every execution of this operation in
    // one root call. This differs from output count for windowed SCC
    // materialization and is charged only when the source candidate is a ring.
    std::size_t maximum_source_event_reads = 0;
    bool select_invocation_window = false;
    bool before_merge = false;
    EventOperationScope scope{};
};

struct EventCarryPlan {
    std::size_t working_representation = 0;
    std::size_t persistent_representation = 0;
    // Staged SCC fan-in can restore before its first stage and commit after its
    // last stage. Other carry operations normally use one producer scope.
    EventOperationScope restore_scope{};
    EventOperationScope commit_scope{};
    std::size_t retained_history_samples = 0;
    std::size_t retained_latency_samples = 0;
};

struct EventPersistentRingPlan {
    std::size_t storage = 0;
    EventOperationScope prune_scope{};
    std::size_t retained_history_samples = 0;
};

struct EventSequenceResetPlan {
    std::size_t storage = 0;
    EventOperationScope scope{};
};

struct EventMergePlan {
    // Sorted callback-output streams are merged in semantic source order after
    // the producers for this scope have completed. target_representation is a
    // separate fan-in sequence. It never aliases a concrete EventOutputPortId
    // raw representation.
    std::vector<std::size_t> source_representations{};
    // Empty for an unindexed stable merge. Ordered fan-in supplies one semantic
    // source index per source storage; the target owns a parallel sidecar used
    // only by compiler runtime helpers.
    std::vector<std::size_t> source_indices{};
    // Empty, or one value per source: the target stores each event of that
    // source this many positions after its own timestamp. A latent Tick
    // output's callback authors events at their own positions, and its
    // retained streams hold each at the position where it becomes final, its
    // position plus the output's latency.
    std::vector<std::size_t> source_delays{};
    std::size_t target_representation = 0;
    EventOperationScope scope{};
    // Every input stream is sorted in forward circular order beginning at the
    // root invocation's history-inclusive aggregate origin. That origin stays
    // fixed while an SCC contributes several slices to the same sequence.
    std::size_t ordering_history_samples = 0;
    // Retained targets already contain restored/pruned events from previous
    // invocations. Staged SCC fan-in also retains events from earlier slices
    // of the current root invocation.
    bool preserve_existing_target = false;
    // Canonical EventOutputPortId storage must receive the callback-local
    // sequence before any source conversion or connection fan-in reads it.
    bool before_materialization = false;
};

struct EventFeedbackPlan {
    std::size_t source_representation = 0;
    std::size_t target_representation = 0;
    std::optional<std::size_t> source_index_filter{};
    // A callback-local source sequence is cleared and populated again for
    // each primitive invocation, and holds each event at its own position.
    // Feedback therefore consumes the complete sequence at every append scope,
    // delayed by the source latency plus the detach latency
    // (loop_extra_latency includes both).
    bool source_sequence_restarts_each_append = false;
    // The source is a retained stream that holds each event where it becomes
    // final. Each append copies exactly the events that became final during
    // the current invocation, [index, index + block_size), so every event is
    // delivered once even when a latent producer adds it among events it
    // authored earlier.
    bool selects_final_window = false;
    EventOperationScope append_scope{};
    EventOperationScope reset_scope{};
    EventConnectionStorageRequirements storage_requirements{};
    EventConnectionStoragePlan storage_plan{};
    // Exact unrounded bounds for producer -> delayed-stream writes and the
    // retained tail. Compact carry restore/commit work is derived from the
    // latter by the shared storage cost model.
    std::size_t authored_event_count = 0;
    std::size_t source_event_reads = 0;
    std::size_t retained_event_count = 0;
    std::size_t retained_window_samples = 0;
    // Source and target sequences are ordered from the retained-history
    // boundary, not necessarily from the current invocation index.
    std::size_t ordering_history_samples = 0;
    std::size_t loop_extra_latency = 1;
};

struct PrimitiveEventPortPlan {
    std::vector<PrimitiveEventInputBindingPlan> inputs{};
    std::vector<PrimitiveEventOutputBindingPlan> outputs{};
};

struct EventPortBindingPlan {
    // Entry positions correspond to
    // ConnectionAnalysisPlan::event_producer_groups. These are the final
    // lowering decisions after concrete producer buffers and operations are known;
    // connection analysis carries only the temporal requirements.
    std::vector<std::optional<EventConnectionStoragePlan>>
        producer_group_storage_plans{};
    std::vector<std::optional<std::size_t>> producer_group_representations{};
    // Per-source representations consumed by each producer-group merge.
    // A detached source reads this representation before the group merge,
    // rather than selecting the source from the already-sorted group result.
    std::vector<std::vector<std::optional<std::size_t>>>
        producer_group_source_representations{};
    std::vector<EventRepresentationPlan> ports{};
    std::vector<EventMaterializationPlan> materializations{};
    std::vector<EventCarryPlan> carry_operations{};
    std::vector<EventPersistentRingPlan> persistent_rings{};
    std::vector<EventSequenceResetPlan> sequence_resets{};
    std::vector<EventMergePlan> merges{};
    std::vector<EventFeedbackPlan> feedback_operations{};
    std::vector<EventTransientAllocationPlan> transient_allocations{};
    std::size_t transient_arena_size = 0;
    std::size_t transient_arena_alignment = 1;
    bool persistent_storage_crosses_tick_partition = false;
    // One entry per analyzed concrete primitive.
    std::vector<PrimitiveEventPortPlan> primitives{};
};

enum class EventOperationKind : std::uint8_t {
    sequence_reset,
    persistent_ring_prune,
    carry_restore,
    merge,
    materialize,
    feedback_append,
    carry_commit,
};

struct EventOperationRef {
    EventOperationKind kind = EventOperationKind::sequence_reset;
    std::size_t index = 0;
};

std::expected<EventPortBindingPlan, std::string> plan_event_ports(
    ConfiguredGraph const& graph,
    std::size_t block_size,
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections,
    ResolvedPortRealization const& realization,
    RealtimeStorageCostModel const& cost_model);

} // namespace iv::graph_jit::detail
