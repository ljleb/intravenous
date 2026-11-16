#pragma once

#include <intravenous/graph_jit/module_input_resolution_runtime.h>

namespace iv::graph_jit::detail {
// Compiler-owned read-only sink. Its one Sequential input uses ordinary source
// routing/storage planning. The root performs the requested reduction while
// that input binding is valid; this callback has no authored DSP or state.
inline void tick_observation_sink(void const*, ReflectedNodeTickContext const&,
    GlobalIndex, std::size_t) {}
inline void tick_observation_sink_batch(ReflectedNodeTickBatchContext const&) {}
inline std::size_t declare_observation_sink(void const*, NodeStateStructures const*,
    NodeLayoutBuilder& builder, ReflectedNodePortRealization const&)
{
    return iv::details::register_node(builder, {.node_type_name = "Output observation"});
}
inline bool initialize_observation_input(std::size_t port, ContinuousValueTypeId type,
    iv::details::SequentialInputInitialization, std::byte* destination,
    std::size_t bytes, std::size_t elements)
{
    if (port || type != ContinuousValueTypeId::sample || bytes != sizeof(Sample) || elements != 1) return false;
    std::memset(destination, 0, bytes);
    return true;
}
inline iv::details::NodeCompilerOperations observation_sink_operations()
{
    return {
        .declare_node = &declare_observation_sink,
        .tick_block = &tick_observation_sink,
        .tick_block_batch = &tick_observation_sink_batch,
        .skip_block = &tick_observation_sink,
        .skip_block_batch = &tick_observation_sink_batch,
        .combine_sequential_input_values = &combine_module_input_values,
    };
}
} // namespace iv::graph_jit::detail
