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

// Append only the producer events belonging to the current SCC slice into a
// persistent feedback ring. Timestamps are shifted by the authored detach
// latency; SCC scheduling latency is deliberately not part of this transport
// operation. Returns the updated monotonic write index.
extern "C" IV_GRAPH_JIT_RETENTION_RUNTIME_EXPORT std::size_t
iv_graph_jit_append_event_feedback(
    void const* source_events,
    std::size_t source_count,
    std::size_t sample_index,
    std::size_t block_size,
    std::size_t loop_extra_latency,
    void* ring_events,
    std::size_t ring_capacity,
    std::size_t read_index,
    std::size_t write_index) noexcept;

#undef IV_GRAPH_JIT_RETENTION_RUNTIME_EXPORT

} // namespace iv::graph_jit::detail
