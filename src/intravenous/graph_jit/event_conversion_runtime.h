#pragma once

#include <intravenous/ports.h>

#include <cstddef>
#include <cstdint>

namespace iv::graph_jit::detail {

inline constexpr char event_sequence_conversion_symbol[] =
    "iv_graph_jit_convert_event_sequence";
inline constexpr char event_sequence_materialization_symbol[] =
    "iv_graph_jit_materialize_event_sequence";
inline constexpr char event_sequence_merge_symbol[] =
    "iv_graph_jit_merge_event_sequence";
inline constexpr char ordered_event_sequence_merge_symbol[] =
    "iv_graph_jit_merge_ordered_event_sequence";
inline constexpr char event_sequence_k_way_merge_symbol[] =
    "iv_graph_jit_merge_event_sequences_into_home";

#if defined(_WIN32)
#define IV_GRAPH_JIT_RUNTIME_EXPORT __declspec(dllexport)
#else
#define IV_GRAPH_JIT_RUNTIME_EXPORT __attribute__((visibility("default")))
#endif

// Runtime leaf used by generated project code for event conversion. GraphJit
// computes the immutable conversion plan ahead of time; this function only
// applies those already-selected steps to one bounded raw event sequence.
extern "C" IV_GRAPH_JIT_RUNTIME_EXPORT std::size_t
iv_graph_jit_convert_event_sequence(
    std::uint32_t source_type,
    std::uint32_t target_type,
    std::uint32_t step0,
    std::uint32_t step1,
    std::uint32_t step2,
    std::size_t step_count,
    void const* source_events,
    std::size_t source_count,
    void* target_events,
    std::size_t target_capacity) noexcept;

// Stable in-place merge of one sorted producer-local sequence into an already
// sorted aggregate sequence/ring. Existing target events precede newly merged
// source events when timestamps are equal, so invoking this helper in semantic
// source order gives deterministic tie ordering.
extern "C" IV_GRAPH_JIT_RUNTIME_EXPORT std::size_t
iv_graph_jit_merge_event_sequence(
    void* target_events,
    std::size_t target_capacity,
    std::size_t target_read_index,
    std::size_t target_write_index,
    void const* source_events,
    std::size_t source_capacity,
    std::size_t source_count) noexcept;

// Stable in-place merge used by staged fan-in. source_indices is parallel to
// the target event ring/sequence; source_index identifies every event in the
// producer-local source. Ordering is lexicographic by (time, semantic source
// index), with existing events of the same source remaining first.
extern "C" IV_GRAPH_JIT_RUNTIME_EXPORT std::size_t
iv_graph_jit_merge_ordered_event_sequence(
    void* target_events,
    std::size_t* target_source_indices,
    std::size_t target_capacity,
    std::size_t target_read_index,
    std::size_t target_write_index,
    void const* source_events,
    std::size_t source_capacity,
    std::size_t source_count,
    std::size_t source_index) noexcept;



// Stable k-way merge for transient fan-in where semantic source 0 already
// resides in target storage. source_events/source_remaining describe semantic
// sources 1..N in order. All streams are required by the EventOutputPort
// contract to be nondecreasing by absolute sample index. The merge walks the
// streams backwards so it can write the final aggregate in place without
// allocating scratch event storage. Equal timestamps are ordered by semantic
// source index. source_remaining is caller-owned stack scratch and is
// decremented in place; no heap allocation occurs.
extern "C" IV_GRAPH_JIT_RUNTIME_EXPORT std::size_t
iv_graph_jit_merge_event_sequences_into_home(
    void* target_events,
    std::size_t target_capacity,
    std::size_t target_count,
    void const* const* source_events,
    std::size_t* source_remaining,
    std::size_t source_count) noexcept;

// Runtime leaf for retained-source materialization. The source is described by
// monotonic read/write indices over a power-of-two raw TimedEvent ring. Ordinary
// bounded sequences use read_index=0 and write_index=count. The helper selects
// the current root invocation plus declared target history before applying the
// immutable non-expanding conversion plan. Target capacity is intentionally not
// derived from the narrower selected window: max_events_per_index is a static
// sizing rate, not a runtime density constraint.
extern "C" IV_GRAPH_JIT_RUNTIME_EXPORT std::size_t
iv_graph_jit_materialize_event_sequence(
    std::uint32_t source_type,
    std::uint32_t target_type,
    std::uint32_t step0,
    std::uint32_t step1,
    std::uint32_t step2,
    std::size_t step_count,
    void const* source_events,
    std::size_t source_capacity,
    std::size_t source_read_index,
    std::size_t source_write_index,
    SampleIndex block_index,
    std::size_t block_size,
    std::size_t history_samples,
    void* target_events,
    std::size_t target_capacity) noexcept;

#undef IV_GRAPH_JIT_RUNTIME_EXPORT

} // namespace iv::graph_jit::detail
