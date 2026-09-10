#pragma once

// Transitional adapter from a compiler-selected node implementation to the
// current reflected executor. It is deliberately separate from the module /
// finalizer compiler-record boundary so the runtime model can change without
// widening that record.

#include <intravenous/node/compiler_record.h>
#include <intravenous/ports.h>

#include <cstddef>
#include <span>

namespace iv {

struct ReflectedNodeTickContext {
    std::span<InputPort> inputs {};
    std::span<OutputPort> outputs {};
    std::span<EventInputPort> event_inputs {};
    std::span<EventOutputPort> event_outputs {};
    std::size_t sample_rate = 48000;
    std::size_t scc_feedback_latency = 0;
    std::span<std::byte> state {};
};

struct ReflectedNodeRuntimeOperations {
    void const* node_data = nullptr;
    NodeStateStructure const* state_structure = nullptr;
    std::size_t (*declare_node)(
        void const*, NodeStateStructure const*, NodeLayoutBuilder&) = nullptr;
    void (*tick_block)(
        void const*, ReflectedNodeTickContext const&, std::size_t, std::size_t) = nullptr;
    void (*skip_block)(
        void const*, ReflectedNodeTickContext const&, std::size_t, std::size_t) = nullptr;

    constexpr bool valid() const
    {
        return declare_node != nullptr && tick_block != nullptr
            && skip_block != nullptr;
    }
};

struct ReflectedNodeOperations {
    ReflectedNodeRuntimeOperations runtime {};

    constexpr bool valid() const
    {
        return runtime.valid();
    }
};

namespace details {

constexpr ReflectedNodeRuntimeOperations make_runtime_operations(
    NodeCompilerRecord const& record,
    void const* node_data,
    NodeStateStructure const* state_structure = nullptr)
{
    return {
        .node_data = node_data,
        .state_structure = state_structure,
        .declare_node = record.operations.declare_node,
        .tick_block = record.operations.tick_block,
        .skip_block = record.operations.skip_block,
    };
}

} // namespace details
} // namespace iv
