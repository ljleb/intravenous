#pragma once

// ABI shared by a module build and the finalizer.  A record is emitted once
// for each node type used by a translation unit; its key has no meaning after
// that build has been finalized.

#include <intravenous/node/code_key.h>

#include <cstddef>

namespace iv {

struct NodeLayoutBuilder;
struct NodeStateStructures;
struct ReflectedNodeTickContext;

namespace details {

// The compiler record contains only code selected by the node type. The
// transitional executor adapter lives in graph/reflected_node_operations.h.
struct NodeCompilerOperations {
    std::size_t (*declare_node)(
        void const*, NodeStateStructures const*, NodeLayoutBuilder&) = nullptr;
    void (*tick_block)(
        void const*, ReflectedNodeTickContext const&, std::size_t, std::size_t) = nullptr;
    void (*skip_block)(
        void const*, ReflectedNodeTickContext const&, std::size_t, std::size_t) = nullptr;

    // Compiler-facing one-node indexed callback anchors. The opaque context
    // points to the corresponding Node-specialized public context. Whole-
    // project lowering imports and specializes these stable LLVM entry points;
    // future multi-node batching uses a separate ABI.
    void (*tock_coverage)(void const*, void*) = nullptr;
    void (*propagate_forward_coverage)(void const*, void*) = nullptr;
    void (*propagate_reverse_coverage)(void const*, void*) = nullptr;

    constexpr bool valid() const
    {
        return declare_node != nullptr && tick_block != nullptr
            && skip_block != nullptr;
    }
};

struct NodeCompilerRecord {
    NodeCodeKey code_key {};
    NodeCompilerOperations operations {};
    char const* type_name = nullptr;
    std::size_t type_name_size = 0;
    // Finalizer ABI data.  This independently verifies the State metadata
    // received from Clang before publishing it to the graph compiler.
    std::size_t state_size = 0;
    std::size_t state_alignment = 1;
    // IndexedState is non-semantic acceleration state available only to
    // tock_coverage(). It is allocated independently from sequential State.
    std::size_t indexed_state_size = 0;
    std::size_t indexed_state_alignment = 1;
    // Authored and statically validated; the project planner must separately
    // prove that a specific instance can replay its transitive dependencies.
    bool intrinsically_replayable = false;
};

} // namespace details
} // namespace iv
