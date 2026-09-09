#pragma once

// ABI shared by a module build and the finalizer.  A record is emitted once
// for each node type used by a translation unit; its key has no meaning after
// that build has been finalized.

#include <intravenous/node/code_key.h>
#include <intravenous/ports.h>

#include <cstddef>
#include <span>

namespace iv {

struct NodeLayoutBuilder;
struct NodeStateStructure;

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

struct NodeCompilerRecord {
    NodeCodeKey code_key {};
    ReflectedNodeRuntimeOperations runtime {};
    char const* type_name = nullptr;
    std::size_t type_name_size = 0;
    // Finalizer ABI data.  This independently verifies the State metadata
    // received from Clang before publishing it to the graph compiler.
    std::size_t state_size = 0;
    std::size_t state_alignment = 1;
};

} // namespace details
} // namespace iv
