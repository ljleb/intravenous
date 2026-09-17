#pragma once

// Transitional adapter from a compiler-selected node implementation to the
// current reflected executor. It is deliberately separate from the module /
// finalizer compiler-record boundary so the runtime model can change without
// widening that record.

#include <intravenous/node/compiler_record.h>
#include <intravenous/node/compiled_port_context.h>
#include <intravenous/ports.h>

#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>

namespace iv {

// Compiler-facing span ABI. Whole-project LLVM may materialize these fields
// directly, so their object representation is explicit rather than inheriting
// an implementation-defined std::span layout. The implicit conversion keeps
// the reflected adapter compatible with the ordinary TickContext interface.
template<typename T>
struct ReflectedSpan {
    T* pointer = nullptr;
    std::size_t extent = 0;

    constexpr ReflectedSpan() noexcept = default;
    constexpr ReflectedSpan(std::span<T> value) noexcept
        : pointer(value.data())
        , extent(value.size())
    {}

    template<typename Range>
        requires requires(Range&& range) {
            std::span<T>(std::forward<Range>(range));
        }
    constexpr ReflectedSpan(Range&& range)
        noexcept(noexcept(std::span<T>(std::forward<Range>(range))))
        : ReflectedSpan(std::span<T>(std::forward<Range>(range)))
    {}

    [[nodiscard]] constexpr T* data() const noexcept { return pointer; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return extent; }
    [[nodiscard]] constexpr bool empty() const noexcept { return extent == 0; }

    constexpr operator std::span<T>() const noexcept
    {
        return {pointer, extent};
    }
};

static_assert(std::is_standard_layout_v<ReflectedSpan<std::byte>>);
static_assert(std::is_trivially_copyable_v<ReflectedSpan<std::byte>>);

struct ReflectedNodeTickContext {
    ReflectedSpan<InputPort> inputs {};
    ReflectedSpan<OutputPort> outputs {};
    ReflectedSpan<EventInputPort> event_inputs {};
    ReflectedSpan<EventOutputPort> event_outputs {};
    ReflectedSpan<CompiledInputPort const> compiled_inputs {};
    ReflectedSpan<CompiledEventInputPort const> compiled_event_inputs {};
    ReflectedSpan<std::byte> compiled_state {};
    std::size_t sample_rate = 48000;
    std::size_t scc_feedback_latency = 0;
    ReflectedSpan<std::byte> state {};
};

static_assert(std::is_standard_layout_v<ReflectedNodeTickContext>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeTickContext>);

struct ReflectedNodeRuntimeOperations {
    void const* node_data = nullptr;
    NodeStateStructures const* state_structures = nullptr;
    std::size_t (*declare_node)(
        void const*, NodeStateStructures const*, NodeLayoutBuilder&) = nullptr;
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
    NodeStateStructures const* state_structures = nullptr)
{
    return {
        .node_data = node_data,
        .state_structures = state_structures,
        .declare_node = record.operations.declare_node,
        .tick_block = record.operations.tick_block,
        .skip_block = record.operations.skip_block,
    };
}

} // namespace details
} // namespace iv
