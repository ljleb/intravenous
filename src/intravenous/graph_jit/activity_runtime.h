#pragma once

#include <intravenous/graph/reflected_node_operations.h>

#include <cstddef>
#include <cstdint>

namespace iv::graph_jit::detail {

inline constexpr char continuous_input_activity_symbol[] =
    "iv_graph_jit_continuous_input_activity";
inline constexpr char continuous_output_clear_symbol[] =
    "iv_graph_jit_continuous_output_is_clear";
inline constexpr char event_input_activity_symbol[] =
    "iv_graph_jit_event_input_is_empty";
inline constexpr char event_output_activity_symbol[] =
    "iv_graph_jit_event_output_is_empty";

enum ContinuousInputActivity : std::uint32_t {
    continuous_input_constant = 1u << 0,
    continuous_input_unchanged = 1u << 1,
};

// Persistent compiler-owned state stored at the start of every enabled
// primitive activity region. Remembered continuous values follow at offsets
// selected by the lowering plan.
struct PrimitiveActivityState {
    std::size_t silent_samples = 0;
    std::uint8_t dormant = 0;
};

#if defined(_WIN32)
#define IV_GRAPH_JIT_ACTIVITY_EXPORT __declspec(dllexport)
#else
#define IV_GRAPH_JIT_ACTIVITY_EXPORT __attribute__((visibility("default")))
#endif

// Inspect one resolved continuous input channel. The operation stores a
// representative value only when the complete current block is constant.
// Static constant inputs do not call this helper.
extern "C" IV_GRAPH_JIT_ACTIVITY_EXPORT std::uint32_t
iv_graph_jit_continuous_input_activity(
    ReflectedContinuousInputPortBinding const* binding,
    std::size_t channel,
    std::size_t value_count,
    std::uint8_t* remembered_valid,
    void* remembered_value) noexcept;

extern "C" IV_GRAPH_JIT_ACTIVITY_EXPORT bool
iv_graph_jit_continuous_output_is_clear(
    ReflectedContinuousOutputPortBinding const* binding,
    std::size_t channel,
    std::size_t value_count) noexcept;

extern "C" IV_GRAPH_JIT_ACTIVITY_EXPORT bool
iv_graph_jit_event_input_is_empty(
    ReflectedEventInputPortBinding const* binding,
    GlobalIndex block_index,
    std::size_t block_size) noexcept;

extern "C" IV_GRAPH_JIT_ACTIVITY_EXPORT bool
iv_graph_jit_event_output_is_empty(
    ReflectedEventOutputPortBinding const* binding,
    GlobalIndex block_index,
    std::size_t block_size) noexcept;

#undef IV_GRAPH_JIT_ACTIVITY_EXPORT

} // namespace iv::graph_jit::detail
