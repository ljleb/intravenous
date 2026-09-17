#pragma once

#include <intravenous/ports.h>

#include <cstddef>

namespace iv::graph_jit::detail {

inline constexpr char event_carry_restore_symbol[] =
    "iv_graph_jit_restore_event_carry";
inline constexpr char event_carry_commit_symbol[] =
    "iv_graph_jit_commit_event_carry";

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

#undef IV_GRAPH_JIT_RETENTION_RUNTIME_EXPORT

} // namespace iv::graph_jit::detail
