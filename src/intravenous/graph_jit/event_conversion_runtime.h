#pragma once

#include <intravenous/ports.h>

#include <cstddef>
#include <cstdint>

namespace iv::graph_jit::detail {

inline constexpr char event_sequence_conversion_symbol[] =
    "iv_graph_jit_convert_event_sequence";

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

#undef IV_GRAPH_JIT_RUNTIME_EXPORT

} // namespace iv::graph_jit::detail
