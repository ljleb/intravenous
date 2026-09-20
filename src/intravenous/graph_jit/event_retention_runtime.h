#pragma once

#include <intravenous/ports.h>

#include <cstddef>

namespace iv::graph_jit::detail {

inline constexpr char event_carry_restore_symbol[] =
    "iv_graph_jit_restore_event_carry";
inline constexpr char event_carry_commit_symbol[] =
    "iv_graph_jit_commit_event_carry";
inline constexpr char event_persistent_ring_prune_symbol[] =
    "iv_graph_jit_prune_event_persistent_ring";
inline constexpr char event_feedback_append_symbol[] =
    "iv_graph_jit_append_event_feedback";
inline constexpr char event_feedback_append_ring_source_symbol[] =
    "iv_graph_jit_append_event_feedback_ring_source";
inline constexpr char event_feedback_append_sequence_symbol[] =
    "iv_graph_jit_append_event_feedback_sequence";
inline constexpr char event_feedback_append_sequence_ring_source_symbol[] =
    "iv_graph_jit_append_event_feedback_sequence_ring_source";

#if defined(_WIN32)
#define IV_GRAPH_JIT_RETENTION_RUNTIME_EXPORT __declspec(dllexport)
#else
#define IV_GRAPH_JIT_RETENTION_RUNTIME_EXPORT __attribute__((visibility("default")))
#endif

// Raw bounded-sequence helpers used by generated project code. The persistent
// carry stores only TimedEvent payloads plus its count word; no EventPort or
// compatibility runtime objects cross root invocations.
extern "C" IV_GRAPH_JIT_RETENTION_RUNTIME_EXPORT std::size_t
iv_graph_jit_restore_event_carry(
    void const* carry_events,
    std::size_t carry_count,
    void* working_events,
    std::size_t working_capacity) noexcept;

extern "C" IV_GRAPH_JIT_RETENTION_RUNTIME_EXPORT std::size_t
iv_graph_jit_commit_event_carry(
    void const* working_events,
    std::size_t working_count,
    std::size_t sample_index,
    std::size_t block_size,
    std::size_t retained_history_samples,
    std::size_t retained_latency_samples,
    void* carry_events,
    std::size_t carry_capacity) noexcept;

// Advance only the persistent ring's oldest retained index. Producer and
// consumers continue to share the same TimedEvent storage directly; no retained
// payload bytes are copied at the root-call boundary.
extern "C" IV_GRAPH_JIT_RETENTION_RUNTIME_EXPORT std::size_t
iv_graph_jit_prune_event_persistent_ring(
    void const* ring_events,
    std::size_t ring_capacity,
    std::size_t read_index,
    std::size_t write_index,
    std::size_t sample_index,
    std::size_t retained_history_samples) noexcept;

// Append only the newly-produced suffix of one aggregate SCC event sequence.
// Before appending, retire feedback events older than the current slice start.
// Consumers run before the semantic producer in same-slice order and therefore
// observe the old ring indices; pruning here avoids a separate hot-path helper
// call without changing their visible window.
extern "C" IV_GRAPH_JIT_RETENTION_RUNTIME_EXPORT void
iv_graph_jit_append_event_feedback(
    void const* source_events,
    std::size_t source_begin_index,
    std::size_t source_end_index,
    std::size_t sample_index,
    std::size_t loop_extra_latency,
    void* ring_events,
    std::size_t ring_capacity,
    std::size_t* ring_read_index,
    std::size_t* ring_write_index) noexcept;

// Append a newly-authored delayed suffix to a linear bounded sequence. Compact
// feedback carry restores its retained prefix before SCC execution; this helper
// then extends that working sequence without introducing ring indices.
extern "C" IV_GRAPH_JIT_RETENTION_RUNTIME_EXPORT void
iv_graph_jit_append_event_feedback_sequence(
    void const* source_events,
    std::size_t source_begin_index,
    std::size_t source_end_index,
    std::size_t loop_extra_latency,
    void* target_events,
    std::size_t target_capacity,
    std::size_t* target_count) noexcept;

// Persistent producer rings expose monotonic source indices rather than a
// linear aggregate count. Copy the newly-authored [begin,end) suffix through
// the source ring mask into the selected feedback representation without
// materializing an intermediate linear sequence.
extern "C" IV_GRAPH_JIT_RETENTION_RUNTIME_EXPORT void
iv_graph_jit_append_event_feedback_sequence_ring_source(
    void const* source_events,
    std::size_t source_capacity,
    std::size_t source_begin_index,
    std::size_t source_end_index,
    std::size_t loop_extra_latency,
    void* target_events,
    std::size_t target_capacity,
    std::size_t* target_count) noexcept;

extern "C" IV_GRAPH_JIT_RETENTION_RUNTIME_EXPORT void
iv_graph_jit_append_event_feedback_ring_source(
    void const* source_events,
    std::size_t source_capacity,
    std::size_t source_begin_index,
    std::size_t source_end_index,
    std::size_t sample_index,
    std::size_t loop_extra_latency,
    void* ring_events,
    std::size_t ring_capacity,
    std::size_t* ring_read_index,
    std::size_t* ring_write_index) noexcept;

#undef IV_GRAPH_JIT_RETENTION_RUNTIME_EXPORT

} // namespace iv::graph_jit::detail
