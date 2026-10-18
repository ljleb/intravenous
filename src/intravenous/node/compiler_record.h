#pragma once

// ABI shared by a module build and the finalizer.  A record is emitted once
// for each node type used by a translation unit; its key has no meaning after
// that build has been finalized.

#include <intravenous/node/code_key.h>
#include <intravenous/global_index.h>

#include <cstddef>
#include <cstdint>

namespace iv {

struct NodeLayoutBuilder;
struct NodeStateStructures;
struct ReflectedNodeTickContext;
struct ReflectedNodePortRealization;
struct ReflectedNodeTockCoverageContext;
struct ReflectedNodeForwardCoverageContext;
struct ReflectedNodeReverseCoverageContext;
enum class ContinuousValueTypeId : std::uint8_t;

namespace details {

enum class SequentialInputInitialization : std::uint8_t {
    disconnected,
    neutral,
};

// The compiler record contains only code selected by the node type. The
// compiler-facing reflected execution ABI lives in
// graph/reflected_node_operations.h.
struct NodeCompilerOperations {
    std::size_t (*declare_node)(
        void const*,
        NodeStateStructures const*,
        NodeLayoutBuilder&,
        ReflectedNodePortRealization const&) = nullptr;
    void (*tick_block)(
        void const*, ReflectedNodeTickContext const&, GlobalIndex, std::size_t) = nullptr;
    void (*skip_block)(
        void const*, ReflectedNodeTickContext const&, GlobalIndex, std::size_t) = nullptr;

    // Compiler-facing one-node background callback anchors. Reflected contexts use
    // explicit pointer/count spans; wrappers reconstruct the Node-specialized
    // public contexts before entering authored code. Whole-project batching is
    // a separate root ABI.
    void (*tock_coverage)(
        void const*, ReflectedNodeTockCoverageContext const&) = nullptr;
    void (*propagate_forward_coverage)(
        void const*, ReflectedNodeForwardCoverageContext const&) = nullptr;
    void (*propagate_reverse_coverage)(
        void const*, ReflectedNodeReverseCoverageContext const&) = nullptr;

    // Executable Sequential fan-in semantics stay in package LLVM. The
    // configured graph serializes declarative metadata, never native function
    // pointers. These node-specialized dispatchers accept only one of the
    // node's declared continuous input/type pairs and reject every other pair.
    bool (*initialize_sequential_input)(
        std::size_t continuous_input,
        ContinuousValueTypeId,
        SequentialInputInitialization,
        std::byte* destination,
        std::size_t destination_bytes,
        std::size_t elements_per_value) = nullptr;
    bool (*combine_sequential_input_values)(
        std::size_t continuous_input,
        ContinuousValueTypeId,
        std::byte* destination,
        std::byte const* source,
        std::size_t byte_count,
        std::size_t elements_per_value) = nullptr;

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
    // TockState is non-semantic acceleration state available only to
    // tock_coverage(). It is allocated independently from sequential State.
    std::size_t background_state_size = 0;
    std::size_t background_state_alignment = 1;
    // Authored and statically validated; the project planner must separately
    // prove that a specific instance can replay its transitive dependencies.
    bool intrinsically_replayable = false;
    // Pointwise tick() can only be realized when every relevant Sequential
    // input and Tick output uses one equal absolute pace. Native tick_block()
    // nodes may observe heterogeneous per-port projections directly.
    bool pointwise_tick = false;
    // True when an authored tick_block() or skip_block() callback can request
    // a complete contiguous input block. A pointwise callback can request
    // block_extended() as history plus its one current value; the storage
    // planner accounts for that separately when resolved history is nonzero.
    bool block_input_access = false;
};

} // namespace details
} // namespace iv
